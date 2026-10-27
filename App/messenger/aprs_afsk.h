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

/* The bit stream of one HDLC frame on the air: preamble flags, the frame LSB
 * first with bit stuffing, tail flags. The transmitter applies NRZI (a 0
 * changes the tone). Pure C, built by the host tests. */

#ifndef MESSENGER_APRS_AFSK_H
#define MESSENGER_APRS_AFSK_H

#include <stdint.h>

#define AFSK_BAUD          1200u
#define AFSK_MARK_HZ       1200u
#define AFSK_SPACE_HZ      2200u
#define AFSK_PREAMBLE      40u     /* flags before the frame: ~267 ms (as APRS TX) */
#define AFSK_TAIL          3u

typedef struct {
    const uint8_t *frame;
    uint16_t len;
    uint16_t flags_left;           /* preamble, then tail                     */
    uint16_t tail;
    uint16_t pos;                  /* byte in the frame                       */
    uint8_t  bit;                  /* bit in the byte (or in the flag)        */
    uint8_t  ones;                 /* run of 1s inside the frame              */
    uint8_t  stage;                /* 0 preamble, 1 frame, 2 tail, 3 done     */
} afsk_bits_t;

void   afsk_bits_init(afsk_bits_t *g, const uint8_t *frame, uint16_t len,
                      uint16_t preamble_flags, uint16_t tail_flags);
/* The next bit (0/1) before NRZI, or -1 once everything is sent. */
int8_t afsk_bits_next(afsk_bits_t *g);

#endif
