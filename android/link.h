/* Virtual Game Boy link cable over a datagram transport.
 * Each side runs its own Game Boy. When a game starts a transfer as the
 * clock master, the byte is sent to the peer; the peer's Game Boy receives
 * it as soon as it is armed (SC=$80), and its outgoing byte travels back to
 * complete the master's transfer - exactly what the cable does, plus lag.
 * Packets are retransmitted until acknowledged, delivered once, in order,
 * and no faster than the sending Game Boy produced them (each transfer
 * carries the time since the previous one), so a game that reads the cable
 * once per frame never has a byte overwritten before it looks. */
#ifndef LINK_H
#define LINK_H
#include <stdint.h>

typedef struct {
    int (*master_waiting)(void);
    void (*master_complete)(uint8_t);
    int (*passive_ready)(void);
    uint8_t (*passive_exchange)(uint8_t);
    uint64_t (*cycles)(void);
} LinkCore;

typedef void (*LinkSendFn)(void *ctx, const uint8_t *pkt, int n);

#define LINK_WIN 128
typedef struct {
    LinkCore core;
    LinkSendFn send; void *send_ctx;
    struct { uint16_t seq; uint8_t b, ff, active; uint16_t gap; int64_t sent_at; } out[LINK_WIN];
    uint64_t last_start_cyc;          /* master side: when our previous transfer started */
    uint64_t last_deliver_cyc;        /* passive side: when we last handed a byte to our Game Boy */
    uint16_t rx_gap[256];
    uint16_t tx_seq;
    int waiting; uint16_t wait_seq;
    uint16_t rx_next;
    uint8_t rx_have[256], rx_byte[256];
    uint8_t rep_have[256], rep_byte[256]; uint16_t rep_seq[256];
    int64_t head_since;
    int64_t last_heard, last_sent;
    /* stats */
    unsigned xfers_out, xfers_in, retransmits, dups;
    unsigned garb_sent, garb_recv;
    int peer_bye;                    /* the other side unlinked on purpose */   /* bytes $81/$82/$84 (2P garbage) sent / received by this side */
} Link;

enum { LINK_PKT_XFER = 1, LINK_PKT_REPLY = 2, LINK_PKT_PING = 3, LINK_PKT_BYE = 4 };

void link_init(Link *l, LinkCore core, LinkSendFn send, void *ctx, int64_t now);
/* call from the core's link callback (a transfer was started as master) */
void link_master_start(Link *l, uint8_t byte_out, int fire_and_forget, int64_t now);
/* feed one received packet; returns 0 if it wasn't a link packet */
int link_on_packet(Link *l, const uint8_t *p, int n, int64_t now);
/* call often (between emulation slices) */
void link_poll(Link *l, int64_t now);
/* ms since we last heard anything from the peer */
int64_t link_silence_ns(const Link *l, int64_t now);
/* tell the other side we're leaving (a few copies, it's UDP) */
void link_send_bye(Link *l, int64_t now);
#endif
