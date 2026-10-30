/* Link transports.
 * LAN: UDP on port 47089. Both phones broadcast HELLO on every Wi-Fi /
 *      hotspot interface; the first other phone heard is paired with, and
 *      the link is up once each side has seen the other's id in a HELLO.
 * ONLINE: matchmaking over a public open-source MQTT broker (TLS through the
 *      platform's javax.net.ssl.SSLSocket via JNI; the NDK has no TLS library),
 *      then a direct UDP path when NATs allow it, else relayed by the broker.
 * USB: phone-to-phone cable (Android Open Accessory). */
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <pthread.h>
#include <stdarg.h>
#include "net.h"

#define LAN_PORT 47089
/* tests can run two copies on one machine by crossing the ports */
static int listen_port = LAN_PORT, send_port = LAN_PORT;
void net_test_ports(int listen, int send) { listen_port = listen; send_port = send; }
static const uint8_t MAGIC[4] = { 'T', '8', '9', 1 };

static int state = NET_OFF, kind = NET_KIND_NONE;
static char status[64] = "";
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;

static void set_status(int st, const char *s) {
    pthread_mutex_lock(&mu);
    state = st;
    snprintf(status, sizeof status, "%s", s);
    pthread_mutex_unlock(&mu);
}
int net_state(void) { pthread_mutex_lock(&mu); int s = state; pthread_mutex_unlock(&mu); return s; }
int net_kind(void) { return kind; }
const char *net_status_text(void) { return status; }

static int64_t mono_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec; }
static uint32_t rand32(void) {
    uint32_t v = 0;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) { if (read(fd, &v, 4) != 4) v = 0; close(fd); }
    if (!v) v = (uint32_t)mono_ns() ^ (uint32_t)getpid() * 2654435761u;
    return v ? v : 1;
}

/* ------------------------------------------------------------------ LAN */
static int ufd = -1;
static uint32_t my_id, peer_id;
static struct sockaddr_in peer_addr;
static int peer_confirmed;
static int64_t last_hello;

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static void lan_hello(int64_t now) {
    uint8_t m[13];
    memcpy(m, MAGIC, 4); m[4] = 'H'; put32(m + 5, my_id); put32(m + 9, peer_id);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons((uint16_t)send_port);
    if (peer_id) sendto(ufd, m, sizeof m, 0, (struct sockaddr *)&peer_addr, sizeof peer_addr);
    struct ifaddrs *ifs = NULL;
    if (getifaddrs(&ifs) == 0) {
        for (struct ifaddrs *i = ifs; i; i = i->ifa_next) {
            if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
            if (!(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK) || !(i->ifa_flags & IFF_BROADCAST)) continue;
            if (!i->ifa_broadaddr) continue;
            a.sin_addr = ((struct sockaddr_in *)i->ifa_broadaddr)->sin_addr;
            sendto(ufd, m, sizeof m, 0, (struct sockaddr *)&a, sizeof a);
        }
        freeifaddrs(ifs);
    }
    a.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    sendto(ufd, m, sizeof m, 0, (struct sockaddr *)&a, sizeof a);
    last_hello = now;
}

int net_start_lan(void) {
    net_stop();
    ufd = socket(AF_INET, SOCK_DGRAM, 0);
    if (ufd < 0) { set_status(NET_FAILED, "NO NETWORK"); return -1; }
    int one = 1;
    setsockopt(ufd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    setsockopt(ufd, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
    fcntl(ufd, F_SETFL, fcntl(ufd, F_GETFL) | O_NONBLOCK);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons((uint16_t)listen_port); a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(ufd, (struct sockaddr *)&a, sizeof a) < 0) {
        close(ufd); ufd = -1; set_status(NET_FAILED, "WI-FI PORT BUSY"); return -1;
    }
    my_id = rand32(); peer_id = 0; peer_confirmed = 0; last_hello = 0;
    kind = NET_KIND_LAN;
    set_status(NET_SEARCHING, "LOOKING ON WI-FI...");
    return 0;
}

/* --------------------------------------------------------------- ONLINE
 * Matchmaking through a public, open-source MQTT broker (Eclipse Mosquitto's
 * test.mosquitto.org first, then other public brokers), then a direct UDP
 * path between the phones when the networks allow it (STUN + hole punching).
 * Until (or unless) the direct path works, link packets travel through the
 * broker. TLS comes from the platform's own SSLSocket through JNI. */
static JavaVM *jvm;
static jobject j_sock, j_in, j_out;       /* global refs */
static pthread_t ws_th; static int ws_running, ws_stop;
static int wake_pipe[2] = { -1, -1 };
static pthread_mutex_t wmu = PTHREAD_MUTEX_INITIALIZER;   /* serialises socket writes */
static char ws_host[128]; static int ws_port = 8883;      /* a broker chosen by the user ("" = built-in list) */
/* received packets (ring) */
#define RXQ 512
static struct { uint8_t d[32]; int n; } rxq[RXQ];
static int rx_h, rx_t;
static pthread_mutex_t rmu = PTHREAD_MUTEX_INITIALIZER;
static JNIEnv *game_env;                    /* the game thread's JNI env (attached lazily) */

/* public brokers, tried in order; both players walk the same list */
static const struct { const char *host; int port; } BROKERS[] = {
    { "test.mosquitto.org", 8886 },        /* Eclipse Mosquitto, Let's Encrypt certificate */
    { "broker.emqx.io", 8883 },            /* EMQX */
    { "broker.hivemq.com", 8883 },         /* HiveMQ */
};
static const struct { const char *host; int port; } STUNS[] = {
    { "stun.l.google.com", 19302 },
    { "stun.nextcloud.com", 443 },
};
#define TOPIC_BASE "greenscreen89/v3/"
static int test_mode_plain;                /* desktop test: no STUN */
static int test_no_direct;                 /* desktop test: broker relay only */

/* direct path state (shared between the broker thread and the game thread) */
static int o_fd = -1;                       /* UDP socket for STUN, punching and direct play */
static struct sockaddr_in cands[6]; static volatile int ncands;   /* peer's addresses */
static struct sockaddr_in direct_addr;
static volatile int paired, direct_ok;
static uint8_t token[8];                    /* shared by the pair, from both ids */
static int64_t punch_until, last_punch, last_udp_rx, last_keep;
static char my_hex[17], peer_hex[17], my_cands[200];

static int jexc(JNIEnv *e) {
    if ((*e)->ExceptionCheck(e)) { (*e)->ExceptionClear(e); return 1; }
    return 0;
}

static int ws_write_raw(JNIEnv *e, const uint8_t *p, int n) {
    int bad = 1;
    pthread_mutex_lock(&wmu);
    if (j_out) {
        jbyteArray arr = (*e)->NewByteArray(e, n);
        if (arr) {
            (*e)->SetByteArrayRegion(e, arr, 0, n, (const jbyte *)p);
            jclass c = (*e)->GetObjectClass(e, j_out);
            jmethodID w = (*e)->GetMethodID(e, c, "write", "([BII)V");
            jmethodID f = (*e)->GetMethodID(e, c, "flush", "()V");
            (*e)->CallVoidMethod(e, j_out, w, arr, 0, n);
            bad = jexc(e);
            if (!bad) { (*e)->CallVoidMethod(e, j_out, f); bad = jexc(e); }
            (*e)->DeleteLocalRef(e, arr);
            (*e)->DeleteLocalRef(e, c);
        }
    }
    pthread_mutex_unlock(&wmu);
    return bad ? -1 : 0;
}

/* blocking read of up to n bytes; returns bytes, 0 on timeout, -1 on error */
static int ws_read(JNIEnv *e, uint8_t *dst, int n) {
    jbyteArray arr = (*e)->NewByteArray(e, n);
    jclass c = (*e)->GetObjectClass(e, j_in);
    jmethodID r = (*e)->GetMethodID(e, c, "read", "([BII)I");
    jint got = (*e)->CallIntMethod(e, j_in, r, arr, 0, n);
    int res;
    if ((*e)->ExceptionCheck(e)) {
        jthrowable t = (*e)->ExceptionOccurred(e);
        (*e)->ExceptionClear(e);
        jclass to = (*e)->FindClass(e, "java/net/SocketTimeoutException");
        res = (to && (*e)->IsInstanceOf(e, t, to)) ? 0 : -1;
        if (to) (*e)->DeleteLocalRef(e, to);
        (*e)->DeleteLocalRef(e, t);
    } else if (got < 0) res = -1;
    else { (*e)->GetByteArrayRegion(e, arr, 0, got, (jbyte *)dst); res = got; }
    (*e)->DeleteLocalRef(e, arr);
    (*e)->DeleteLocalRef(e, c);
    return res;
}

static uint8_t rbuf[4096]; static int rlen;
static void consume(int n) { memmove(rbuf, rbuf + n, (size_t)(rlen - n)); rlen -= n; }

static void rx_push(const uint8_t *p, int n) {
    if (n > 32 || n <= 0) return;
    pthread_mutex_lock(&rmu);
    int nt = (rx_t + 1) % RXQ;
    if (nt != rx_h) { memcpy(rxq[rx_t].d, p, (size_t)n); rxq[rx_t].n = n; rx_t = nt; }
    pthread_mutex_unlock(&rmu);
    if (wake_pipe[1] >= 0) { char b = 1; if (write(wake_pipe[1], &b, 1) < 0) {} }
}

static void close_sock(JNIEnv *e) {
    pthread_mutex_lock(&wmu);
    if (j_sock) {
        jclass cSock = (*e)->FindClass(e, "java/net/Socket");
        (*e)->CallVoidMethod(e, j_sock, (*e)->GetMethodID(e, cSock, "close", "()V")); jexc(e);
    }
    if (j_in) { (*e)->DeleteGlobalRef(e, j_in); j_in = NULL; }
    if (j_out) { (*e)->DeleteGlobalRef(e, j_out); j_out = NULL; }
    if (j_sock) { (*e)->DeleteGlobalRef(e, j_sock); j_sock = NULL; }
    pthread_mutex_unlock(&wmu);
}

/* TLS connection with certificate + hostname checks; sets j_sock/j_in/j_out */
static int tls_open(JNIEnv *e, const char *host, int port) {
    jobject sock = NULL;
    {
        jclass cF = (*e)->FindClass(e, "javax/net/ssl/SSLSocketFactory");
        jmethodID gd = cF ? (*e)->GetStaticMethodID(e, cF, "getDefault", "()Ljavax/net/SocketFactory;") : NULL;
        jobject f = gd ? (*e)->CallStaticObjectMethod(e, cF, gd) : NULL;
        if (jexc(e)) f = NULL;
        jclass cSF = (*e)->FindClass(e, "javax/net/SocketFactory");
        jmethodID cs = cSF ? (*e)->GetMethodID(e, cSF, "createSocket", "()Ljava/net/Socket;") : NULL;
        if (f && cs) sock = (*e)->CallObjectMethod(e, f, cs);
        if (jexc(e)) sock = NULL;
        if (!sock) return -1;
        /* connect with a timeout so a dead broker doesn't stall the search */
        jclass cISA = (*e)->FindClass(e, "java/net/InetSocketAddress");
        jstring h = (*e)->NewStringUTF(e, host);
        jobject sa = (*e)->NewObject(e, cISA, (*e)->GetMethodID(e, cISA, "<init>", "(Ljava/lang/String;I)V"), h, port);
        if (jexc(e)) sa = NULL;
        jclass cSock = (*e)->FindClass(e, "java/net/Socket");
        if (sa) (*e)->CallVoidMethod(e, sock, (*e)->GetMethodID(e, cSock, "connect", "(Ljava/net/SocketAddress;I)V"), sa, 5000);
        if (!sa || jexc(e) || ws_stop) {
            (*e)->CallVoidMethod(e, sock, (*e)->GetMethodID(e, cSock, "close", "()V")); jexc(e);
            return -1;
        }
    }
    jclass cS = (*e)->FindClass(e, "javax/net/ssl/SSLSocket");
    {
        jmethodID gp = (*e)->GetMethodID(e, cS, "getSSLParameters", "()Ljavax/net/ssl/SSLParameters;");
        jmethodID sp = (*e)->GetMethodID(e, cS, "setSSLParameters", "(Ljavax/net/ssl/SSLParameters;)V");
        jobject prm = (gp && sp) ? (*e)->CallObjectMethod(e, sock, gp) : NULL;
        if (jexc(e)) prm = NULL;
        if (prm) {
            jclass cP = (*e)->FindClass(e, "javax/net/ssl/SSLParameters");
            jmethodID se = cP ? (*e)->GetMethodID(e, cP, "setEndpointIdentificationAlgorithm", "(Ljava/lang/String;)V") : NULL;
            if (jexc(e)) se = NULL;
            jstring https = (*e)->NewStringUTF(e, "HTTPS");
            if (se) { (*e)->CallVoidMethod(e, prm, se, https); jexc(e); (*e)->CallVoidMethod(e, sock, sp, prm); jexc(e); }
        }
    }
    jclass cSock = (*e)->FindClass(e, "java/net/Socket");
    (*e)->CallVoidMethod(e, sock, (*e)->GetMethodID(e, cSock, "setSoTimeout", "(I)V"), 6000); jexc(e);
    (*e)->CallVoidMethod(e, sock, (*e)->GetMethodID(e, cS, "startHandshake", "()V"));
    int bad = jexc(e);
    jobject sess = NULL;
    if (!bad) { sess = (*e)->CallObjectMethod(e, sock, (*e)->GetMethodID(e, cS, "getSession", "()Ljavax/net/ssl/SSLSession;")); if (jexc(e)) sess = NULL; }
    jboolean ok = JNI_FALSE;
    if (sess) {
        jclass cH = (*e)->FindClass(e, "javax/net/ssl/HttpsURLConnection");
        jobject hv = (*e)->CallStaticObjectMethod(e, cH, (*e)->GetStaticMethodID(e, cH, "getDefaultHostnameVerifier", "()Ljavax/net/ssl/HostnameVerifier;"));
        if (jexc(e)) hv = NULL;
        jclass cV = (*e)->FindClass(e, "javax/net/ssl/HostnameVerifier");
        jstring h = (*e)->NewStringUTF(e, host);
        if (hv) ok = (*e)->CallBooleanMethod(e, hv, (*e)->GetMethodID(e, cV, "verify", "(Ljava/lang/String;Ljavax/net/ssl/SSLSession;)Z"), h, sess);
        if (jexc(e)) ok = JNI_FALSE;
#ifdef NET_DESKTOP_TEST
        ok = JNI_TRUE;   /* desktop Java's default verifier always refuses; the handshake's endpoint identification checked it */
#endif
    }
    jobject in = NULL, out = NULL;
    if (ok) {
        (*e)->CallVoidMethod(e, sock, (*e)->GetMethodID(e, cSock, "setTcpNoDelay", "(Z)V"), JNI_TRUE); jexc(e);
        (*e)->CallVoidMethod(e, sock, (*e)->GetMethodID(e, cSock, "setSoTimeout", "(I)V"), 200); jexc(e);
        in = (*e)->CallObjectMethod(e, sock, (*e)->GetMethodID(e, cSock, "getInputStream", "()Ljava/io/InputStream;"));
        if (jexc(e)) in = NULL;
        out = (*e)->CallObjectMethod(e, sock, (*e)->GetMethodID(e, cSock, "getOutputStream", "()Ljava/io/OutputStream;"));
        if (jexc(e)) out = NULL;
    }
    if (!ok || !in || !out) {
        (*e)->CallVoidMethod(e, sock, (*e)->GetMethodID(e, cSock, "close", "()V")); jexc(e);
        return -1;
    }
    pthread_mutex_lock(&wmu);
    j_sock = (*e)->NewGlobalRef(e, sock);
    j_in = (*e)->NewGlobalRef(e, in);
    j_out = (*e)->NewGlobalRef(e, out);
    pthread_mutex_unlock(&wmu);
    rlen = 0;
    return 0;
}

/* ---- minimal MQTT 3.1.1 client (QoS 0) ---- */
static int mq_varlen(uint8_t *p, int n) { int k = 0; do { uint8_t b = (uint8_t)(n & 127); n >>= 7; if (n) b |= 128; p[k++] = b; } while (n); return k; }
static int mq_str(uint8_t *p, const char *s) { int n = (int)strlen(s); p[0] = (uint8_t)(n >> 8); p[1] = (uint8_t)n; memcpy(p + 2, s, (size_t)n); return n + 2; }
static int mq_publish(JNIEnv *e, const char *topic, const uint8_t *pl, int n) {
    uint8_t body[300], pkt[310];
    int tl = (int)strlen(topic);
    if (tl + 2 + n > (int)sizeof body) return -1;
    int k = mq_str(body, topic);
    memcpy(body + k, pl, (size_t)n); k += n;
    pkt[0] = 0x30; int h = 1 + mq_varlen(pkt + 1, k);
    memcpy(pkt + h, body, (size_t)k);
    return ws_write_raw(e, pkt, h + k);
}
static int mq_publish_text(JNIEnv *e, const char *topic, const char *s) { return mq_publish(e, topic, (const uint8_t *)s, (int)strlen(s)); }
static int mq_connect(JNIEnv *e) {
    uint8_t body[128], pkt[140]; int k = 0;
    k += mq_str(body + k, "MQTT");
    body[k++] = 4;            /* protocol level 3.1.1 */
    body[k++] = 0x02;         /* clean session */
    body[k++] = 0; body[k++] = 30;   /* keepalive 30 s */
    char cid[32]; snprintf(cid, sizeof cid, "t89-%s", my_hex);
    k += mq_str(body + k, cid);
    pkt[0] = 0x10; int h = 1 + mq_varlen(pkt + 1, k);
    memcpy(pkt + h, body, (size_t)k);
    return ws_write_raw(e, pkt, h + k);
}
static int mq_subscribe(JNIEnv *e, const char *t1, const char *t2) {
    uint8_t body[256], pkt[270]; int k = 0;
    body[k++] = 0; body[k++] = 1;           /* packet id */
    k += mq_str(body + k, t1); body[k++] = 0;
    k += mq_str(body + k, t2); body[k++] = 0;
    pkt[0] = 0x82; int h = 1 + mq_varlen(pkt + 1, k);
    memcpy(pkt + h, body, (size_t)k);
    return ws_write_raw(e, pkt, h + k);
}
/* next whole MQTT packet from the stream: returns 1 with type/payload, 0 timeout, -1 error */
static int mq_next(JNIEnv *e, int *type, uint8_t **pl, int *plen, int *total) {
    for (;;) {
        if (rlen >= 2) {
            int len = 0, mul = 1, i = 1;
            while (i < rlen && i < 5) {
                len += (rbuf[i] & 127) * mul; mul *= 128;
                if (!(rbuf[i] & 128)) break;
                i++;
            }
            if (i >= 5) return -1;
            if (i < rlen && !(rbuf[i] & 128)) {
                int h = i + 1;
                if (h + len > (int)sizeof rbuf) return -1;
                if (rlen >= h + len) { *type = rbuf[0]; *pl = rbuf + h; *plen = len; *total = h + len; return 1; }
            }
        }
        if (ws_stop) return -1;
        int g = ws_read(e, rbuf + rlen, (int)sizeof rbuf - rlen);
        if (g < 0) return -1;
        if (g == 0) return 0;
        rlen += g;
    }
}

/* ---- STUN (RFC 5389 binding request) ---- */
static int resolve4(const char *host, int port, struct sockaddr_in *out) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints); hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
    char ps[8]; snprintf(ps, sizeof ps, "%d", port);
    if (getaddrinfo(host, ps, &hints, &res) != 0 || !res) return -1;
    memcpy(out, res->ai_addr, sizeof *out);
    freeaddrinfo(res);
    return 0;
}
static int stun_public(struct sockaddr_in *pub) {
    for (unsigned s = 0; s < sizeof STUNS / sizeof STUNS[0] && !ws_stop; s++) {
        struct sockaddr_in sa;
        if (resolve4(STUNS[s].host, STUNS[s].port, &sa) != 0) continue;
        uint8_t req[20] = { 0x00, 0x01, 0x00, 0x00, 0x21, 0x12, 0xA4, 0x42 };
        for (int i = 8; i < 20; i += 4) put32(req + i, rand32());
        for (int attempt = 0; attempt < 3; attempt++) {
            sendto(o_fd, req, sizeof req, 0, (struct sockaddr *)&sa, sizeof sa);
            struct pollfd p = { o_fd, POLLIN, 0 };
            int64_t until = mono_ns() + 400000000LL;
            while (mono_ns() < until) {
                if (poll(&p, 1, 50) <= 0) continue;
                uint8_t r[512]; ssize_t g = recv(o_fd, r, sizeof r, 0);
                if (g < 20 || r[0] != 0x01 || r[1] != 0x01 || memcmp(r + 8, req + 8, 12) != 0) continue;
                int off = 20;
                while (off + 4 <= g) {
                    int at = r[off] << 8 | r[off + 1], al = r[off + 2] << 8 | r[off + 3];
                    const uint8_t *v = r + off + 4;
                    if (off + 4 + al > g) break;
                    if ((at == 0x0020 || at == 0x0001) && al >= 8 && v[1] == 0x01) {
                        uint16_t port = (uint16_t)(v[2] << 8 | v[3]);
                        uint32_t ip = (uint32_t)v[4] << 24 | (uint32_t)v[5] << 16 | (uint32_t)v[6] << 8 | v[7];
                        if (at == 0x0020) { port ^= 0x2112; ip ^= 0x2112A442u; }
                        memset(pub, 0, sizeof *pub);
                        pub->sin_family = AF_INET; pub->sin_port = htons(port); pub->sin_addr.s_addr = htonl(ip);
                        return 0;
                    }
                    off += 4 + ((al + 3) & ~3);
                }
            }
        }
    }
    return -1;
}

/* our candidate addresses: public (from STUN) + every local IPv4 */
static void gather_cands(int have_pub, struct sockaddr_in *pub) {
    struct sockaddr_in me; socklen_t ml = sizeof me;
    getsockname(o_fd, (struct sockaddr *)&me, &ml);
    int port = ntohs(me.sin_port), k = 0, n = 0;
    my_cands[0] = 0;
    if (have_pub) k += snprintf(my_cands + k, sizeof my_cands - (size_t)k, "%s:%d", inet_ntoa(pub->sin_addr), ntohs(pub->sin_port)), n++;
    struct ifaddrs *ifs = NULL, *i;
    if (getifaddrs(&ifs) == 0) {
        for (i = ifs; i && n < 4; i = i->ifa_next) {
            if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || (i->ifa_flags & IFF_LOOPBACK)) continue;
            struct sockaddr_in *a = (struct sockaddr_in *)i->ifa_addr;
            k += snprintf(my_cands + k, sizeof my_cands - (size_t)k, "%s%s:%d", n ? "," : "", inet_ntoa(a->sin_addr), port);
            n++;
        }
        freeifaddrs(ifs);
    }
#ifdef NET_DESKTOP_TEST
    k += snprintf(my_cands + k, sizeof my_cands - (size_t)k, "%s127.0.0.1:%d", n ? "," : "", port);
#endif
}
static void parse_cands(const char *s) {
    int n = 0;
    while (s && *s && n < 6) {
        char ip[32]; int port = 0;
        const char *c = strchr(s, ':'); if (!c || c - s >= 31) break;
        memcpy(ip, s, (size_t)(c - s)); ip[c - s] = 0;
        port = atoi(c + 1);
        struct sockaddr_in a; memset(&a, 0, sizeof a);
        a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port);
        if (port > 0 && port < 65536 && inet_aton(ip, &a.sin_addr)) cands[n++] = a;
        s = strchr(c, ','); if (s) s++;
    }
    ncands = n;
}
static void make_token(void) {
    const char *a = strcmp(my_hex, peer_hex) < 0 ? my_hex : peer_hex, *b = a == my_hex ? peer_hex : my_hex;
    uint64_t h = 1469598103934665603ULL;
    for (const char *p = a; *p; p++) { h ^= (uint8_t)*p; h *= 1099511628211ULL; }
    for (const char *p = b; *p; p++) { h ^= (uint8_t)*p; h *= 1099511628211ULL; }
    for (int i = 0; i < 8; i++) token[i] = (uint8_t)(h >> (8 * i));
}

static char t_lobby[96], t_me[96], t_peer[96];

static void become_paired(const char *peer, const char *cl) {
    snprintf(peer_hex, sizeof peer_hex, "%s", peer);
    snprintf(t_peer, sizeof t_peer, TOPIC_BASE "p/%s", peer_hex);
    parse_cands(cl);
    make_token();
    direct_ok = 0;
    punch_until = mono_ns() + 6000000000LL;
    last_udp_rx = 0;
    __atomic_store_n(&paired, 1, __ATOMIC_RELEASE);
    set_status(NET_CONNECTED, "LINKED ONLINE");
}

static void *ws_thread(void *arg) {
    (void)arg;
    JNIEnv *e = NULL;
    if ((*jvm)->AttachCurrentThread(jvm, &e, NULL) != JNI_OK || !e) { set_status(NET_FAILED, "INTERNAL ERROR"); ws_running = 0; return NULL; }
    set_status(NET_SEARCHING, "CONNECTING...");
    struct sockaddr_in pub;
    int have_pub = test_mode_plain ? 0 : (stun_public(&pub) == 0);
    gather_cands(have_pub, &pub);
    int nb = (int)(sizeof BROKERS / sizeof BROKERS[0]);
    int connected = 0;
    for (int bi = 0; bi < (ws_host[0] ? 1 : nb) && !ws_stop && !connected; bi++) {
        const char *h = ws_host[0] ? ws_host : BROKERS[bi].host;
        int port = ws_host[0] ? ws_port : BROKERS[bi].port;
        if (tls_open(e, h, port) != 0) continue;
        int type, plen, total; uint8_t *pl;
        if (mq_connect(e) == 0) {
            int64_t t0 = mono_ns();
            while (!ws_stop && mono_ns() - t0 < 6000000000LL) {
                int r = mq_next(e, &type, &pl, &plen, &total);
                if (r < 0) break;
                if (r == 0) continue;
                consume(total);
                if ((type & 0xF0) == 0x20 && plen >= 2 && pl[1] == 0) { connected = 1; break; }
                break;
            }
        }
        if (!connected) close_sock(e);
    }
    if (!connected) { if (!ws_stop) set_status(NET_FAILED, "CAN'T REACH ANY SERVER"); goto done; }
    snprintf(t_lobby, sizeof t_lobby, TOPIC_BASE "lobby");
    snprintf(t_me, sizeof t_me, TOPIC_BASE "p/%s", my_hex);
    if (mq_subscribe(e, t_lobby, t_me) != 0) { set_status(NET_FAILED, "CONNECTION FAILED"); goto done_sock; }
    set_status(NET_SEARCHING, "WAITING FOR A PLAYER...");
    {
        int64_t last_w = 0, last_ping = mono_ns(), pend_t = 0, last_rx = mono_ns();
        char pending[17] = "";
        while (!ws_stop) {
            int64_t now = mono_ns();
            if (!paired && now - last_w > 1000000000LL) {
                char m[40]; snprintf(m, sizeof m, "W %s", my_hex);
                mq_publish_text(e, t_lobby, m); last_w = now;
            }
            if (pending[0] && now - pend_t > 3000000000LL) pending[0] = 0;
            if (now - last_ping > 15000000000LL) { uint8_t pr[2] = { 0xC0, 0 }; ws_write_raw(e, pr, 2); last_ping = now; }
            if (now - last_rx > 45000000000LL) break;          /* broker went quiet */
            int type, plen, total; uint8_t *pl;
            int r = mq_next(e, &type, &pl, &plen, &total);
            if (r < 0) break;
            if (r == 0) continue;
            last_rx = mono_ns();
            if ((type & 0xF0) == 0x30 && plen >= 2) {
                int tl = pl[0] << 8 | pl[1];
                int hdr = 2 + tl + ((type & 6) ? 2 : 0);
                if (hdr > plen) { consume(total); continue; }
                char topic[128]; int cl = tl < 127 ? tl : 127; memcpy(topic, pl + 2, (size_t)cl); topic[cl] = 0;
                uint8_t *d = pl + hdr; int dn = plen - hdr;
                if (dn >= 5 && memcmp(d, MAGIC, 4) == 0 && d[4] == 'D') {
                    if (paired && !strcmp(topic, t_me)) rx_push(d + 5, dn - 5);
                } else if (dn > 2 && dn < 230) {
                    char m[232]; memcpy(m, d, (size_t)dn); m[dn] = 0;
                    char who[17] = ""; const char *rest = NULL;
                    if (strlen(m) >= 18 && m[1] == ' ') { memcpy(who, m + 2, 16); who[16] = 0; rest = m[18] == ' ' ? m + 19 : ""; }
                    if (who[0] && strcmp(who, my_hex) != 0) {
                        char reply[240];
                        if (m[0] == 'W' && !strcmp(topic, t_lobby)) {
                            if (!paired && !pending[0] && strcmp(my_hex, who) < 0) {
                                char tp[96]; snprintf(tp, sizeof tp, TOPIC_BASE "p/%s", who);
                                snprintf(reply, sizeof reply, "I %s %s", my_hex, my_cands);
                                mq_publish_text(e, tp, reply);
                                snprintf(pending, sizeof pending, "%s", who); pend_t = mono_ns();
                            }
                        } else if (!strcmp(topic, t_me)) {
                            char tp[96]; snprintf(tp, sizeof tp, TOPIC_BASE "p/%s", who);
                            if (m[0] == 'I') {
                                if (!paired) {
                                    snprintf(reply, sizeof reply, "A %s %s", my_hex, my_cands);
                                    mq_publish_text(e, tp, reply);
                                    become_paired(who, rest);
                                } else if (strcmp(who, peer_hex) != 0) {
                                    snprintf(reply, sizeof reply, "N %s", my_hex); mq_publish_text(e, tp, reply);
                                }
                            } else if (m[0] == 'A') {
                                if (!paired && !strcmp(pending, who)) { pending[0] = 0; become_paired(who, rest); }
                                else if (!paired || strcmp(who, peer_hex) != 0) { snprintf(reply, sizeof reply, "N %s", my_hex); mq_publish_text(e, tp, reply); }
                            } else if (m[0] == 'N') {
                                if (!strcmp(pending, who)) pending[0] = 0;
                                if (paired && !strcmp(who, peer_hex)) { set_status(NET_FAILED, "OTHER PLAYER LEFT"); consume(total); break; }
                            }
                        }
                    }
                }
            }
            consume(total);
        }
    }
    if (net_state() != NET_FAILED && !ws_stop) set_status(NET_FAILED, "CONNECTION LOST");
done_sock:
    if (!ws_stop) { uint8_t dc[2] = { 0xE0, 0 }; ws_write_raw(e, dc, 2); }
    close_sock(e);
done:
    (*jvm)->DetachCurrentThread(jvm);
    ws_running = 0;
    if (wake_pipe[1] >= 0) { char b = 1; if (write(wake_pipe[1], &b, 1) < 0) {} }
    return NULL;
}

/* host "" = built-in list of public brokers; "host[:port]" = that broker */
int net_start_online(JavaVM *vm, const char *host) {
    net_stop();
    jvm = vm;
    snprintf(ws_host, sizeof ws_host, "%s", host ? host : "");
    ws_port = 8883;
    char *colon = strchr(ws_host, ':');
    if (colon) { ws_port = atoi(colon + 1); *colon = 0; if (ws_port <= 0) ws_port = 8883; }
    if (wake_pipe[0] < 0) {
        if (pipe(wake_pipe) == 0) {
            fcntl(wake_pipe[0], F_SETFL, fcntl(wake_pipe[0], F_GETFL) | O_NONBLOCK);
            fcntl(wake_pipe[1], F_SETFL, fcntl(wake_pipe[1], F_GETFL) | O_NONBLOCK);
        }
    }
    uint32_t a = rand32(), b = rand32();
    snprintf(my_hex, sizeof my_hex, "%08x%08x", a, b);
    paired = 0; direct_ok = 0; ncands = 0; peer_hex[0] = 0;
    o_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (o_fd >= 0) {
        struct sockaddr_in me; memset(&me, 0, sizeof me);
        me.sin_family = AF_INET; me.sin_addr.s_addr = htonl(INADDR_ANY); me.sin_port = 0;
        bind(o_fd, (struct sockaddr *)&me, sizeof me);
        fcntl(o_fd, F_SETFL, fcntl(o_fd, F_GETFL) | O_NONBLOCK);
    }
    rx_h = rx_t = 0; ws_stop = 0; ws_running = 1;
    kind = NET_KIND_ONLINE;
    set_status(NET_SEARCHING, "CONNECTING...");
    if (pthread_create(&ws_th, NULL, ws_thread, NULL) != 0) { ws_running = 0; set_status(NET_FAILED, "INTERNAL ERROR"); return -1; }
    return 0;
}
void net_test_plain_brokers(int on) { test_mode_plain = on; }
void net_test_no_direct(int on) { test_no_direct = on; }
static int test_block_udp;
void net_test_block_udp(int on) { test_block_udp = on; }   /* tests: a path that dies mid-game */
int net_online_direct(void) { return kind == NET_KIND_ONLINE && direct_ok; }

/* game thread: the direct UDP path (punching, keepalive, data) */
static void online_udp_send(const struct sockaddr_in *to, uint8_t type, const uint8_t *p, int n) {
    uint8_t m[64]; if (n > 40) return;
    memcpy(m, MAGIC, 4); m[4] = type; memcpy(m + 5, token, 8); if (n) memcpy(m + 13, p, (size_t)n);
    sendto(o_fd, m, (size_t)(13 + n), 0, (const struct sockaddr *)to, sizeof *to);
}
static void online_poll(int64_t now) {
    if (o_fd < 0 || test_no_direct || !__atomic_load_n(&paired, __ATOMIC_ACQUIRE)) return;
    if (!direct_ok && now < punch_until && now - last_punch > 100000000LL) {
        for (int i = 0; i < ncands; i++) online_udp_send(&cands[i], 'P', NULL, 0);
        last_punch = now;
    }
    if (direct_ok) {
        if (now - last_keep > 400000000LL) { online_udp_send(&direct_addr, 'K', NULL, 0); last_keep = now; }
        if (now - last_udp_rx > 2000000000LL) { direct_ok = 0; punch_until = now + 5000000000LL; }   /* back to the broker */
    }
}
/* returns a data payload length (copied to buf) or 0 */
static int online_udp_recv(uint8_t *buf, int max) {
    if (o_fd < 0) return 0;
    for (;;) {
        uint8_t m[128]; struct sockaddr_in from; socklen_t fl = sizeof from;
        ssize_t g = recvfrom(o_fd, m, sizeof m, 0, (struct sockaddr *)&from, &fl);
        if (g <= 0) return 0;
        if (test_block_udp) continue;
        if (!paired || test_no_direct || g < 13 || memcmp(m, MAGIC, 4) != 0 || memcmp(m + 5, token, 8) != 0) continue;
        int64_t now = mono_ns();
        if (m[4] == 'P') { online_udp_send(&from, 'Q', NULL, 0); continue; }
        if (m[4] == 'Q') { direct_addr = from; last_udp_rx = now; if (!direct_ok) { direct_ok = 1; last_keep = 0; } continue; }
        if (from.sin_addr.s_addr != direct_addr.sin_addr.s_addr || from.sin_port != direct_addr.sin_port) continue;
        last_udp_rx = now;
        if (m[4] == 'K') continue;
        if (m[4] == 'D') { int n = (int)g - 13; if (n > max) n = max; memcpy(buf, m + 13, (size_t)n); return n; }
    }
}


/* ----------------------------------------------------------------- USB
 * Phone-to-phone cable using Android Open Accessory (AOA): whichever phone
 * ends up as the USB host asks the other to switch into "accessory" mode;
 * then both get a plain two-way byte pipe (bulk endpoints / a file
 * descriptor). Packets are framed as [A5][len][payload]. */
#include <sys/ioctl.h>
#include <linux/usbdevice_fs.h>

static JNIEnv *genv(void);
enum { U_SCAN, U_PERM, U_SWITCHING, U_OPEN, U_DEAD };
static int ustate;
static int64_t u_next, u_since, u_last_hello, u_started;
static jobject j_act, j_conn, j_pfd;
static int u_fd = -1, u_host, u_ep_in, u_ep_out;
static volatile int u_gen, u_peer_seen, u_failed;
static int u_asked;                  /* permission requested for the current target */
static char u_bad[8][64]; static int u_nbad;   /* device names that refused AOA */
static pthread_mutex_t umu = PTHREAD_MUTEX_INITIALIZER;

static int ctrl(int fd, int type, int req, int val, int idx, void *data, int len) {
    struct usbdevfs_ctrltransfer c;
    memset(&c, 0, sizeof c);
    c.bRequestType = (uint8_t)type; c.bRequest = (uint8_t)req; c.wValue = (uint16_t)val; c.wIndex = (uint16_t)idx;
    c.wLength = (uint16_t)len; c.timeout = 1000; c.data = data;
    return ioctl(fd, USBDEVFS_CONTROL, &c);
}

static void usb_frame_parse(uint8_t *buf, int *len, int gen) {
    int p = 0;
    while (*len - p >= 2) {
        if (buf[p] != 0xA5) { p++; continue; }
        int n = buf[p + 1];
        if (*len - p < 2 + n) break;
        uint8_t *m = buf + p + 2;
        if (gen == u_gen && n >= 5 && memcmp(m, MAGIC, 4) == 0) {
            if (m[4] == 'D') rx_push(m + 5, n - 5);
            else if (m[4] == 'U') u_peer_seen = 1;
        }
        p += 2 + n;
    }
    memmove(buf, buf + p, (size_t)(*len - p)); *len -= p;
}

typedef struct { int fd, host, ep_in, gen; } UArg;
static void *usb_reader(void *arg) {
    UArg a = *(UArg *)arg; free(arg);
    static uint8_t chunk[16384];
    uint8_t acc[1024]; int alen = 0;
    while (a.gen == u_gen) {
        int n;
        if (a.host) {
            struct usbdevfs_bulktransfer b = { (unsigned)a.ep_in, sizeof chunk, 200, chunk };
            n = ioctl(a.fd, USBDEVFS_BULK, &b);
            if (n < 0 && (errno == ETIMEDOUT || errno == EAGAIN || errno == EINTR)) continue;
        } else {
            n = (int)read(a.fd, chunk, sizeof chunk);
            if (n < 0 && errno == EINTR) continue;
        }
        if (n <= 0) { if (a.gen == u_gen) { u_failed = 1; set_status(NET_FAILED, "CABLE DISCONNECTED"); } break; }
        for (int i = 0; i < n; i += 512) {
            int k = n - i < 512 ? n - i : 512;
            if (alen + k > (int)sizeof acc) alen = 0;
            memcpy(acc + alen, chunk + i, (size_t)k); alen += k;
            usb_frame_parse(acc, &alen, a.gen);
        }
        if (wake_pipe[1] >= 0) { char b1 = 1; if (write(wake_pipe[1], &b1, 1) < 0) {} }
    }
    return NULL;
}

static int usb_write(const uint8_t *p, int n) {
    if (u_fd < 0 || n > 250) return -1;
    uint8_t f[256]; f[0] = 0xA5; f[1] = (uint8_t)n; memcpy(f + 2, p, (size_t)n);
    pthread_mutex_lock(&umu);
    int r;
    if (u_host) {
        struct usbdevfs_bulktransfer b = { (unsigned)u_ep_out, (unsigned)(n + 2), 100, f };
        r = ioctl(u_fd, USBDEVFS_BULK, &b);
    } else r = (int)write(u_fd, f, (size_t)n + 2);
    pthread_mutex_unlock(&umu);
    return r;
}

static void usb_hello(void) { uint8_t m[5]; memcpy(m, MAGIC, 4); m[4] = 'U'; usb_write(m, 5); }

static void usb_start_reader(int fd, int host, int ep_in) {
    UArg *a = malloc(sizeof *a);
    if (!a) return;
    a->fd = fd; a->host = host; a->ep_in = ep_in; a->gen = ++u_gen;
    pthread_t t;
    if (pthread_create(&t, NULL, usb_reader, a) == 0) pthread_detach(t); else free(a);
}

/* JNI helpers (game thread) */
static jobject jcall_obj(JNIEnv *e, jobject o, const char *cls, const char *m, const char *sig, ...) {
    jclass c = (*e)->FindClass(e, cls);
    if (!c) { jexc(e); return NULL; }
    jmethodID id = (*e)->GetMethodID(e, c, m, sig);
    if (!id) { jexc(e); return NULL; }
    va_list ap; va_start(ap, sig);
    jobject r = (*e)->CallObjectMethodV(e, o, id, ap);
    va_end(ap);
    if (jexc(e)) return NULL;
    return r;
}
static jint jcall_int(JNIEnv *e, jobject o, const char *cls, const char *m, const char *sig, ...) {
    jclass c = (*e)->FindClass(e, cls);
    if (!c) { jexc(e); return -1; }
    jmethodID id = (*e)->GetMethodID(e, c, m, sig);
    if (!id) { jexc(e); return -1; }
    va_list ap; va_start(ap, sig);
    jint r = (*e)->CallIntMethodV(e, o, id, ap);
    va_end(ap);
    if (jexc(e)) return -1;
    return r;
}
static int jcall_bool(JNIEnv *e, jobject o, const char *cls, const char *m, const char *sig, ...) {
    jclass c = (*e)->FindClass(e, cls);
    if (!c) { jexc(e); return 0; }
    jmethodID id = (*e)->GetMethodID(e, c, m, sig);
    if (!id) { jexc(e); return 0; }
    va_list ap; va_start(ap, sig);
    jboolean r = (*e)->CallBooleanMethodV(e, o, id, ap);
    va_end(ap);
    if (jexc(e)) return 0;
    return r ? 1 : 0;
}

static void usb_request_permission(JNIEnv *e, jobject um, jobject target, int accessory) {
    jclass cI = (*e)->FindClass(e, "android/content/Intent");
    jmethodID ctor = (*e)->GetMethodID(e, cI, "<init>", "(Ljava/lang/String;)V");
    jobject intent = (*e)->NewObject(e, cI, ctor, (*e)->NewStringUTF(e, "greenscreen89.USB_PERMISSION"));
    if (jexc(e) || !intent) return;
    jobject pkg = jcall_obj(e, j_act, "android/content/Context", "getPackageName", "()Ljava/lang/String;");
    if (pkg) jcall_obj(e, intent, "android/content/Intent", "setPackage", "(Ljava/lang/String;)Landroid/content/Intent;", pkg);
    jclass cP = (*e)->FindClass(e, "android/app/PendingIntent");
    jmethodID gb = (*e)->GetStaticMethodID(e, cP, "getBroadcast", "(Landroid/content/Context;ILandroid/content/Intent;I)Landroid/app/PendingIntent;");
    jobject pi = (*e)->CallStaticObjectMethod(e, cP, gb, j_act, 0, intent, 0x04000000 /* FLAG_IMMUTABLE */);
    if (jexc(e) || !pi) return;
    if (accessory)
        jcall_obj(e, um, "android/hardware/usb/UsbManager", "requestPermission", "(Landroid/hardware/usb/UsbAccessory;Landroid/app/PendingIntent;)V", target, pi);
    else
        jcall_obj(e, um, "android/hardware/usb/UsbManager", "requestPermission", "(Landroid/hardware/usb/UsbDevice;Landroid/app/PendingIntent;)V", target, pi);
    jexc(e);
}

static int is_aoa(int vid, int pid) { return vid == 0x18D1 && pid >= 0x2D00 && pid <= 0x2D05; }

static void usb_step(int64_t now) {
    JNIEnv *e = genv();
    if (!e || !j_act) return;
    if ((*e)->PushLocalFrame(e, 64) != 0) { jexc(e); return; }
    jobject um = jcall_obj(e, j_act, "android/content/Context", "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;", (*e)->NewStringUTF(e, "usb"));
    if (!um) { set_status(NET_FAILED, "NO USB ON THIS PHONE"); ustate = U_DEAD; goto out; }

    /* accessory side: the other phone already switched us into accessory mode */
    jobjectArray accs = (jobjectArray)jcall_obj(e, um, "android/hardware/usb/UsbManager", "getAccessoryList", "()[Landroid/hardware/usb/UsbAccessory;");
    if (accs && (*e)->GetArrayLength(e, accs) > 0) {
        jobject acc = (*e)->GetObjectArrayElement(e, accs, 0);
        if (!jcall_bool(e, um, "android/hardware/usb/UsbManager", "hasPermission", "(Landroid/hardware/usb/UsbAccessory;)Z", acc)) {
            if (!u_asked) { usb_request_permission(e, um, acc, 1); u_asked = 1; }
            set_status(NET_SEARCHING, "ALLOW USB ACCESS ON THIS PHONE");
            ustate = U_PERM; goto out;
        }
        jobject pfd = jcall_obj(e, um, "android/hardware/usb/UsbManager", "openAccessory", "(Landroid/hardware/usb/UsbAccessory;)Landroid/os/ParcelFileDescriptor;", acc);
        int fd = pfd ? jcall_int(e, pfd, "android/os/ParcelFileDescriptor", "getFd", "()I") : -1;
        if (fd < 0) { set_status(NET_SEARCHING, "OPENING CABLE..."); goto out; }
        j_pfd = (*e)->NewGlobalRef(e, pfd);
        u_fd = fd; u_host = 0; u_peer_seen = 0; u_failed = 0;
        usb_start_reader(fd, 0, 0);
        ustate = U_OPEN; u_since = now;
        set_status(NET_SEARCHING, "CABLE OPEN - SAYING HELLO...");
        goto out;
    }

    /* host side: look at what's plugged into us */
    jobject map = jcall_obj(e, um, "android/hardware/usb/UsbManager", "getDeviceList", "()Ljava/util/HashMap;");
    jobject vals = map ? jcall_obj(e, map, "java/util/HashMap", "values", "()Ljava/util/Collection;") : NULL;
    jobjectArray devs = vals ? (jobjectArray)jcall_obj(e, vals, "java/util/Collection", "toArray", "()[Ljava/lang/Object;") : NULL;
    int nd = devs ? (*e)->GetArrayLength(e, devs) : 0;
    jobject pick = NULL; int pick_aoa = 0; char pick_name[64] = "";
    for (int i = 0; i < nd; i++) {
        jobject d = (*e)->GetObjectArrayElement(e, devs, i);
        int vid = jcall_int(e, d, "android/hardware/usb/UsbDevice", "getVendorId", "()I");
        int pid = jcall_int(e, d, "android/hardware/usb/UsbDevice", "getProductId", "()I");
        jstring jn = (jstring)jcall_obj(e, d, "android/hardware/usb/UsbDevice", "getDeviceName", "()Ljava/lang/String;");
        char name[64] = "";
        if (jn) { const char *u = (*e)->GetStringUTFChars(e, jn, NULL); if (u) { snprintf(name, sizeof name, "%s", u); (*e)->ReleaseStringUTFChars(e, jn, u); } }
        int bad = 0; for (int k = 0; k < u_nbad; k++) if (!strcmp(u_bad[k], name)) bad = 1;
        if (bad) continue;
        if (is_aoa(vid, pid)) { pick = d; pick_aoa = 1; snprintf(pick_name, sizeof pick_name, "%s", name); break; }
        /* skip keyboards, mice and hubs */
        int ni = jcall_int(e, d, "android/hardware/usb/UsbDevice", "getInterfaceCount", "()I"), useful = (ni == 0);
        for (int k = 0; k < ni; k++) {
            jobject itf = jcall_obj(e, d, "android/hardware/usb/UsbDevice", "getInterface", "(I)Landroid/hardware/usb/UsbInterface;", k);
            int cls = itf ? jcall_int(e, itf, "android/hardware/usb/UsbInterface", "getInterfaceClass", "()I") : 3;
            if (cls != 3 && cls != 9 && cls != 1) useful = 1;
        }
        if (useful && !pick) { pick = d; snprintf(pick_name, sizeof pick_name, "%s", name); }
    }
    if (!pick) {
        if (ustate == U_SWITCHING && now - u_since < 10000000000LL) set_status(NET_SEARCHING, "SWITCHING THE OTHER PHONE...");
        else if (now - u_started > 6000000000LL) set_status(NET_SEARCHING, "NO CABLE YET - OPEN THE GAME ON BOTH");
        else set_status(NET_SEARCHING, "PLUG THE PHONES TOGETHER...");
        if (ustate != U_SWITCHING) ustate = U_SCAN;
        goto out;
    }
    if (!jcall_bool(e, um, "android/hardware/usb/UsbManager", "hasPermission", "(Landroid/hardware/usb/UsbDevice;)Z", pick)) {
        if (!u_asked) { usb_request_permission(e, um, pick, 0); u_asked = 1; }
        set_status(NET_SEARCHING, "ALLOW USB ACCESS ON THIS PHONE");
        ustate = U_PERM; goto out;
    }
    jobject conn = jcall_obj(e, um, "android/hardware/usb/UsbManager", "openDevice", "(Landroid/hardware/usb/UsbDevice;)Landroid/hardware/usb/UsbDeviceConnection;", pick);
    int fd = conn ? jcall_int(e, conn, "android/hardware/usb/UsbDeviceConnection", "getFileDescriptor", "()I") : -1;
    if (fd < 0) { set_status(NET_SEARCHING, "OPENING CABLE..."); goto out; }
    if (pick_aoa) {
        jobject itf = jcall_obj(e, pick, "android/hardware/usb/UsbDevice", "getInterface", "(I)Landroid/hardware/usb/UsbInterface;", 0);
        int ne = itf ? jcall_int(e, itf, "android/hardware/usb/UsbInterface", "getEndpointCount", "()I") : 0;
        int ein = -1, eout = -1;
        for (int k = 0; k < ne; k++) {
            jobject ep = jcall_obj(e, itf, "android/hardware/usb/UsbInterface", "getEndpoint", "(I)Landroid/hardware/usb/UsbEndpoint;", k);
            if (!ep) continue;
            if (jcall_int(e, ep, "android/hardware/usb/UsbEndpoint", "getType", "()I") != 2) continue;   /* bulk */
            int addr = jcall_int(e, ep, "android/hardware/usb/UsbEndpoint", "getAddress", "()I");
            if (jcall_int(e, ep, "android/hardware/usb/UsbEndpoint", "getDirection", "()I") == 0x80) ein = addr; else eout = addr;
        }
        if (ein < 0 || eout < 0 || !jcall_bool(e, conn, "android/hardware/usb/UsbDeviceConnection", "claimInterface", "(Landroid/hardware/usb/UsbInterface;Z)Z", itf, JNI_TRUE)) {
            jcall_obj(e, conn, "android/hardware/usb/UsbDeviceConnection", "close", "()V");
            set_status(NET_SEARCHING, "CABLE BUSY - RETRYING..."); goto out;
        }
        j_conn = (*e)->NewGlobalRef(e, conn);
        u_fd = fd; u_host = 1; u_ep_in = ein; u_ep_out = eout; u_peer_seen = 0; u_failed = 0;
        usb_start_reader(fd, 1, ein);
        ustate = U_OPEN; u_since = now;
        set_status(NET_SEARCHING, "CABLE OPEN - SAYING HELLO...");
        goto out;
    }
    /* ask the other phone to become an accessory */
    {
        uint8_t ver[2] = { 0, 0 };
        int r = ctrl(fd, 0xC0, 51, 0, 0, ver, 2);
        if (r < 2 || (ver[0] | ver[1] << 8) < 1) {
            if (u_nbad < 8) snprintf(u_bad[u_nbad++], sizeof u_bad[0], "%s", pick_name);
            jcall_obj(e, conn, "android/hardware/usb/UsbDeviceConnection", "close", "()V");
            set_status(NET_SEARCHING, "THAT USB DEVICE CAN'T LINK"); goto out;
        }
        const char *str[6] = { "Ari", "Greenscreen89Link", "Greenscreen 89 link cable", "1", "", "GS89" };
        for (int k = 0; k < 6; k++) ctrl(fd, 0x40, 52, 0, k, (void *)str[k], (int)strlen(str[k]) + 1);
        ctrl(fd, 0x40, 53, 0, 0, NULL, 0);
        jcall_obj(e, conn, "android/hardware/usb/UsbDeviceConnection", "close", "()V");
        ustate = U_SWITCHING; u_since = now; u_asked = 0;
        set_status(NET_SEARCHING, "SWITCHING THE OTHER PHONE...");
    }
out:
    (*e)->PopLocalFrame(e, NULL);
}

int net_start_usb(JavaVM *vm, jobject activity) {
    net_stop();
    jvm = vm; j_act = activity;
    if (wake_pipe[0] < 0) {
        if (pipe(wake_pipe) == 0) {
            fcntl(wake_pipe[0], F_SETFL, fcntl(wake_pipe[0], F_GETFL) | O_NONBLOCK);
            fcntl(wake_pipe[1], F_SETFL, fcntl(wake_pipe[1], F_GETFL) | O_NONBLOCK);
        }
    }
    rx_h = rx_t = 0;
    kind = NET_KIND_USB;
    ustate = U_SCAN; u_asked = 0; u_nbad = 0; u_peer_seen = 0; u_failed = 0;
    u_started = mono_ns(); u_next = 0; u_last_hello = 0;
    set_status(NET_SEARCHING, "PLUG THE PHONES TOGETHER...");
    return 0;
}

static void usb_close(void) {
    u_gen++;                                /* stale reader threads stop delivering */
    JNIEnv *e = genv();
    if (e) {
        if (j_conn) { jcall_obj(e, j_conn, "android/hardware/usb/UsbDeviceConnection", "close", "()V"); (*e)->DeleteGlobalRef(e, j_conn); j_conn = NULL; }
        if (j_pfd) { jcall_obj(e, j_pfd, "android/os/ParcelFileDescriptor", "close", "()V"); (*e)->DeleteGlobalRef(e, j_pfd); j_pfd = NULL; }
    }
    u_fd = -1;
    ustate = U_SCAN;
}

static void usb_poll(int64_t now) {
    if (ustate == U_DEAD || u_failed) return;
    if (ustate == U_OPEN) {
        if (!u_peer_seen) {
            if (now - u_last_hello > 200000000LL) { usb_hello(); u_last_hello = now; }
            if (now - u_since > 8000000000LL) { usb_close(); set_status(NET_SEARCHING, "NO ANSWER - IS THE GAME OPEN ON BOTH?"); }
        } else if (net_state() != NET_CONNECTED) {
            usb_hello();
            set_status(NET_CONNECTED, "LINKED BY CABLE");
        }
        return;
    }
    if (now >= u_next) { u_next = now + 400000000LL; usb_step(now); }
}

/* ------------------------------------------------------------- common */
void net_stop(void) {
    if (ufd >= 0) { close(ufd); ufd = -1; }
    if (kind == NET_KIND_ONLINE) {
        /* tell the other player right away (the link layer's BYE may not get through the broker in time) */
        if (paired && j_out) { JNIEnv *e = genv(); char m[40]; snprintf(m, sizeof m, "N %s", my_hex); if (e) mq_publish_text(e, t_peer, m); }
        ws_stop = 1;
        /* closing the socket unblocks the reader */
        if (j_sock && genv()) {
            pthread_mutex_lock(&wmu);
            if (j_sock) {
                jclass cSock = (*game_env)->FindClass(game_env, "java/net/Socket");
                (*game_env)->CallVoidMethod(game_env, j_sock, (*game_env)->GetMethodID(game_env, cSock, "close", "()V"));
                jexc(game_env);
            }
            pthread_mutex_unlock(&wmu);
        }
        if (ws_running || ws_th) { pthread_join(ws_th, NULL); }
        ws_th = 0; ws_running = 0;
        if (o_fd >= 0) { close(o_fd); o_fd = -1; }
        paired = 0; direct_ok = 0;
    }
    if (kind == NET_KIND_USB) usb_close();
    kind = NET_KIND_NONE;
    set_status(NET_OFF, "");
}

static JNIEnv *genv(void) {
    if (!game_env && jvm) {
        if ((*jvm)->AttachCurrentThread(jvm, &game_env, NULL) != JNI_OK) game_env = NULL;
    }
    return game_env;
}

void net_send(const uint8_t *p, int n) {
    uint8_t m[64];
    if (n > 48) return;
    memcpy(m, MAGIC, 4); m[4] = 'D'; memcpy(m + 5, p, (size_t)n);
    if (kind == NET_KIND_LAN) {
        if (ufd >= 0 && peer_confirmed)
            sendto(ufd, m, (size_t)n + 5, 0, (struct sockaddr *)&peer_addr, sizeof peer_addr);
    } else if (kind == NET_KIND_ONLINE) {
        if (!paired || net_state() != NET_CONNECTED) return;
        if (direct_ok) { online_udp_send(&direct_addr, 'D', p, n); return; }
        JNIEnv *e = genv();
        if (e) mq_publish(e, t_peer, m, n + 5);
    } else if (kind == NET_KIND_USB) {
        if (net_state() == NET_CONNECTED) usb_write(m, n + 5);
    }
}

int net_recv(uint8_t *buf, int max) {
    if (kind == NET_KIND_LAN) {
        if (ufd < 0) return 0;
        for (;;) {
            uint8_t m[64];
            struct sockaddr_in from; socklen_t fl = sizeof from;
            ssize_t g = recvfrom(ufd, m, sizeof m, 0, (struct sockaddr *)&from, &fl);
            if (g <= 0) return 0;
            if (g < 5 || memcmp(m, MAGIC, 4) != 0) continue;
            if (m[4] == 'H' && g >= 13) {
                uint32_t id = get32(m + 5), seen = get32(m + 9);
                if (id == my_id) continue;                         /* our own broadcast */
                if (!peer_id) { peer_id = id; peer_addr = from; peer_addr.sin_port = htons((uint16_t)send_port); lan_hello(mono_ns()); }
                if (id == peer_id) {
                    peer_addr = from; peer_addr.sin_port = htons((uint16_t)send_port);
                    if (seen == my_id && !peer_confirmed) { peer_confirmed = 1; set_status(NET_CONNECTED, "LINKED ON WI-FI"); }
                    if (seen != my_id) lan_hello(mono_ns());       /* make sure they hear us */
                }
                continue;
            }
            if (m[4] == 'D' && peer_confirmed && from.sin_addr.s_addr == peer_addr.sin_addr.s_addr) {
                int n = (int)g - 5; if (n > max) n = max;
                memcpy(buf, m + 5, (size_t)n);
                return n;
            }
        }
    } else if (kind == NET_KIND_ONLINE || kind == NET_KIND_USB) {
        if (kind == NET_KIND_ONLINE) { int u = online_udp_recv(buf, max); if (u > 0) return u; }
        int n = 0;
        pthread_mutex_lock(&rmu);
        if (rx_h != rx_t) { n = rxq[rx_h].n < max ? rxq[rx_h].n : max; memcpy(buf, rxq[rx_h].d, (size_t)n); rx_h = (rx_h + 1) % RXQ; }
        pthread_mutex_unlock(&rmu);
        if (!n && wake_pipe[0] >= 0) { char t[64]; while (read(wake_pipe[0], t, sizeof t) > 0) {} }
        return n;
    }
    return 0;
}

void net_poll(int64_t now) {
    if (kind == NET_KIND_USB) { usb_poll(now); return; }
    if (kind == NET_KIND_ONLINE) { online_poll(now); return; }
    if (kind == NET_KIND_LAN && ufd >= 0 && !peer_confirmed) {
        uint8_t junk[64];
        while (net_recv(junk, sizeof junk) > 0) {}        /* handles HELLOs while pairing */
        if (!peer_confirmed && now - last_hello > 250000000LL) lan_hello(now);
    }
}

void net_wait(int64_t timeout_ns) {
    if (timeout_ns <= 0) return;
    int fd = kind == NET_KIND_LAN ? ufd : (kind == NET_KIND_ONLINE || kind == NET_KIND_USB) ? wake_pipe[0] : -1;
    int ms = (int)(timeout_ns / 1000000LL);
    if (fd < 0 || ms <= 0) {
        struct timespec ts = { 0, (long)timeout_ns };
        nanosleep(&ts, NULL);
        return;
    }
    struct pollfd p[2] = { { fd, POLLIN, 0 }, { kind == NET_KIND_ONLINE ? o_fd : -1, POLLIN, 0 } };
    poll(p, p[1].fd >= 0 ? 2 : 1, ms);
}

void net_thread_exit(void) {
    if (game_env && jvm) { (*jvm)->DetachCurrentThread(jvm); game_env = NULL; }
}

/* read the clipboard's text (the user copies their relay address) */
int net_clipboard_text(JavaVM *vm, jobject activity, char *out, int max) {
    out[0] = 0;
    if (!jvm) jvm = vm;
    JNIEnv *e = genv();
    if (!e || !activity) return -1;
    int ok = -1;
    jclass cCtx = (*e)->FindClass(e, "android/content/Context");
    jmethodID gss = cCtx ? (*e)->GetMethodID(e, cCtx, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;") : NULL;
    if (jexc(e) || !gss) return -1;
    jstring name = (*e)->NewStringUTF(e, "clipboard");
    jobject cm = (*e)->CallObjectMethod(e, activity, gss, name);
    if (jexc(e) || !cm) return -1;
    jclass cCM = (*e)->FindClass(e, "android/content/ClipboardManager");
    jmethodID gpc = cCM ? (*e)->GetMethodID(e, cCM, "getPrimaryClip", "()Landroid/content/ClipData;") : NULL;
    if (jexc(e) || !gpc) return -1;
    jobject clip = (*e)->CallObjectMethod(e, cm, gpc);
    if (jexc(e) || !clip) return -1;
    jclass cCD = (*e)->FindClass(e, "android/content/ClipData");
    jmethodID gic = (*e)->GetMethodID(e, cCD, "getItemCount", "()I");
    jmethodID gia = (*e)->GetMethodID(e, cCD, "getItemAt", "(I)Landroid/content/ClipData$Item;");
    if (jexc(e) || !gic || !gia) return -1;
    if ((*e)->CallIntMethod(e, clip, gic) < 1 || jexc(e)) return -1;
    jobject item = (*e)->CallObjectMethod(e, clip, gia, 0);
    if (jexc(e) || !item) return -1;
    jclass cIt = (*e)->FindClass(e, "android/content/ClipData$Item");
    jmethodID gt = (*e)->GetMethodID(e, cIt, "getText", "()Ljava/lang/CharSequence;");
    if (jexc(e) || !gt) return -1;
    jobject cs = (*e)->CallObjectMethod(e, item, gt);
    if (jexc(e) || !cs) return -1;
    jclass cObj = (*e)->FindClass(e, "java/lang/Object");
    jstring str = (jstring)(*e)->CallObjectMethod(e, cs, (*e)->GetMethodID(e, cObj, "toString", "()Ljava/lang/String;"));
    if (jexc(e) || !str) return -1;
    const char *u = (*e)->GetStringUTFChars(e, str, NULL);
    if (u) { snprintf(out, (size_t)max, "%s", u); (*e)->ReleaseStringUTFChars(e, str, u); ok = 0; }
    return ok;
}

/* "mqtts://broker.example.org:8883" -> "broker.example.org:8883" (keeps :port) */
int net_parse_host(const char *in, char *out, int max) {
    const char *p = in;
    while (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r') p++;
    if (!strncmp(p, "https://", 8)) p += 8;
    else if (!strncmp(p, "wss://", 6)) p += 6;
    else if (!strncmp(p, "http://", 7)) p += 7;
    else if (!strncmp(p, "mqtts://", 8)) p += 8;
    else if (!strncmp(p, "ssl://", 6)) p += 6;
    int n = 0;
    while (p[n] && p[n] != '/' && p[n] != '?' && p[n] != ' ' && p[n] != '\n' && p[n] != '\r' && n < max - 1) {
        char c = p[n];
        int okc = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == ':';
        if (!okc) return -1;
        out[n] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
        n++;
    }
    out[n] = 0;
    if (n < 4 || !strchr(out, '.')) return -1;
    return 0;
}
