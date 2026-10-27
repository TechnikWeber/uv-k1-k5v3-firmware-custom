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
#include "msg_flash.h"

#define MBX_COMMIT   0xA5u
#define HDR_SIZE     8u                /* commit, flags, dir, text_len, seq */
#define CRC_FROM     offsetof(mbx_rec_t, dir)
#define CRC_TO       offsetof(mbx_rec_t, crc)
#define FIND_DEPTH   (MBX_PER_SECTOR * 2u)

_Static_assert(offsetof(mbx_rec_t, seq) == 4u, "header layout");
_Static_assert(offsetof(mbx_rec_t, text) == 28u, "record layout");
_Static_assert(MSG_FLASH_BASE + MSG_FLASH_SECTORS * MSG_FLASH_SECTOR <= 0x001DE000u,
               "the mailbox must stay below the Sonic 4K reserve");

typedef struct {
    uint8_t  commit, flags, dir, text_len;
    uint32_t seq;
} hdr_t;

static int16_t  gNext;                 /* slot for the next record           */
static int16_t  gNewest;
static uint32_t gSeqNext;
static uint16_t gUnread;
static uint16_t gCount;

static uint32_t slot_addr(int16_t slot)
{
    return MBX_LOG_BASE + (uint32_t)slot * MBX_REC_SIZE;
}

static int16_t slot_next(int16_t s) { return (int16_t)((s + 1) % (int16_t)MBX_SLOTS); }
static int16_t slot_prev(int16_t s) { return (int16_t)((s + (int16_t)MBX_SLOTS - 1) % (int16_t)MBX_SLOTS); }

static bool read_hdr(int16_t slot, hdr_t *h)
{
    msg_flash_read(slot_addr(slot), h, HDR_SIZE);
    return h->commit == MBX_COMMIT && h->dir <= MBX_DIR_MAX && h->text_len <= APRS_TEXT_BUF;
}

static bool is_unread(const hdr_t *h)
{
    return (h->dir == MBX_DIR_IN || h->dir == MBX_DIR_BC_IN)
        && (h->flags & MBX_F_READ) && (h->flags & MBX_F_DELETED);
}

static uint8_t crc8(const mbx_rec_t *r)
{
    const uint8_t *p = (const uint8_t *)r;
    uint8_t c = 0;
    for (size_t i = CRC_FROM; i < CRC_TO + 0u; i++) {
        c ^= p[i];
        for (uint8_t k = 0; k < 8u; k++)
            c = (c & 0x80u) ? (uint8_t)((c << 1) ^ 0x07u) : (uint8_t)(c << 1);
    }
    for (uint8_t i = 0; i < r->text_len; i++) {
        c ^= (uint8_t)r->text[i];
        for (uint8_t k = 0; k < 8u; k++)
            c = (c & 0x80u) ? (uint8_t)((c << 1) ^ 0x07u) : (uint8_t)(c << 1);
    }
    return c;
}

void mbx_init(void)
{
    gNewest  = MBX_NONE;
    gUnread  = 0;
    gCount   = 0;
    uint32_t maxSeq = 0;

    for (int16_t s = 0; s < (int16_t)MBX_SLOTS; s++) {
        hdr_t h;
        if (!read_hdr(s, &h))
            continue;
        if (h.flags & MBX_F_DELETED)
            gCount++;
        if (is_unread(&h))
            gUnread++;
        if (gNewest == MBX_NONE || h.seq > maxSeq) {
            maxSeq  = h.seq;
            gNewest = s;
        }
    }
    gNext    = (gNewest == MBX_NONE) ? 0 : slot_next(gNewest);
    gSeqNext = maxSeq + 1u;
}

static bool blank(uint32_t addr, uint32_t len)
{
    uint8_t buf[32];
    while (len) {
        const uint32_t n = len < sizeof buf ? len : sizeof buf;
        msg_flash_read(addr, buf, n);
        for (uint32_t i = 0; i < n; i++)
            if (buf[i] != 0xFFu)
                return false;
        addr += n;
        len -= n;
    }
    return true;
}

/* The sector starting at slot is about to be reused: forget its records. */
static void recycle_sector(int16_t slot)
{
    const uint32_t addr = slot_addr(slot);
    if (blank(addr, MSG_FLASH_SECTOR))
        return;
    for (int16_t s = slot; s < slot + (int16_t)MBX_PER_SECTOR; s++) {
        hdr_t h;
        if (!read_hdr(s, &h))
            continue;
        if (h.flags & MBX_F_DELETED)
            gCount--;
        if (is_unread(&h))
            gUnread--;
        if (s == gNewest)
            gNewest = MBX_NONE;        /* only when every slot was full */
    }
    msg_flash_erase(addr);
}

int16_t mbx_append(uint8_t dir, const char *peer, const char *id, const char *text, uint8_t flags)
{
    mbx_rec_t r;
    memset(&r, 0xFF, sizeof r);
    r.flags = (uint8_t)~flags;
    r.dir = dir;
    const size_t tl = strlen(text);
    r.text_len = (uint8_t)(tl > APRS_TEXT_BUF ? APRS_TEXT_BUF : tl);
    r.seq = gSeqNext;
    memset(r.peer, 0, sizeof r.peer);
    strncpy(r.peer, peer, sizeof r.peer - 1u);
    memset(r.id, 0, sizeof r.id);
    if (id)
        strncpy(r.id, id, sizeof r.id - 1u);
    memcpy(r.text, text, r.text_len);
    r.crc = crc8(&r);

    for (uint16_t tries = 0; tries < MBX_SLOTS; tries++) {
        const int16_t slot = gNext;
        gNext = slot_next(slot);
        if (slot % (int16_t)MBX_PER_SECTOR == 0)
            recycle_sector(slot);
        const uint32_t addr = slot_addr(slot);
        if (!blank(addr, MBX_REC_SIZE))
            continue;                  /* a record cut by a power loss */

        msg_flash_program(addr + 1u, (const uint8_t *)&r + 1u, MBX_REC_SIZE - 1u);
        const uint8_t commit = MBX_COMMIT;
        msg_flash_program(addr, &commit, 1);

        hdr_t h;
        if (!read_hdr(slot, &h) || h.seq != r.seq)
            continue;                  /* did not stick: next slot */

        gSeqNext++;
        gNewest = slot;
        gCount++;
        if (dir == MBX_DIR_IN || dir == MBX_DIR_BC_IN)
            gUnread++;
        return slot;
    }
    return MBX_NONE;
}

bool mbx_read(int16_t slot, mbx_rec_t *rec)
{
    if (slot < 0 || slot >= (int16_t)MBX_SLOTS)
        return false;
    msg_flash_read(slot_addr(slot), rec, MBX_REC_SIZE);
    if (rec->commit != MBX_COMMIT || rec->dir > MBX_DIR_MAX || rec->text_len > APRS_TEXT_BUF
        || rec->crc != crc8(rec))
        return false;
    rec->peer[sizeof rec->peer - 1u] = '\0';
    rec->id[sizeof rec->id - 1u] = '\0';
    if (rec->text_len < APRS_TEXT_BUF)
        rec->text[rec->text_len] = '\0';
    else
        rec->text[APRS_TEXT_BUF - 1u] = '\0';
    return true;
}

void mbx_set_flags(int16_t slot, uint8_t flags)
{
    hdr_t h;
    if (slot < 0 || slot >= (int16_t)MBX_SLOTS || !read_hdr(slot, &h))
        return;
    const uint8_t now = (uint8_t)(h.flags & ~flags);
    if (now == h.flags)
        return;
    const bool wasUnread = is_unread(&h);
    const bool wasLive   = h.flags & MBX_F_DELETED;
    msg_flash_program(slot_addr(slot) + 1u, &now, 1);
    h.flags = now;
    if (wasUnread && !is_unread(&h))
        gUnread--;
    if (wasLive && !(now & MBX_F_DELETED))
        gCount--;
}

int16_t mbx_newest(void)
{
    if (gNewest == MBX_NONE)
        return MBX_NONE;
    hdr_t h;
    if (read_hdr(gNewest, &h) && (h.flags & MBX_F_DELETED))
        return gNewest;
    return mbx_older(gNewest);
}

/* Walks the log from slot towards older (dir -1) or newer (+1) records.
 * Sequence numbers must keep falling (rising): past the oldest (newest)
 * record the walk meets a newer (older) one and stops. Blank and cut slots
 * are stepped over. */
static int16_t walk(int16_t slot, int8_t dir)
{
    hdr_t h;
    if (slot < 0 || !read_hdr(slot, &h))
        return MBX_NONE;
    uint32_t seq = h.seq;
    int16_t s = slot;
    for (uint16_t i = 1; i < MBX_SLOTS; i++) {
        s = dir < 0 ? slot_prev(s) : slot_next(s);
        if (!read_hdr(s, &h))
            continue;
        if (dir < 0 ? h.seq >= seq : h.seq <= seq)
            return MBX_NONE;
        seq = h.seq;
        if (h.flags & MBX_F_DELETED)
            return s;
    }
    return MBX_NONE;
}

int16_t mbx_older(int16_t slot) { return walk(slot, -1); }
int16_t mbx_newer(int16_t slot) { return walk(slot, +1); }

int16_t mbx_find_outgoing(const char *peer, const char *id)
{
    int16_t s = mbx_newest();
    mbx_rec_t r;
    for (uint16_t i = 0; s != MBX_NONE && i < FIND_DEPTH; i++, s = mbx_older(s)) {
        if (!mbx_read(s, &r) || r.dir != MBX_DIR_OUT || !(r.flags & MBX_F_DELIVERED))
            continue;
        if (strcmp(r.id, id) == 0 && strcmp(r.peer, peer) == 0)
            return s;
    }
    return MBX_NONE;
}

uint16_t mbx_unread(void) { return gUnread; }
uint16_t mbx_count(void)  { return gCount; }

void mbx_clear(void)
{
    for (uint16_t k = 0; k < MBX_LOG_SECTORS; k++) {
        const uint32_t addr = MBX_LOG_BASE + (uint32_t)k * MSG_FLASH_SECTOR;
        if (!blank(addr, MSG_FLASH_SECTOR))
            msg_flash_erase(addr);
    }
    mbx_init();
}
