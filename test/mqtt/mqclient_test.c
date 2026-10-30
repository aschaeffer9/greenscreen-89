/* Runs android/net.c's online (JNI SSLSocket WebSocket) client inside a
 * desktop JVM. usage: wsclient_test HOST:PORT ROLE(a|b|badhost) TRUSTSTORE */
#include <jni.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include "../../android/net.h"

static int64_t now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec; }

int main(int argc, char **argv) {
    char opt[512]; snprintf(opt, sizeof opt, "-Djavax.net.ssl.trustStore=%s", argv[3]);
    JavaVMOption o[2] = { { opt, NULL }, { "-Djavax.net.ssl.trustStorePassword=changeit", NULL } };
    JavaVMInitArgs a = { JNI_VERSION_1_8, 2, o, JNI_FALSE };
    JavaVM *vm; JNIEnv *env;
    if (JNI_CreateJavaVM(&vm, (void **)&env, &a) != JNI_OK) { printf("no jvm\n"); return 1; }
    const char *role = argv[2];
    net_test_plain_brokers(1); if (getenv("NO_DIRECT")) net_test_no_direct(1); net_start_online(vm, argv[1]);
    int64_t t0 = now();
    int last = -1;
    while (now() - t0 < 25000000000LL) {
        int st = net_state();
        if (st != last) { printf("[%s] state %d: %s\n", role, st, net_status_text()); fflush(stdout); last = st; }
        if (st == NET_CONNECTED || st == NET_FAILED) break;
        net_wait(20000000);
    }
    if (!strcmp(role, "badhost")) { net_stop(); printf("[%s] done\n", role); return 0; }
    if (net_state() != NET_CONNECTED) { printf("[%s] FAIL not connected\n", role); return 2; }
    /* settle for 1.5 s (lets the direct path form); keep anything the other side already sent */
    static uint8_t early[400][64]; static int early_len[400], nearly = 0, eat = 0;
    { int64_t tw = now(); while (now() - tw < 1500000000LL) { uint8_t b[64]; int n; net_poll(now());
        while ((n = net_recv(b, sizeof b)) > 0) if (nearly < 400) { memcpy(early[nearly], b, (size_t)n); early_len[nearly++] = n; }
        net_wait(5000000); } }
    printf("[%s] path after 1.5s: %s\n", role, net_online_direct() ? "DIRECT P2P" : "BROKER RELAY");
    /* exchange 300 numbered packets each way */
    int sent = 0, got = 0, bad = 0, expect = 0;
    t0 = now();
    while ((sent < 300 || got < 300) && now() - t0 < 20000000000LL) {
        if (sent < 300) {
            uint8_t p[12] = { 1, (uint8_t)sent, (uint8_t)(sent >> 8), (uint8_t)(role[0]), 0, 0, 0 };
            for (int i = 7; i < 12; i++) p[i] = (uint8_t)(sent * 7 + i);
            net_send(p, 12); sent++;
        }
        uint8_t b[64]; int n;
        while ((eat < nearly && (n = early_len[eat], memcpy(b, early[eat], (size_t)n), eat++, 1)) || (n = net_recv(b, sizeof b)) > 0) {
            int k = b[1] | b[2] << 8;
            if (n != 12 || k != expect || b[3] == (uint8_t)role[0]) bad++;
            for (int i = 7; i < 12 && n == 12; i++) if (b[i] != (uint8_t)(k * 7 + i)) bad++;
            expect++; got++;
        }
        net_poll(now());
        net_wait(2000000);
    }
    printf("[%s] sent %d got %d bad %d (%.1fs) path %s\n", role, sent, got, bad, (now() - t0) / 1e9, net_online_direct() ? "DIRECT P2P" : "BROKER RELAY");
    if (!strcmp(role, "a")) { sleep(2); net_stop(); printf("[%s] left\n", role); }
    else {
        t0 = now();
        while (now() - t0 < 8000000000LL && net_state() == NET_CONNECTED) net_wait(20000000);
        printf("[%s] after partner left: state %d: %s\n", role, net_state(), net_status_text());
        net_stop();
    }
    net_thread_exit();
    return (got == 300 && !bad) ? 0 : 3;
}
