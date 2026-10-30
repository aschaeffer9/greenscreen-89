/* Transports for the link cable: same-Wi-Fi UDP (auto pairing) and an
 * online mode (open-source MQTT broker matchmaking + direct UDP), and USB. */
#ifndef NET_H
#define NET_H
#include <stdint.h>
#include <jni.h>

enum { NET_OFF, NET_SEARCHING, NET_CONNECTED, NET_FAILED };
enum { NET_KIND_NONE, NET_KIND_LAN, NET_KIND_ONLINE, NET_KIND_USB };

int  net_start_lan(void);
int  net_start_online(JavaVM *vm, const char *broker);   /* "" = built-in public brokers */
int  net_online_direct(void);        /* 1 when the online link runs phone-to-phone */
void net_test_plain_brokers(int on);   /* tests: skip STUN */
void net_test_no_direct(int on);      /* tests: broker relay only */
void net_test_block_udp(int on);
int  net_start_usb(JavaVM *vm, jobject activity);   /* phone-to-phone USB-C cable */
void net_stop(void);
int  net_state(void);
int  net_kind(void);
const char *net_status_text(void);   /* short uppercase message for the UI */
void net_poll(int64_t now);          /* pairing / keepalive work (game thread) */
void net_send(const uint8_t *p, int n);
int  net_recv(uint8_t *buf, int max); /* non-blocking; returns length or 0 */
/* wait until a packet may be available or timeout_ns passes */
void net_wait(int64_t timeout_ns);

void net_thread_exit(void);   /* game thread: detach from the JVM before exiting */
int net_clipboard_text(JavaVM *vm, jobject activity, char *out, int max);
int net_parse_host(const char *in, char *out, int max);
#endif
