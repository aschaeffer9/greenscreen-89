/* Link-cable transports on iOS: not in this build.
 *
 * The Wi-Fi mode used UDP broadcast for pairing, which iOS gates behind the
 * multicast entitlement, USB-C accessory mode has no iOS equivalent, and the
 * online mode needs an MQTT-over-TLS stack (the Android build borrows Java's
 * SSLSocket through JNI). The menu still opens; it fails fast with a message
 * instead of searching forever. */

#include <stdio.h>
#include <string.h>

#include "net.h"
#include "ios_glue.h"

static int state = NET_OFF;
static int kind = NET_KIND_NONE;
static char status[64] = "";

static int refuse(int k, const char *why) {
    kind = k;
    state = NET_FAILED;
    snprintf(status, sizeof status, "%s", why);
    return -1;
}

int net_start_lan(void) { return refuse(NET_KIND_LAN, "NO LINK IN THIS BUILD"); }
int net_start_online(JavaVM *vm, const char *broker) { (void)vm; (void)broker; return refuse(NET_KIND_ONLINE, "NO ONLINE IN THIS BUILD"); }
int net_start_usb(JavaVM *vm, jobject activity) { (void)vm; (void)activity; return refuse(NET_KIND_USB, "NO USB LINK ON IOS"); }

int net_presence_start(JavaVM *vm, const char *broker) { (void)vm; (void)broker; return -1; }
void net_presence_stop(void) {}
void net_presence_set_busy(int busy) { (void)busy; }
int net_presence_available(void) { return -1; }
int net_invite_pending(void) { return 0; }
void net_invite_answer(int accept) { (void)accept; }

void net_test_search_timeout(int seconds) { (void)seconds; }
int net_online_direct(void) { return 0; }
void net_test_plain_brokers(int on) { (void)on; }
void net_test_no_direct(int on) { (void)on; }
void net_test_block_udp(int on) { (void)on; }

void net_stop(void) { state = NET_OFF; kind = NET_KIND_NONE; status[0] = 0; }
int net_state(void) { return state; }
int net_kind(void) { return kind; }
const char *net_status_text(void) { return status; }

void net_poll(int64_t now) { (void)now; }
void net_send(const uint8_t *p, int n) { (void)p; (void)n; }
int net_recv(uint8_t *buf, int max) { (void)buf; (void)max; return 0; }
void net_wait(int64_t timeout_ns) { (void)timeout_ns; }
void net_thread_exit(void) {}

int net_clipboard_text(JavaVM *vm, jobject activity, char *out, int max) {
    (void)vm; (void)activity;
    return ios_clipboard(out, max);
}

/* "host", "host:port", "mqtts://host:port" -> host */
int net_parse_host(const char *in, char *out, int max) {
    if (!in || !out || max <= 0) return -1;
    while (*in == ' ') in++;
    const char *p = strstr(in, "://");
    if (p) in = p + 3;
    int n = 0;
    while (*in && *in != ':' && *in != '/' && *in != ' ' && n < max - 1) out[n++] = *in++;
    out[n] = 0;
    return n > 0 ? 0 : -1;
}
