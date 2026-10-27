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

#include <string.h>

#include "aprs_ax25.h"

uint16_t ax25_crc_byte(uint16_t crc, uint8_t b)
{
    crc ^= b;
    for (uint8_t i = 0; i < 8u; i++)
        crc = (crc & 1u) ? (uint16_t)((crc >> 1) ^ 0x8408u) : (uint16_t)(crc >> 1);
    return crc;
}

uint16_t ax25_fcs(const uint8_t *data, uint16_t len)
{
    uint16_t crc = 0xFFFFu;
    while (len--)
        crc = ax25_crc_byte(crc, *data++);
    return (uint16_t)(crc ^ 0xFFFFu);
}

static bool is_call_char(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

bool ax25_addr_parse(const char *text, ax25_addr_t *out)
{
    uint8_t n = 0;
    memset(out, 0, sizeof(*out));

    while (*text && *text != '-' && *text != '*') {
        char c = *text++;
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 32);
        if (!is_call_char(c) || n >= AX25_CALL_LEN)
            return false;
        out->call[n++] = c;
    }
    if (n == 0)
        return false;

    if (*text == '-') {
        text++;
        uint8_t ssid = 0, digits = 0;
        while (*text >= '0' && *text <= '9') {
            ssid = (uint8_t)(ssid * 10u + (uint8_t)(*text++ - '0'));
            if (++digits > 2u || ssid > 15u)
                return false;
        }
        if (digits == 0)
            return false;
        out->ssid = ssid;
    }
    if (*text == '*') {
        out->repeated = true;
        text++;
    }
    return *text == '\0';
}

void ax25_addr_format(const ax25_addr_t *a, char *out)
{
    uint8_t n = 0;
    for (uint8_t i = 0; i < AX25_CALL_LEN && a->call[i]; i++)
        out[n++] = a->call[i];
    if (a->ssid) {
        out[n++] = '-';
        if (a->ssid >= 10u) {
            out[n++] = '1';
            out[n++] = (char)('0' + a->ssid - 10u);
        } else {
            out[n++] = (char)('0' + a->ssid);
        }
    }
    out[n] = '\0';
}

bool ax25_addr_equal(const ax25_addr_t *a, const ax25_addr_t *b)
{
    return a->ssid == b->ssid && strncmp(a->call, b->call, AX25_CALL_LEN) == 0;
}

/* One 7-byte address field. command: the C bit of the destination (AX.25 v2
 * command frame); src and digis keep it clear. */
static void put_addr(uint8_t *o, const ax25_addr_t *a, bool command, bool last)
{
    uint8_t i = 0;
    for (; i < AX25_CALL_LEN && a->call[i]; i++)
        o[i] = (uint8_t)(a->call[i] << 1);
    for (; i < AX25_CALL_LEN; i++)
        o[i] = (uint8_t)(' ' << 1);
    uint8_t sb = (uint8_t)(0x60u | ((a->ssid & 15u) << 1));
    if (command || a->repeated)
        sb |= 0x80u;
    if (last)
        sb |= 0x01u;
    o[6] = sb;
}

uint16_t ax25_build_ui(uint8_t *out, uint16_t max,
                       const ax25_addr_t *dst, const ax25_addr_t *src,
                       const ax25_addr_t *digi, uint8_t ndigi,
                       const char *info, uint16_t info_len)
{
    if (ndigi > AX25_MAX_DIGIS)
        return 0;
    const uint32_t len = 7u * (2u + ndigi) + 2u + info_len + 2u;
    if (len > max)
        return 0;

    uint8_t *o = out;
    put_addr(o, dst, true, false);                 o += 7;
    put_addr(o, src, false, ndigi == 0);           o += 7;
    for (uint8_t i = 0; i < ndigi; i++) {
        put_addr(o, &digi[i], false, i + 1u == ndigi);
        o += 7;
    }
    *o++ = 0x03u;
    *o++ = 0xF0u;
    memcpy(o, info, info_len);
    o += info_len;

    const uint16_t fcs = ax25_fcs(out, (uint16_t)(o - out));
    *o++ = (uint8_t)(fcs & 0xFFu);
    *o++ = (uint8_t)(fcs >> 8);
    return (uint16_t)(o - out);
}

static bool get_addr(const uint8_t *a, ax25_addr_t *out)
{
    uint8_t n = 0;
    memset(out, 0, sizeof(*out));
    for (uint8_t i = 0; i < AX25_CALL_LEN; i++) {
        if (a[i] & 1u)
            return false;                 /* address extension bit inside a call */
        const char c = (char)(a[i] >> 1);
        if (c == ' ')
            continue;
        if (!is_call_char(c))
            return false;
        out->call[n++] = c;
    }
    out->ssid     = (uint8_t)((a[6] >> 1) & 15u);
    out->repeated = (a[6] & 0x80u) != 0;
    return n > 0;
}

bool ax25_parse_ui(const uint8_t *frame, uint16_t len, ax25_frame_t *out)
{
    if (len < AX25_FRAME_MIN)
        return false;
    const uint16_t body = (uint16_t)(len - 2u);    /* without the FCS */

    uint16_t pos = 0;
    uint8_t naddr = 0;
    for (;;) {
        if (pos + 7u > body)
            return false;
        ax25_addr_t a;
        if (!get_addr(frame + pos, &a))
            return false;
        if (naddr == 0)
            out->dst = a;
        else if (naddr == 1)
            out->src = a;
        else if (naddr - 2u < AX25_MAX_DIGIS)
            out->digi[naddr - 2u] = a;
        else
            return false;
        naddr++;
        const bool last = frame[pos + 6u] & 1u;
        pos += 7;
        if (last)
            break;
    }
    if (naddr < 2u || pos + 2u > body)
        return false;
    if (frame[pos] != 0x03u || frame[pos + 1u] != 0xF0u)
        return false;

    out->dst.repeated = false;
    out->src.repeated = false;
    out->ndigi    = (uint8_t)(naddr - 2u);
    out->info     = frame + pos + 2u;
    out->info_len = (uint16_t)(body - pos - 2u);
    return true;
}
