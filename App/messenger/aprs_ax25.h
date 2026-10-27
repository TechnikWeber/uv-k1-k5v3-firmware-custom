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

/* AX.25 UI frames as APRS uses them: addresses, FCS, building and parsing.
 * Pure C, no hardware: also built by the host tests (test/). */

#ifndef MESSENGER_APRS_AX25_H
#define MESSENGER_APRS_AX25_H

#include <stdbool.h>
#include <stdint.h>

#define AX25_CALL_LEN    6u      /* characters of a callsign, without SSID   */
#define AX25_CALL_STR    10u     /* "DL4PW-15" + NUL, rounded up             */
#define AX25_MAX_DIGIS   8u
#define AX25_FRAME_MAX   200u    /* largest frame kept (with FCS); an APRS
                                    message frame stays below ~170 bytes     */
#define AX25_FRAME_MIN   18u     /* 2 addresses + control + PID + FCS        */
#define AX25_FCS_GOOD    0xF0B8u /* CRC residue over a frame with its FCS    */

typedef struct {
    char    call[AX25_CALL_LEN + 1];  /* upper case, NUL-terminated, no spaces */
    uint8_t ssid;                     /* 0..15                                */
    bool    repeated;                 /* H bit (digipeaters only)             */
} ax25_addr_t;

typedef struct {
    ax25_addr_t    dst;
    ax25_addr_t    src;
    ax25_addr_t    digi[AX25_MAX_DIGIS];
    uint8_t        ndigi;
    const uint8_t *info;              /* points into the frame buffer         */
    uint16_t       info_len;
} ax25_frame_t;

/* CRC-16/X.25 step: crc = ax25_crc_byte(crc, b), start with 0xFFFF. */
uint16_t ax25_crc_byte(uint16_t crc, uint8_t b);
/* FCS of a frame body (init 0xFFFF, final xor 0xFFFF). */
uint16_t ax25_fcs(const uint8_t *data, uint16_t len);

/* "dl4pw-7" -> {"DL4PW", 7}. Accepts 1-6 letters/digits and an optional
 * -0..-15; a trailing '*' sets repeated. Returns false on anything else. */
bool ax25_addr_parse(const char *text, ax25_addr_t *out);
/* {"DL4PW", 7} -> "DL4PW-7" (no "-0"). out holds AX25_CALL_STR bytes. */
void ax25_addr_format(const ax25_addr_t *a, char *out);
/* Same station: call and SSID (repeated is ignored). */
bool ax25_addr_equal(const ax25_addr_t *a, const ax25_addr_t *b);

/* Builds dst, src, path, control 0x03, PID 0xF0, info and the FCS into out.
 * Returns the frame length, 0 if it does not fit in max bytes. */
uint16_t ax25_build_ui(uint8_t *out, uint16_t max,
                       const ax25_addr_t *dst, const ax25_addr_t *src,
                       const ax25_addr_t *digi, uint8_t ndigi,
                       const char *info, uint16_t info_len);

/* Parses a received frame (len includes the 2 FCS bytes, already checked).
 * Fails on anything that is not a UI frame with PID 0xF0. */
bool ax25_parse_ui(const uint8_t *frame, uint16_t len, ax25_frame_t *out);

#endif
