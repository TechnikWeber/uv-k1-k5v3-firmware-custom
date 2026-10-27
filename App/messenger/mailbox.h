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

/* The mailbox: incoming and outgoing messages in the external SPI flash, kept
 * across power-off.
 *
 * A circular log of 128-byte records over MBX_LOG_SECTORS sectors. A record is
 * written into erased flash, its commit byte last, so a power loss leaves
 * either a whole record or a slot that is skipped. State changes (read,
 * delivered, failed, deleted) only clear bits of the flags byte: NOR flash does
 * that without an erase. When the log wraps, the oldest sector is erased.
 *
 * Pure C over msg_flash.h; the host tests run it on a simulated NOR flash. */

#ifndef MESSENGER_MAILBOX_H
#define MESSENGER_MAILBOX_H

#include <stdbool.h>
#include <stdint.h>

#include "aprs_ax25.h"
#include "aprs_msg.h"

/* External flash region (see App/driver/mb_flash.h for the map): the top of
 * the free "expansion reserve", below the Sonic 4K reserve at 0x1DE000. */
#define MSG_FLASH_BASE      0x001CE000u
#define MSG_FLASH_SECTOR    0x1000u
#define MSG_FLASH_SECTORS   16u               /* 64 KiB                        */
#define MSG_CONFIG_ADDR     MSG_FLASH_BASE    /* sector 0: settings            */
#define MBX_LOG_BASE        (MSG_FLASH_BASE + MSG_FLASH_SECTOR)
#define MBX_LOG_SECTORS     (MSG_FLASH_SECTORS - 1u)
#define MBX_REC_SIZE        128u
#define MBX_PER_SECTOR      (MSG_FLASH_SECTOR / MBX_REC_SIZE)
#define MBX_SLOTS           (MBX_LOG_SECTORS * MBX_PER_SECTOR)   /* 480 */

#define MBX_DIR_IN          0u
#define MBX_DIR_OUT         1u

/* flags: a bit cleared means the state is set (erased flash = 0xFF) */
#define MBX_F_READ          0x01u   /* incoming: opened by the user           */
#define MBX_F_DELIVERED     0x02u   /* outgoing: acknowledged                 */
#define MBX_F_FAILED        0x04u   /* outgoing: no ack after all retries, or rejected */
#define MBX_F_DELETED       0x08u

#define MBX_NONE            (-1)

typedef struct {
    uint8_t  commit;                /* MBX_COMMIT once complete               */
    uint8_t  flags;                 /* MBX_F_*, active low                    */
    uint8_t  dir;                   /* MBX_DIR_*                              */
    uint8_t  text_len;
    uint32_t seq;                   /* grows by one per record                */
    char     peer[AX25_CALL_STR];   /* sender (in) or recipient (out)         */
    char     id[APRS_ID_MAX + 1];   /* message number                         */
    uint8_t  crc;                   /* CRC-8 over dir..text                   */
    uint8_t  reserved[3];
    char     text[APRS_TEXT_BUF];
} mbx_rec_t;

_Static_assert(sizeof(mbx_rec_t) == MBX_REC_SIZE, "one record per 128 bytes");

/* Scans the log: next free slot, newest record, unread count. */
void     mbx_init(void);

/* Appends a record (commit, flags and seq are set here). Returns its slot,
 * MBX_NONE if the flash refused it. */
int16_t  mbx_append(uint8_t dir, const char *peer, const char *id, const char *text);

/* Reads a committed record; false if the slot holds none or its CRC fails. */
bool     mbx_read(int16_t slot, mbx_rec_t *rec);

/* Sets state flags (clears their bits). Keeps the unread count in step. */
void     mbx_set_flags(int16_t slot, uint8_t flags);

/* Navigation over records that are not deleted, newest first.
 * mbx_older / mbx_newer return MBX_NONE past either end. */
int16_t  mbx_newest(void);
int16_t  mbx_older(int16_t slot);
int16_t  mbx_newer(int16_t slot);

/* The newest outgoing record to peer with this id that is not delivered yet,
 * among the last MBX_PER_SECTOR * 2 records; MBX_NONE if none. */
int16_t  mbx_find_outgoing(const char *peer, const char *id);

uint16_t mbx_unread(void);
uint16_t mbx_count(void);           /* records not deleted                    */

/* Erases the whole log (settings stay). */
void     mbx_clear(void);

#endif
