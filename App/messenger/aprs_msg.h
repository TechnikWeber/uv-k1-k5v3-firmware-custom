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

/* APRS messages (APRS 1.0.1 chapter 14, plus reply-acks): parsing the info
 * field of a received frame and formatting the info field of a message, an ack
 * or a status beacon. Pure C, built by the host tests. */

#ifndef MESSENGER_APRS_MSG_H
#define MESSENGER_APRS_MSG_H

#include <stdbool.h>
#include <stdint.h>

#include "aprs_ax25.h"

#define APRS_ADDRESSEE_LEN  9u     /* fixed width, space padded              */
#define APRS_TEXT_MAX       67u    /* message text the spec allows           */
#define APRS_TEXT_BUF       100u   /* received text after umlaut expansion   */
#define APRS_ID_MAX         5u     /* message number                         */

typedef enum {
    APRS_MSG_NONE = 0,             /* not a message                          */
    APRS_MSG_TEXT,                 /* a message, with or without an id      */
    APRS_MSG_ACK,
    APRS_MSG_REJ,
} aprs_msg_kind_t;

typedef struct {
    aprs_msg_kind_t kind;
    ax25_addr_t     from;          /* originator; for third-party traffic the
                                      station inside the "}" header          */
    bool            third_party;
    char            to[APRS_ADDRESSEE_LEN + 1];   /* addressee, trimmed      */
    char            text[APRS_TEXT_BUF + 1];      /* TEXT only, printable ASCII */
    char            id[APRS_ID_MAX + 1];          /* TEXT: its number ("" when
                                                      none); ACK/REJ: the number
                                                      acknowledged            */
    char            reply_ack[APRS_ID_MAX + 1];   /* "{MM}AA": AA, an ack for one
                                                      of our messages riding on
                                                      this one ("" when none) */
} aprs_msg_t;

/* Fills m from a parsed UI frame; m->kind is APRS_MSG_NONE when the frame is
 * not a message (positions, status, telemetry...). */
void aprs_msg_parse(const ax25_frame_t *f, aprs_msg_t *m);

/* Is the addressee this station? Case-insensitive, SSID must match. */
bool aprs_msg_is_for(const aprs_msg_t *m, const ax25_addr_t *me);

/* ":DL1ABC   :text{id". id may be NULL or "" (no ack wanted). Characters the
 * spec forbids in the text ('|', '~', '{') become '-'. Returns the length
 * written (no NUL), 0 if it does not fit. */
uint16_t aprs_msg_format_text(char *out, uint16_t max, const char *to,
                              const char *text, const char *id);
/* ":DL1ABC   :ackID" or ":DL1ABC   :rejID". */
uint16_t aprs_msg_format_ack(char *out, uint16_t max, const char *to,
                             const char *id, bool reject);
/* ">text" status report. */
uint16_t aprs_msg_format_status(char *out, uint16_t max, const char *text);

#endif
