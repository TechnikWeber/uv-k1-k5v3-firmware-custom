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

#include "aprs_afsk.h"

#define FLAG 0x7Eu

void afsk_bits_init(afsk_bits_t *g, const uint8_t *frame, uint16_t len,
                    uint16_t preamble_flags, uint16_t tail_flags)
{
    g->frame      = frame;
    g->len        = len;
    g->flags_left = preamble_flags ? preamble_flags : 1u;
    g->tail       = tail_flags ? tail_flags : 1u;
    g->pos        = 0;
    g->bit        = 0;
    g->ones       = 0;
    g->stage      = 0;
}

int8_t afsk_bits_next(afsk_bits_t *g)
{
    for (;;) {
        switch (g->stage) {
        case 0:
        case 2: {                              /* flags, never stuffed */
            const int8_t b = (int8_t)((FLAG >> g->bit) & 1u);
            if (++g->bit == 8u) {
                g->bit = 0;
                if (--g->flags_left == 0) {
                    g->stage++;
                    if (g->stage == 1u && g->len == 0)
                        g->stage = 2;
                    if (g->stage == 2u)
                        g->flags_left = g->tail;
                }
            }
            return b;
        }
        case 1: {
            if (g->ones == 5u) {               /* stuffed zero after five 1s */
                g->ones = 0;
                return 0;
            }
            const int8_t b = (int8_t)((g->frame[g->pos] >> g->bit) & 1u);
            g->ones = b ? (uint8_t)(g->ones + 1u) : 0u;
            if (++g->bit == 8u) {
                g->bit = 0;
                if (++g->pos == g->len) {
                    /* a stuffed zero may still be due before the tail */
                    if (g->ones == 5u) {
                        g->stage = 4;
                        return b;
                    }
                    g->stage = 2;
                    g->flags_left = g->tail;
                }
            }
            return b;
        }
        case 4:                                /* the last stuffed zero */
            g->ones = 0;
            g->stage = 2;
            g->flags_left = g->tail;
            return 0;
        default:
            return -1;
        }
    }
}
