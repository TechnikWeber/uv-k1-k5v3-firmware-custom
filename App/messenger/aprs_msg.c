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

#include "aprs_msg.h"

static char upper(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
}

static bool is_alnum(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

/* German umlauts as two ASCII letters; the radio's fonts are ASCII only. */
static const char *umlaut(uint8_t latin1)
{
    switch (latin1) {
        case 0xC4: return "Ae";
        case 0xD6: return "Oe";
        case 0xDC: return "Ue";
        case 0xE4: return "ae";
        case 0xF6: return "oe";
        case 0xFC: return "ue";
        case 0xDF: return "ss";
        default:   return 0;
    }
}

/* Length of a valid UTF-8 sequence starting at s[0] (2 to 4), 0 if the bytes
 * are not one (then they are Latin-1). */
static uint8_t utf8_len(const uint8_t *s, uint16_t avail)
{
    uint8_t len;
    if ((s[0] & 0xE0u) == 0xC0u)      len = 2;
    else if ((s[0] & 0xF0u) == 0xE0u) len = 3;
    else if ((s[0] & 0xF8u) == 0xF0u) len = 4;
    else                              return 0;
    if (len > avail)
        return 0;
    for (uint8_t k = 1; k < len; k++)
        if ((s[k] & 0xC0u) != 0x80u)
            return 0;
    return len;
}

/* Copies the message text as printable ASCII. UTF-8 (APRSdroid) and Latin-1
 * umlauts become two letters, any other non-ASCII character '?'. */
static void copy_text(char *out, const uint8_t *s, uint16_t n)
{
    uint16_t o = 0;
    for (uint16_t i = 0; i < n && o < APRS_TEXT_BUF; i++) {
        uint8_t c = s[i];
        if (c >= 0x20u && c <= 0x7Eu) {
            out[o++] = (char)c;
            continue;
        }
        if (c < 0x20u || c == 0x7Fu)         /* CR/LF and other controls */
            continue;
        const char *rep = 0;
        const uint8_t ul = utf8_len(s + i, (uint16_t)(n - i));
        if (ul == 2u && c == 0xC3u)
            rep = umlaut((uint8_t)(0xC0u | (s[i + 1] & 0x3Fu)));
        else if (ul == 0)
            rep = umlaut(c);                 /* plain Latin-1 */
        if (ul)
            i = (uint16_t)(i + ul - 1u);
        if (rep) {
            for (; *rep && o < APRS_TEXT_BUF; rep++)
                out[o++] = *rep;
        } else {
            out[o++] = '?';
        }
    }
    while (o && out[o - 1u] == ' ')
        o--;
    out[o] = '\0';
}

/* Up to APRS_ID_MAX alphanumerics from s (n bytes) into out; stops at the
 * first other character. */
static uint16_t copy_id(char *out, const uint8_t *s, uint16_t n)
{
    uint16_t k = 0;
    while (k < n && k < APRS_ID_MAX && is_alnum((char)s[k])) {
        out[k] = (char)s[k];
        k++;
    }
    out[k] = '\0';
    return k;
}

/* "}DL1ABC-9>APRS,TCPIP,DB0XYZ*:payload": the originator and the payload. */
static bool unwrap_third_party(const uint8_t **info, uint16_t *len, ax25_addr_t *from)
{
    const uint8_t *s = *info;
    const uint16_t n = *len;
    uint16_t gt = 1;
    while (gt < n && s[gt] != '>')
        gt++;
    if (gt >= n || gt - 1u > AX25_CALL_STR - 1u)
        return false;

    char call[AX25_CALL_STR];
    memcpy(call, s + 1, gt - 1u);
    call[gt - 1u] = '\0';
    if (!ax25_addr_parse(call, from))
        return false;

    uint16_t colon = gt;
    while (colon < n && s[colon] != ':')
        colon++;
    if (colon >= n)
        return false;

    *info = s + colon + 1u;
    *len  = (uint16_t)(n - colon - 1u);
    return true;
}

void aprs_msg_parse(const ax25_frame_t *f, aprs_msg_t *m)
{
    memset(m, 0, sizeof(*m));
    m->from = f->src;

    const uint8_t *s = f->info;
    uint16_t n = f->info_len;

    if (n && s[0] == '}') {
        if (!unwrap_third_party(&s, &n, &m->from))
            return;
        m->third_party = true;
    }

    /* ":ADDRESSEE:" - 9 characters, then ':' */
    if (n < 2u + APRS_ADDRESSEE_LEN || s[0] != ':' || s[1u + APRS_ADDRESSEE_LEN] != ':')
        return;

    uint8_t k = 0;
    for (uint8_t i = 0; i < APRS_ADDRESSEE_LEN; i++)
        m->to[k++] = (char)s[1u + i];
    while (k && m->to[k - 1u] == ' ')
        k--;
    m->to[k] = '\0';
    if (k == 0)
        return;

    const uint8_t *t = s + 2u + APRS_ADDRESSEE_LEN;
    uint16_t tn = (uint16_t)(n - 2u - APRS_ADDRESSEE_LEN);
    while (tn && (t[tn - 1u] == '\r' || t[tn - 1u] == '\n' || t[tn - 1u] == ' '))
        tn--;

    /* ack / rej: "ackID" (an "ackMM}AA" reply-ack acknowledges MM) */
    if (tn > 3u && (t[0] == 'a' || t[0] == 'r')) {
        const bool ack = t[0] == 'a' && t[1] == 'c' && t[2] == 'k';
        const bool rej = t[0] == 'r' && t[1] == 'e' && t[2] == 'j';
        if ((ack || rej) && copy_id(m->id, t + 3, (uint16_t)(tn - 3u)) > 0) {
            const uint16_t rest = (uint16_t)(tn - 3u - strlen(m->id));
            if (rest == 0 || t[3u + strlen(m->id)] == '}') {
                m->kind = ack ? APRS_MSG_ACK : APRS_MSG_REJ;
                return;
            }
            m->id[0] = '\0';                 /* "acknowledge this" is plain text */
        }
    }

    /* text, then "{ID" or "{MM}AA" at the end */
    uint16_t brace = tn;
    for (uint16_t i = tn; i > 0; i--) {
        if (t[i - 1u] == '{') {
            brace = (uint16_t)(i - 1u);
            break;
        }
    }
    if (brace < tn) {
        const uint8_t *p = t + brace + 1u;
        const uint16_t pn = (uint16_t)(tn - brace - 1u);
        const uint16_t idn = copy_id(m->id, p, pn);
        if (idn > 0 && idn == pn) {
            /* plain "{ID" */
        } else if (idn > 0 && p[idn] == '}' &&
                   copy_id(m->reply_ack, p + idn + 1u, (uint16_t)(pn - idn - 1u))
                       == pn - idn - 1u) {
            /* "{MM}AA" or "{MM}" */
        } else {
            m->reply_ack[0] = '\0';
            m->id[0] = '\0';                 /* a '{' inside the text */
            brace = tn;
        }
    }
    copy_text(m->text, t, brace);
    m->kind = APRS_MSG_TEXT;
}

bool aprs_msg_is_for(const aprs_msg_t *m, const ax25_addr_t *me)
{
    char mine[AX25_CALL_STR];
    ax25_addr_format(me, mine);
    uint8_t i = 0;
    for (; mine[i] && m->to[i]; i++)
        if (upper(m->to[i]) != mine[i])
            return false;
    return mine[i] == '\0' && m->to[i] == '\0';
}

static uint16_t put_addressee(char *out, const char *to)
{
    uint16_t o = 0;
    out[o++] = ':';
    for (uint8_t i = 0; i < APRS_ADDRESSEE_LEN; i++)
        out[o++] = *to ? upper(*to++) : ' ';
    out[o++] = ':';
    return o;
}

uint16_t aprs_msg_format_text(char *out, uint16_t max, const char *to,
                              const char *text, const char *id)
{
    const uint16_t tl = (uint16_t)strlen(text);
    const uint16_t il = id ? (uint16_t)strlen(id) : 0u;
    if (tl > APRS_TEXT_MAX || il > APRS_ID_MAX ||
        11u + tl + (il ? il + 1u : 0u) > max)
        return 0;

    uint16_t o = put_addressee(out, to);
    for (uint16_t i = 0; i < tl; i++) {
        char c = text[i];
        if (c == '|' || c == '~' || c == '{' || (uint8_t)c < 0x20u || (uint8_t)c > 0x7Eu)
            c = '-';
        out[o++] = c;
    }
    if (il) {
        out[o++] = '{';
        memcpy(out + o, id, il);
        o = (uint16_t)(o + il);
    }
    return o;
}

uint16_t aprs_msg_format_ack(char *out, uint16_t max, const char *to,
                             const char *id, bool reject)
{
    const uint16_t il = (uint16_t)strlen(id);
    if (il == 0 || il > APRS_ID_MAX || 14u + il > max)
        return 0;
    uint16_t o = put_addressee(out, to);
    memcpy(out + o, reject ? "rej" : "ack", 3);
    o = (uint16_t)(o + 3u);
    memcpy(out + o, id, il);
    return (uint16_t)(o + il);
}

uint16_t aprs_msg_format_status(char *out, uint16_t max, const char *text)
{
    const uint16_t tl = (uint16_t)strlen(text);
    if (tl + 1u > max)
        return 0;
    out[0] = '>';
    memcpy(out + 1, text, tl);
    return (uint16_t)(tl + 1u);
}
