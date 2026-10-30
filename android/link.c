#include <string.h>
#include "link.h"

#define RTO_NS        60000000LL     /* resend unacknowledged transfers after 60 ms */
#define PING_NS      250000000LL     /* keepalive when idle */
#define UNARMED_NS  3000000000LL     /* peer never armed: behave like no cable ($FF) */

static int is_garb(uint8_t b) { return b == 0x81 || b == 0x82 || b == 0x84; }
static int seq_lt(uint16_t a, uint16_t b) { return (int16_t)(a - b) < 0; }

static void tx(Link *l, const uint8_t *p, int n, int64_t now) { l->send(l->send_ctx, p, n); l->last_sent = now; }

void link_init(Link *l, LinkCore core, LinkSendFn send, void *ctx, int64_t now) {
    memset(l, 0, sizeof *l);
    l->core = core; l->send = send; l->send_ctx = ctx;
    l->last_heard = l->last_sent = now;
}

static void send_xfer(Link *l, int i, int64_t now) {
    uint8_t p[7] = { LINK_PKT_XFER, (uint8_t)l->out[i].seq, (uint8_t)(l->out[i].seq >> 8), l->out[i].b, l->out[i].ff,
                     (uint8_t)l->out[i].gap, (uint8_t)(l->out[i].gap >> 8) };
    l->out[i].sent_at = now;
    tx(l, p, 7, now);
}

void link_master_start(Link *l, uint8_t b, int ff, int64_t now) {
    int slot = -1;
    for (int i = 0; i < LINK_WIN; i++) if (!l->out[i].active) { slot = i; break; }
    if (slot < 0) {   /* window full: drop the oldest (should never happen) */
        slot = 0;
        for (int i = 1; i < LINK_WIN; i++) if (seq_lt(l->out[i].seq, l->out[slot].seq)) slot = i;
    }
    uint16_t seq = l->tx_seq++;
    uint64_t c = l->core.cycles();
    uint64_t gap = (c - l->last_start_cyc) / 16;           /* in 16-cycle units */
    if (seq == 0 || gap > 70224 / 16) gap = 70224 / 16;    /* never need more than a frame apart */
    l->last_start_cyc = c;
    l->out[slot].seq = seq; l->out[slot].b = b; l->out[slot].ff = (uint8_t)ff; l->out[slot].active = 1;
    l->out[slot].gap = (uint16_t)gap;
    l->xfers_out++;
    if (is_garb(b)) l->garb_sent++;
    send_xfer(l, slot, now);
    if (ff) {
        /* the game ignores this reply: complete now (after the usual bit-time) */
        l->waiting = 0;
        l->core.master_complete(0x00);
    } else {
        l->waiting = 1; l->wait_seq = seq;
    }
}

static void send_reply(Link *l, uint16_t seq, uint8_t b, int64_t now) {
    uint8_t p[4] = { LINK_PKT_REPLY, (uint8_t)seq, (uint8_t)(seq >> 8), b };
    tx(l, p, 4, now);
}

int link_on_packet(Link *l, const uint8_t *p, int n, int64_t now) {
    if (n < 1) return 0;
    switch (p[0]) {
    case LINK_PKT_XFER: {
        if (n < 7) return 0;
        uint16_t seq = (uint16_t)(p[1] | p[2] << 8);
        l->last_heard = now;
        if (seq_lt(seq, l->rx_next)) {          /* already delivered: our reply was lost */
            int k = seq & 255;
            if (l->rep_have[k] && l->rep_seq[k] == seq) send_reply(l, seq, l->rep_byte[k], now);
            l->dups++;
            return 1;
        }
        if ((uint16_t)(seq - l->rx_next) >= 200) return 1;   /* absurdly far ahead */
        int k = seq & 255;
        if (!l->rx_have[k]) {
            l->rx_have[k] = 1; l->rx_byte[k] = p[3]; l->rx_gap[k] = (uint16_t)(p[5] | p[6] << 8);
            if (seq == l->rx_next) l->head_since = now;
        } else l->dups++;
        return 1;
    }
    case LINK_PKT_REPLY: {
        if (n < 4) return 0;
        uint16_t seq = (uint16_t)(p[1] | p[2] << 8);
        l->last_heard = now;
        for (int i = 0; i < LINK_WIN; i++)
            if (l->out[i].active && l->out[i].seq == seq) l->out[i].active = 0;
        if (l->waiting && seq == l->wait_seq) {
            l->waiting = 0;
            if (l->core.master_waiting()) { l->core.master_complete(p[3]); if (is_garb(p[3])) l->garb_recv++; }
        }
        return 1;
    }
    case LINK_PKT_PING:
        l->last_heard = now;
        return 1;
    case LINK_PKT_BYE:
        l->last_heard = now;
        l->peer_bye = 1;
        return 1;
    }
    return 0;
}

void link_poll(Link *l, int64_t now) {
    /* the game may have aborted/restarted its transfer */
    if (l->waiting && !l->core.master_waiting()) l->waiting = 0;
    /* hand the next received byte to our Game Boy once it is armed */
    int k = l->rx_next & 255;
    if (l->rx_have[k] && l->core.cycles() - l->last_deliver_cyc >= (uint64_t)l->rx_gap[k] * 16) {
        int deliver = l->core.passive_ready();
        if (deliver || now - l->head_since > UNARMED_NS) {
            uint8_t reply = deliver ? l->core.passive_exchange(l->rx_byte[k]) : 0xFF;
            if (deliver && is_garb(l->rx_byte[k])) l->garb_recv++;
            if (is_garb(reply)) l->garb_sent++;
            l->rx_have[k] = 0;
            l->rep_have[k] = 1; l->rep_seq[k] = l->rx_next; l->rep_byte[k] = reply;
            send_reply(l, l->rx_next, reply, now);
            l->rx_next++;
            l->xfers_in++;
            l->head_since = now;
            l->last_deliver_cyc = l->core.cycles();
        }
    }
    /* retransmit */
    for (int i = 0; i < LINK_WIN; i++)
        if (l->out[i].active && now - l->out[i].sent_at > RTO_NS) { l->retransmits++; send_xfer(l, i, now); }
    if (now - l->last_sent > PING_NS) { uint8_t p = LINK_PKT_PING; tx(l, &p, 1, now); }
}

int64_t link_silence_ns(const Link *l, int64_t now) { return now - l->last_heard; }

void link_send_bye(Link *l, int64_t now) {
    uint8_t p = LINK_PKT_BYE;
    for (int i = 0; i < 3; i++) tx(l, &p, 1, now);
}
