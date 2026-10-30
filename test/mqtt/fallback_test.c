/* online: direct path, then the direct path dies (one side's UDP blocked) ->
 * both sides must fall back to the broker relay and keep exchanging. usage: fallback_test HOST:PORT a|b TRUST */
#include <jni.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include "../../android/net.h"
static int64_t now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec; }
static void pump(int64_t ns) { int64_t t = now(); while (now() - t < ns) { uint8_t b[64]; net_poll(now()); while (net_recv(b, sizeof b) > 0) {} net_wait(5000000); } }
static int exchange(const char *role, int base) {
    int sent = 0, got = 0, bad = 0; int64_t t0 = now();
    while ((sent < 300 || got < 300) && now() - t0 < 20000000000LL) {
        if (sent < 300) { uint8_t p[6] = { 7, (uint8_t)(base + sent), (uint8_t)((base + sent) >> 8), (uint8_t)role[0] }; net_send(p, 6); sent++; }
        uint8_t b[64]; int n;
        while ((n = net_recv(b, sizeof b)) > 0) { if (b[0] != 7) continue; int k = b[1] | b[2] << 8; if (k != base + got || b[3] == (uint8_t)role[0]) bad++; got++; }
        net_poll(now()); net_wait(2000000);
    }
    printf("[%s] phase %d: sent %d got %d bad %d, path %s\n", role, base, sent, got, bad, net_online_direct() ? "DIRECT" : "RELAY");
    return got == 300 && !bad;
}
int main(int argc, char **argv) {
    char opt[512]; snprintf(opt, sizeof opt, "-Djavax.net.ssl.trustStore=%s", argv[3]);
    JavaVMOption o[2] = { { opt, NULL }, { "-Djavax.net.ssl.trustStorePassword=changeit", NULL } };
    JavaVMInitArgs a = { JNI_VERSION_1_8, 2, o, JNI_FALSE };
    JavaVM *vm; JNIEnv *env;
    if (JNI_CreateJavaVM(&vm, (void **)&env, &a) != JNI_OK) return 1;
    const char *role = argv[2];
    net_test_plain_brokers(1);
    net_start_online(vm, argv[1]);
    int64_t t0 = now();
    while (now() - t0 < 20000000000LL && net_state() != NET_CONNECTED && net_state() != NET_FAILED) net_wait(20000000);
    if (net_state() != NET_CONNECTED) { printf("[%s] not connected\n", role); return 2; }
    pump(1500000000LL);
    int ok = exchange(role, 0);
    pump(500000000LL);
    if (role[0] == 'a') net_test_block_udp(1);
    pump(5000000000LL);
    printf("[%s] after the direct path died: %s\n", role, net_online_direct() ? "DIRECT (still?)" : "RELAY");
    ok &= exchange(role, 1000);
    pump(1000000000LL);
    net_stop(); net_thread_exit();
    return ok ? 0 : 3;
}
