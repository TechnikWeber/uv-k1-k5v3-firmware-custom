/* Copyright 2026 Philipp Weber (DL4PW)
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 */

#include <stddef.h>
#include <string.h>

#include "mailbox.h"
#include "messenger.h"
#include "msg_flash.h"

/* ---- settings: a small log of 64-byte records in the first sector ---- */
#define CFG_REC        64u
#define CFG_SLOTS      (MSG_FLASH_SECTOR / CFG_REC)
#define CFG_MAGIC      0xC7u
#define CFG_VERSION    2u

typedef struct {
    uint8_t      magic;
    uint8_t      version;
    uint8_t      crc;
    uint8_t      pad;
    msg_config_t cfg;
} cfg_rec_t;

_Static_assert(sizeof(cfg_rec_t) <= CFG_REC, "settings record fits its slot");

/* ---- transmit scheduling ---- */
#define HOLDOFF_MIN_MS     700u     /* random wait once the channel is clear */
#define HOLDOFF_SPAN_MS    2300u
#define TX_GAP_MS          3000u    /* between two of our own frames        */
#define TOKENS_MAX         6u       /* burst budget, one token per frame    */
#define TOKEN_REFILL_MS    30000u   /* ... refilled one per 30 s            */
#define FIRST_BEACON_MS    20000u   /* after boot or after switching on     */
#define NO_VFO_RETRY_MS    30000u
#define DEDUPE_WINDOW_MS   (15u * 60u * 1000u)
#define COPY_WINDOW_MS     20000u   /* a copy through another digipeater    */
#define BLN_WINDOW_MS      (6u * 3600u * 1000u)   /* bulletins are repeated for hours */
#define BCAST_REPEAT_MS    60000u   /* the second transmission of a broadcast */
#define DEDUPE_SLOTS       16u
#define ACK_QUEUE          4u

/* seconds to wait after transmission n (1-based) of a message; after the
 * last one, the time left for its ack */
static const uint16_t RETRY_S[MSG_RETRIES] = {30, 60, 120, 240, 120};
static const uint8_t  BEACON_MIN[MSG_BEACON_COUNT] = {0, 10, 20, 30, 60};
static const char *const BEACON_TXT[MSG_BEACON_COUNT] = {"OFF", "10", "20", "30", "60"};

typedef struct {
    bool     active;
    uint8_t  tries;
    int16_t  slot;
    uint32_t due;
    char     peer[AX25_CALL_STR];
    char     id[APRS_ID_MAX + 1];
} pending_t;

typedef struct {
    char peer[AX25_CALL_STR];
    char id[APRS_ID_MAX + 1];
} ack_t;

typedef struct {
    uint32_t hash;
    uint32_t at;
} seen_t;

typedef struct {
    bool     active;
    uint8_t  sends;
    int16_t  slot;
    uint32_t due;
    char     to[APRS_ADDRESSEE_LEN + 1];   /* "BLN3FAMIL" */
} bcast_t;

static msg_config_t    gCfg;
static bcast_t         gBc[2];
static int16_t         gCfgSlot = -1;
static pending_t       gOut[MSG_OUTBOX_MAX];
static ack_t           gAck[ACK_QUEUE];
static uint8_t         gAckCount;
static seen_t          gSeen[DEDUPE_SLOTS];
static uint8_t         gSeenNext;
static uint32_t        gBeaconDue;
static uint32_t        gHoldoffAt;           /* 0: not armed            */
static uint32_t        gLastTx;
static bool            gEverTx;
static uint32_t        gTokens;              /* thousandths              */
static uint32_t        gTokenAt;
static msg_tx_result_t gLastResult;
static uint16_t        gHeard, gSent;
static bool            gWasEnabled;

static uint8_t         gFrame[AX25_FRAME_MAX];
static char            gInfo[APRS_TEXT_MAX + 32u];

/* ---------------------------------------------------------------- utils -- */

static bool time_reached(uint32_t now, uint32_t at)
{
    return (int32_t)(now - at) >= 0;
}

static uint8_t crc8(const void *data, size_t len)
{
    const uint8_t *p = data;
    uint8_t c = 0;
    while (len--) {
        c ^= *p++;
        for (uint8_t k = 0; k < 8u; k++)
            c = (c & 0x80u) ? (uint8_t)((c << 1) ^ 0x07u) : (uint8_t)(c << 1);
    }
    return c;
}

static uint32_t fnv(uint32_t h, const char *s)
{
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 16777619u;
    }
    return h * 16777619u;              /* separator */
}

/* "DL1ABC-9" and "DL1ABC": the same station for an ack */
static bool same_base_call(const char *a, const char *b)
{
    while (*a && *a != '-' && *a == *b) {
        a++;
        b++;
    }
    return (*a == '\0' || *a == '-') && (*b == '\0' || *b == '-');
}

/* ------------------------------------------------------------- settings -- */

void MSG_ConfigDefaults(msg_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->enabled = 0;
    cfg->path    = MSG_PATH_WIDE1_WIDE2;
    cfg->beacon  = MSG_BEACON_30;
    cfg->beep    = MSG_BEEP_ALL;
    cfg->audio   = 0;
    cfg->freq    = MSG_DEFAULT_FREQ;
    cfg->next_id = 1;
    strcpy(cfg->status, "UV-K5 messenger");
}

static bool group_valid(const char *g)
{
    uint8_t n = 0;
    for (; n <= MSG_GROUP_LEN && g[n]; n++)
        if (!((g[n] >= 'A' && g[n] <= 'Z') || (g[n] >= '0' && g[n] <= '9')))
            return false;
    return n <= MSG_GROUP_LEN;
}

static bool cfg_valid(const msg_config_t *c)
{
    ax25_addr_t a;
    if (c->call[0] && !ax25_addr_parse(c->call, &a))
        return false;
    if (!group_valid(c->group) || c->beep >= MSG_BEEP_COUNT || c->bulletins > 1u
        || c->next_bln > 9u)
        return false;
    return c->ssid <= 15u && c->path < MSG_PATH_COUNT && c->beacon < MSG_BEACON_COUNT
        && c->freq >= 1800000u && c->freq <= 130000000u
        && c->next_id >= 1u && c->next_id <= 999u
        && memchr(c->status, '\0', sizeof c->status) != NULL;
}

static void cfg_load(void)
{
    MSG_ConfigDefaults(&gCfg);
    gCfgSlot = -1;
    for (int16_t s = 0; s < (int16_t)CFG_SLOTS; s++) {
        cfg_rec_t r;
        msg_flash_read(MSG_CONFIG_ADDR + (uint32_t)s * CFG_REC, &r, sizeof r);
        if (r.magic == 0xFFu)
            break;                     /* the log is written in order */
        if (r.magic != CFG_MAGIC || r.version != CFG_VERSION
            || r.crc != crc8(&r.cfg, sizeof r.cfg) || !cfg_valid(&r.cfg))
            continue;
        gCfg = r.cfg;
        gCfgSlot = s;
    }
    /* a slot written after the last good one (cut by a power loss) */
    for (int16_t s = (int16_t)(gCfgSlot + 1); s < (int16_t)CFG_SLOTS; s++) {
        uint8_t m;
        msg_flash_read(MSG_CONFIG_ADDR + (uint32_t)s * CFG_REC, &m, 1);
        if (m != 0xFFu)
            gCfgSlot = s;              /* skip it when writing */
    }
}

static void cfg_save(void)
{
    cfg_rec_t r;
    memset(&r, 0xFF, sizeof r);
    r.version = CFG_VERSION;
    r.pad     = 0;
    r.cfg     = gCfg;
    r.crc     = crc8(&r.cfg, sizeof r.cfg);

    int16_t slot = (int16_t)(gCfgSlot + 1);
    if (slot >= (int16_t)CFG_SLOTS || slot < 0) {
        msg_flash_erase(MSG_CONFIG_ADDR);
        slot = 0;
    }
    const uint32_t addr = MSG_CONFIG_ADDR + (uint32_t)slot * CFG_REC;
    msg_flash_program(addr + 1u, (const uint8_t *)&r + 1u, sizeof r - 1u);
    const uint8_t magic = CFG_MAGIC;
    msg_flash_program(addr, &magic, 1);
    gCfgSlot = slot;
}

const msg_config_t *MSG_GetConfig(void)
{
    return &gCfg;
}

void MSG_SetConfig(const msg_config_t *cfg)
{
    if (!cfg_valid(cfg))
        return;
    gCfg = *cfg;
    cfg_save();
}

bool MSG_MyAddr(ax25_addr_t *out)
{
    if (!gCfg.call[0] || !ax25_addr_parse(gCfg.call, out))
        return false;
    out->ssid = gCfg.ssid;
    return true;
}

const char *MSG_BeaconMinutesText(uint8_t beacon)
{
    return beacon < MSG_BEACON_COUNT ? BEACON_TXT[beacon] : "?";
}

uint8_t MSG_BeaconMinutes(uint8_t beacon)
{
    return beacon < MSG_BEACON_COUNT ? BEACON_MIN[beacon] : 0;
}

/* ----------------------------------------------------------------- init -- */

void MSG_CoreInit(void)
{
    cfg_load();
    mbx_init();
    memset(gOut, 0, sizeof gOut);
    memset(gBc, 0, sizeof gBc);
    memset(gSeen, 0, sizeof gSeen);
    gAckCount   = 0;
    gHoldoffAt  = 0;
    gEverTx     = false;
    gTokens     = TOKENS_MAX * 1000u;
    gTokenAt    = msg_port_ms();
    gLastResult = MSG_TX_OK;
    gHeard = gSent = 0;
    gWasEnabled = false;
}

/* ------------------------------------------------------------- receive -- */

typedef enum {
    SEEN_NEW = 0,       /* first copy: store and ack                          */
    SEEN_COPY,          /* a digipeater's copy a few seconds later: nothing   */
    SEEN_RETRY,         /* the sender tries again, our ack got lost: ack again */
} seen_t_kind;

/* Copies of one message arrive through several digipeaters within seconds;
 * the sender's own retries come 30 s or more apart. */
static seen_t_kind seen_check(const char *from, const char *id, const char *text, uint32_t now,
                              uint32_t window)
{
    uint32_t h = fnv(2166136261u, from);
    h = fnv(h, id);
    if (!id[0])
        h = fnv(h, text);              /* no number: the text tells copies apart */
    const uint32_t stamp = now ? now : 1u;
    for (uint8_t i = 0; i < DEDUPE_SLOTS; i++) {
        if (gSeen[i].at && gSeen[i].hash == h && now - gSeen[i].at < window) {
            const bool copy = now - gSeen[i].at < COPY_WINDOW_MS;
            gSeen[i].at = stamp;
            return copy ? SEEN_COPY : SEEN_RETRY;
        }
    }
    gSeen[gSeenNext].hash = h;
    gSeen[gSeenNext].at   = stamp;
    gSeenNext = (uint8_t)((gSeenNext + 1u) % DEDUPE_SLOTS);
    return SEEN_NEW;
}

static void queue_ack(const char *peer, const char *id)
{
    for (uint8_t i = 0; i < gAckCount; i++)
        if (strcmp(gAck[i].peer, peer) == 0 && strcmp(gAck[i].id, id) == 0)
            return;
    if (gAckCount == ACK_QUEUE) {      /* drop the oldest */
        memmove(&gAck[0], &gAck[1], sizeof(ack_t) * (ACK_QUEUE - 1u));
        gAckCount--;
    }
    strcpy(gAck[gAckCount].peer, peer);
    strcpy(gAck[gAckCount].id, id);
    gAckCount++;
}

static void outcome(const char *peer, const char *id, uint8_t flag)
{
    for (uint8_t i = 0; i < MSG_OUTBOX_MAX; i++) {
        pending_t *p = &gOut[i];
        if (p->active && strcmp(p->id, id) == 0 && same_base_call(p->peer, peer)) {
            mbx_set_flags(p->slot, flag);
            p->active = false;
            return;
        }
    }
    /* a late ack, after the last retry */
    char exact[AX25_CALL_STR];
    strcpy(exact, peer);
    int16_t s = mbx_find_outgoing(exact, id);
    if (s == MBX_NONE) {
        /* sent to the base call, acked from another SSID (or the reverse) */
        char *dash = strchr(exact, '-');
        if (dash) {
            *dash = '\0';
            s = mbx_find_outgoing(exact, id);
        }
    }
    if (s != MBX_NONE)
        mbx_set_flags(s, flag);
}

static char upper(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
}

/* "BLN" + line (0-9: bulletin, A-Z: announcement) + optional group of up to
 * 5 characters. Returns true when the frame was a bulletin (kept or not). */
static bool handle_bulletin(const aprs_msg_t *m, const ax25_addr_t *me, uint32_t now)
{
    const char *to = m->to;
    if (upper(to[0]) != 'B' || upper(to[1]) != 'L' || upper(to[2]) != 'N' || !to[3])
        return false;

    char group[MSG_GROUP_LEN + 1];
    uint8_t n = 0;
    for (const char *g = to + 4; *g && n < MSG_GROUP_LEN; g++)
        group[n++] = upper(*g);
    group[n] = '\0';

    msg_notify_t kind;
    if (group[0]) {
        if (!gCfg.group[0] || strcmp(group, gCfg.group) != 0)
            return true;               /* another group */
        kind = MSG_NOTIFY_CHANNEL;
    } else {
        if (!gCfg.bulletins)
            return true;
        kind = MSG_NOTIFY_PUBLIC;
    }
    if (ax25_addr_equal(&m->from, me) || !m->text[0])
        return true;

    char from[AX25_CALL_STR];
    ax25_addr_format(&m->from, from);
    char key[AX25_CALL_STR + MSG_GROUP_LEN + 2];   /* "DL1ABC-9#GROUP": no number, */
    strcpy(key, from);                              /* so the text tells them apart */
    strcat(key, "#");
    strcat(key, group);
    if (seen_check(key, "", m->text, now, BLN_WINDOW_MS) != SEEN_NEW)
        return true;
    if (mbx_append(MBX_DIR_BC_IN, from, group, m->text) != MBX_NONE)
        msg_port_notify(kind);
    return true;
}

static void handle_frame(const uint8_t *buf, uint16_t len, uint32_t now)
{
    ax25_frame_t f;
    if (!ax25_parse_ui(buf, len, &f))
        return;
    gHeard++;

    ax25_addr_t me;
    if (!gCfg.enabled || !MSG_MyAddr(&me))
        return;

    aprs_msg_t m;
    aprs_msg_parse(&f, &m);
    if (m.kind == APRS_MSG_TEXT && handle_bulletin(&m, &me, now))
        return;
    if (m.kind == APRS_MSG_NONE || !aprs_msg_is_for(&m, &me))
        return;
    if (ax25_addr_equal(&m.from, &me))
        return;                        /* our own frame, repeated */

    char from[AX25_CALL_STR];
    ax25_addr_format(&m.from, from);

    switch (m.kind) {
    case APRS_MSG_TEXT: {
        if (m.reply_ack[0])
            outcome(from, m.reply_ack, MBX_F_DELIVERED);
        const seen_t_kind seen = seen_check(from, m.id, m.text, now, DEDUPE_WINDOW_MS);
        if (m.id[0] && seen != SEEN_COPY)
            queue_ack(from, m.id);
        if (m.text[0] && seen == SEEN_NEW) {
            if (mbx_append(MBX_DIR_IN, from, m.id, m.text) != MBX_NONE)
                msg_port_notify(MSG_NOTIFY_PRIVATE);
        }
        break;
    }
    case APRS_MSG_ACK:
        outcome(from, m.id, MBX_F_DELIVERED);
        break;
    case APRS_MSG_REJ:
        outcome(from, m.id, MBX_F_FAILED);
        break;
    default:
        break;
    }
}

/* ---------------------------------------------------------------- send -- */

msg_send_result_t MSG_Send(const char *to, const char *text)
{
    ax25_addr_t me, dst;
    if (!gCfg.enabled)
        return MSG_SEND_DISABLED;
    if (!MSG_MyAddr(&me))
        return MSG_SEND_NO_CALL;
    if (!ax25_addr_parse(to, &dst) || dst.repeated)
        return MSG_SEND_BAD_CALL;
    if (!text[0])
        return MSG_SEND_EMPTY;

    pending_t *p = NULL;
    for (uint8_t i = 0; i < MSG_OUTBOX_MAX && !p; i++)
        if (!gOut[i].active)
            p = &gOut[i];
    if (!p)
        return MSG_SEND_OUTBOX_FULL;

    char peer[AX25_CALL_STR], id[APRS_ID_MAX + 1], clean[APRS_TEXT_MAX + 1];
    ax25_addr_format(&dst, peer);

    uint8_t n = 0;
    for (; text[n] && n < APRS_TEXT_MAX; n++) {
        char c = text[n];
        clean[n] = (c == '|' || c == '~' || c == '{' || c < 0x20 || c > 0x7E) ? '-' : c;
    }
    clean[n] = '\0';

    uint16_t num = gCfg.next_id;
    uint8_t k = 0;
    char rev[4];
    do {
        rev[k++] = (char)('0' + num % 10u);
        num /= 10u;
    } while (num);
    for (uint8_t i = 0; i < k; i++)
        id[i] = rev[k - 1u - i];
    id[k] = '\0';
    gCfg.next_id = (uint16_t)(gCfg.next_id >= 999u ? 1u : gCfg.next_id + 1u);
    cfg_save();

    const int16_t slot = mbx_append(MBX_DIR_OUT, peer, id, clean);
    if (slot == MBX_NONE)
        return MSG_SEND_FLASH;

    p->active = true;
    p->tries  = 0;
    p->slot   = slot;
    p->due    = msg_port_ms();
    strcpy(p->peer, peer);
    strcpy(p->id, id);
    return MSG_SEND_OK;
}

msg_send_result_t MSG_Resend(int16_t slot)
{
    mbx_rec_t r;
    if (!gCfg.enabled)
        return MSG_SEND_DISABLED;
    if (!mbx_read(slot, &r) || r.dir != MBX_DIR_OUT || !r.id[0])
        return MSG_SEND_FLASH;
    pending_t *p = NULL;
    for (uint8_t i = 0; i < MSG_OUTBOX_MAX; i++) {
        if (gOut[i].active && gOut[i].slot == slot) {
            gOut[i].due = msg_port_ms();   /* already waiting: send now */
            return MSG_SEND_OK;
        }
        if (!gOut[i].active && !p)
            p = &gOut[i];
    }
    if (!p)
        return MSG_SEND_OUTBOX_FULL;
    p->active = true;
    p->tries  = 0;
    p->slot   = slot;
    p->due    = msg_port_ms();
    strcpy(p->peer, r.peer);
    strcpy(p->id, r.id);
    return MSG_SEND_OK;
}

void MSG_ChannelName(char *out)
{
    out[0] = '#';
    strcpy(out + 1, gCfg.group[0] ? gCfg.group : "ALL");
}

msg_send_result_t MSG_SendBroadcast(const char *group, const char *text)
{
    ax25_addr_t me;
    if (!gCfg.enabled)
        return MSG_SEND_DISABLED;
    if (!MSG_MyAddr(&me))
        return MSG_SEND_NO_CALL;
    if (!text[0])
        return MSG_SEND_EMPTY;
    if (!group_valid(group))
        return MSG_SEND_BAD_CALL;
    bcast_t *b = !gBc[0].active ? &gBc[0] : !gBc[1].active ? &gBc[1] : NULL;
    if (!b)
        return MSG_SEND_OUTBOX_FULL;

    char mine[AX25_CALL_STR], clean[APRS_TEXT_MAX + 1];
    ax25_addr_format(&me, mine);
    uint8_t n = 0;
    for (; text[n] && n < APRS_TEXT_MAX; n++) {
        const char c = text[n];
        clean[n] = (c == '|' || c == '~' || c == '{' || c < 0x20 || c > 0x7E) ? '-' : c;
    }
    clean[n] = '\0';

    b->to[0] = 'B'; b->to[1] = 'L'; b->to[2] = 'N';
    b->to[3] = (char)('0' + gCfg.next_bln);
    strcpy(b->to + 4, group);
    gCfg.next_bln = (uint8_t)((gCfg.next_bln + 1u) % 10u);
    cfg_save();

    const int16_t slot = mbx_append(MBX_DIR_BC_OUT, mine, group, clean);
    if (slot == MBX_NONE)
        return MSG_SEND_FLASH;
    b->active = true;
    b->sends  = 0;
    b->slot   = slot;
    b->due    = msg_port_ms();
    return MSG_SEND_OK;
}

uint8_t MSG_OutboxCount(void)
{
    uint8_t n = 0;
    for (uint8_t i = 0; i < MSG_OUTBOX_MAX; i++)
        n += gOut[i].active;
    return n;
}

bool MSG_SlotPending(int16_t slot, uint8_t *tries)
{
    for (uint8_t i = 0; i < 2u; i++)
        if (gBc[i].active && gBc[i].slot == slot) {
            if (tries)
                *tries = gBc[i].sends;
            return true;
        }
    for (uint8_t i = 0; i < MSG_OUTBOX_MAX; i++)
        if (gOut[i].active && gOut[i].slot == slot) {
            if (tries)
                *tries = gOut[i].tries;
            return true;
        }
    return false;
}

msg_tx_result_t MSG_LastTxResult(void) { return gLastResult; }
uint16_t        MSG_FramesHeard(void)  { return gHeard; }
uint16_t        MSG_FramesSent(void)   { return gSent; }

/* ------------------------------------------------------------ transmit -- */

typedef enum { TX_NONE = 0, TX_ACK, TX_MSG, TX_BCAST, TX_BEACON } tx_kind_t;

static bcast_t *due_bcast(uint32_t now)
{
    for (uint8_t i = 0; i < 2u; i++)
        if (gBc[i].active && time_reached(now, gBc[i].due))
            return &gBc[i];
    return NULL;
}

static pending_t *due_message(uint32_t now)
{
    pending_t *best = NULL;
    for (uint8_t i = 0; i < MSG_OUTBOX_MAX; i++) {
        pending_t *p = &gOut[i];
        if (p->active && p->tries < MSG_RETRIES && time_reached(now, p->due)
            && (!best || (int32_t)(p->due - best->due) < 0))
            best = p;
    }
    return best;
}

static bool beacon_due(uint32_t now)
{
    return BEACON_MIN[gCfg.beacon] != 0 && time_reached(now, gBeaconDue);
}

bool MSG_TxPending(void)
{
    const uint32_t now = msg_port_ms();
    return gCfg.enabled && (gAckCount || due_message(now) || due_bcast(now) || beacon_due(now));
}

static uint16_t build_frame(const char *info, uint16_t info_len)
{
    ax25_addr_t me, dst, path[2];
    uint8_t npath = 0;
    if (!MSG_MyAddr(&me))
        return 0;
    ax25_addr_parse(MSG_TOCALL, &dst);
    if (gCfg.path >= MSG_PATH_WIDE1)
        ax25_addr_parse("WIDE1-1", &path[npath++]);
    if (gCfg.path >= MSG_PATH_WIDE1_WIDE2)
        ax25_addr_parse("WIDE2-1", &path[npath++]);
    return ax25_build_ui(gFrame, sizeof gFrame, &dst, &me, path, npath, info, info_len);
}

static void transmit_one(tx_kind_t kind, pending_t *p, bcast_t *b, uint32_t now)
{
    uint16_t info = 0;
    if (kind == TX_ACK) {
        info = aprs_msg_format_ack(gInfo, sizeof gInfo, gAck[0].peer, gAck[0].id, false);
    } else if (kind == TX_MSG) {
        mbx_rec_t r;
        if (mbx_read(p->slot, &r))
            info = aprs_msg_format_text(gInfo, sizeof gInfo, r.peer, r.text, r.id);
        if (!info) {                   /* the record is gone: give up on it */
            p->active = false;
            return;
        }
    } else if (kind == TX_BCAST) {
        mbx_rec_t r;
        if (mbx_read(b->slot, &r))
            info = aprs_msg_format_text(gInfo, sizeof gInfo, b->to, r.text, NULL);
        if (!info) {
            b->active = false;
            return;
        }
    } else {
        info = aprs_msg_format_status(gInfo, sizeof gInfo, gCfg.status);
    }
    const uint16_t len = info ? build_frame(gInfo, info) : 0;

    msg_tx_result_t res = len ? msg_port_transmit(gFrame, len) : MSG_TX_NO_VFO;
    if (res == MSG_TX_BUSY)
        return;                        /* try again once the channel is clear */
    gLastResult = res;

    if (res == MSG_TX_OK) {
        gSent++;
        gLastTx = msg_port_ms();
        gEverTx = true;
        gTokens = gTokens >= 1000u ? gTokens - 1000u : 0u;
    }

    switch (kind) {
    case TX_ACK:
        memmove(&gAck[0], &gAck[1], sizeof(ack_t) * (ACK_QUEUE - 1u));
        gAckCount--;
        break;
    case TX_MSG:
        if (res == MSG_TX_OK) {
            p->due = now + 1000u * RETRY_S[p->tries];
            p->tries++;
        } else {
            p->due = now + NO_VFO_RETRY_MS;   /* no try lost without a VFO */
        }
        break;
    case TX_BCAST:
        if (res == MSG_TX_OK) {
            if (++b->sends >= MSG_BCAST_SENDS)
                b->active = false;
            else
                b->due = now + BCAST_REPEAT_MS;
        } else {
            b->due = now + NO_VFO_RETRY_MS;
        }
        break;
    case TX_BEACON:
        gBeaconDue = now + (res == MSG_TX_OK ? 60000u * BEACON_MIN[gCfg.beacon]
                                             : NO_VFO_RETRY_MS);
        break;
    default:
        break;
    }
}

void MSG_Task(void)
{
    const uint32_t now = msg_port_ms();

    uint16_t len;
    while (msg_port_take_frame(gFrame, &len))
        handle_frame(gFrame, len, now);

    if (!gCfg.enabled) {
        gWasEnabled = false;
        gHoldoffAt = 0;
        return;
    }
    if (!gWasEnabled) {                /* switched on (or boot) */
        gWasEnabled = true;
        gBeaconDue = now + FIRST_BEACON_MS;
    }

    /* token bucket against runaway transmissions */
    while (now - gTokenAt >= TOKEN_REFILL_MS) {
        gTokenAt += TOKEN_REFILL_MS;
        if (gTokens < TOKENS_MAX * 1000u)
            gTokens += 1000u;
    }

    /* messages whose last ack window closed */
    for (uint8_t i = 0; i < MSG_OUTBOX_MAX; i++) {
        pending_t *p = &gOut[i];
        if (p->active && p->tries >= MSG_RETRIES && time_reached(now, p->due)) {
            mbx_set_flags(p->slot, MBX_F_FAILED);
            p->active = false;
        }
    }

    tx_kind_t kind = TX_NONE;
    pending_t *p = NULL;
    bcast_t *b = NULL;
    if (gAckCount)
        kind = TX_ACK;
    else if ((p = due_message(now)) != NULL)
        kind = TX_MSG;
    else if ((b = due_bcast(now)) != NULL)
        kind = TX_BCAST;
    else if (beacon_due(now))
        kind = TX_BEACON;
    if (kind == TX_NONE) {
        gHoldoffAt = 0;
        return;
    }

    if (!msg_port_channel_clear()) {
        gHoldoffAt = 0;                /* wait for a clear channel, then a new delay */
        return;
    }
    if (gHoldoffAt == 0) {
        gHoldoffAt = now + HOLDOFF_MIN_MS + msg_port_random() % HOLDOFF_SPAN_MS;
        if (gHoldoffAt == 0)
            gHoldoffAt = 1;
        return;
    }
    if (!time_reached(now, gHoldoffAt))
        return;
    if (gTokens < 1000u || (gEverTx && now - gLastTx < TX_GAP_MS))
        return;
    gHoldoffAt = 0;
    transmit_one(kind, p, b, now);
}
