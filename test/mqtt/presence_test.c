/* Presence + invitation scenarios against a local broker, using the app's own
 * android/net.c inside a desktop JVM.
 * usage: presence_test HOST:PORT TRUSTSTORE ROLE [seconds]
 * roles: watch | free-accept | free-decline | busy | search | crash */
#include <jni.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include "../../android/net.h"
static int64_t now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec; }
static const char *role;
static int exchange(void) {
    int sent = 0, got = 0, bad = 0; int64_t t0 = now();
    static uint8_t seen[300]; uint8_t me = (uint8_t)(getpid() & 0xFF);
    while ((sent < 300 || got < 300) && now() - t0 < 20000000000LL && net_state() == NET_CONNECTED) {
        if (sent < 300) { uint8_t p[6] = { 7, (uint8_t)sent, (uint8_t)(sent >> 8), me }; net_send(p, 6); sent++; }
        uint8_t b[64]; int n;
        while ((n = net_recv(b, sizeof b)) > 0) {
            if (b[0] != 7) continue;
            int k = b[1] | b[2] << 8;
            if (k >= 300 || b[3] == me || seen[k]) { bad++; continue; }
            seen[k] = 1; got++;
        }
        net_poll(now()); net_wait(2000000);
    }
    printf("[%s] exchanged: sent %d got %d bad %d path %s\n", role, sent, got, bad, net_online_direct() ? "DIRECT" : "RELAY");
    fflush(stdout);
    return got == 300 && !bad;
}
int main(int argc, char **argv) {
    char opt[512]; snprintf(opt, sizeof opt, "-Djavax.net.ssl.trustStore=%s", argv[2]);
    JavaVMOption o[2] = { { opt, NULL }, { "-Djavax.net.ssl.trustStorePassword=changeit", NULL } };
    JavaVMInitArgs a = { JNI_VERSION_1_8, 2, o, JNI_FALSE };
    JavaVM *vm; JNIEnv *env;
    if (JNI_CreateJavaVM(&vm, (void **)&env, &a) != JNI_OK) return 1;
    role = argv[3];
    int secs = argc > 4 ? atoi(argv[4]) : 20;
    setvbuf(stdout, NULL, _IOLBF, 0);
    net_test_plain_brokers(1);
    if (getenv("SEARCH_TIMEOUT")) net_test_search_timeout(atoi(getenv("SEARCH_TIMEOUT")));
    net_presence_start(vm, argv[1]);
    if (!strcmp(role, "busy")) net_presence_set_busy(1);
    int64_t t0 = now(), last_print = 0; char last_status[80] = "";
    int searching = 0, linked = 0, invites = 0;
    while (now() - t0 < (int64_t)secs * 1000000000LL) {
        int64_t t = now();
        net_poll(t);
        /* (data that arrives before we notice the link is kept for exchange()) */
        if (!strcmp(role, "watch") && t - last_print > 1000000000LL) {
            printf("[watch] t=%2.0fs free players: %d\n", (t - t0) / 1e9, net_presence_available()); last_print = t;
        }
        if (!strcmp(role, "search") && !searching && net_presence_available() >= 0 && t - t0 > 1500000000LL) {
            printf("[search] free players before searching: %d\n", net_presence_available());
            net_start_online(vm, argv[1]); searching = 1;
        }
        if (!strcmp(role, "crash") && t - t0 > 3000000000LL) { printf("[crash] dying without saying goodbye\n"); _exit(0); }
        int inv = net_invite_pending();
        if (inv && (!strcmp(role, "free-accept") || !strcmp(role, "free-decline") || !strcmp(role, "busy"))) {
            invites++;
            printf("[%s] invitation received (%ds to answer)\n", role, inv);
            usleep(800000);
            net_invite_answer(!strcmp(role, "free-accept"));
            printf("[%s] answered %s\n", role, !strcmp(role, "free-accept") ? "ACCEPT" : "DECLINE");
        }
        if (net_kind() == NET_KIND_ONLINE && strcmp(last_status, net_status_text()) != 0) {
            snprintf(last_status, sizeof last_status, "%s", net_status_text());
            printf("[%s] status: %s\n", role, last_status);
        }
        if (net_state() == NET_CONNECTED && !linked) {
            linked = 1;
            for (int i = 0; i < 75; i++) { net_poll(now()); usleep(20000); }   /* let the direct path form */
            exchange();
            break;
        }
        if (net_kind() == NET_KIND_ONLINE && net_state() == NET_FAILED) { printf("[%s] search ended: %s\n", role, net_status_text()); break; }
        net_wait(20000000);
    }
    printf("[%s] done (invitations seen: %d)\n", role, invites);
    if (linked) { usleep(1000000); }
    net_presence_stop();
    net_thread_exit();
    return 0;
}
