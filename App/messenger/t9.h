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

/* Multi-tap text entry on the number keys, as on a classic phone keypad:
 * 2 = a b c 2, 3 = d e f 3, ... 0 = space 0, 1 = punctuation. Pressing the
 * same key again within T9_TAP_MS steps through its letters; another key or
 * the timeout fixes the letter. Pure C, built by the host tests. */

#ifndef MESSENGER_T9_H
#define MESSENGER_T9_H

#include <stdbool.h>
#include <stdint.h>

#define T9_TAP_MS   1000u
#define T9_MAX      67u

typedef enum {
    T9_MODE_SENTENCE = 0,          /* "Abc": capital after start / . ! ?      */
    T9_MODE_LOWER,                 /* "abc"                                   */
    T9_MODE_UPPER,                 /* "ABC"                                   */
    T9_MODE_DIGITS,                /* "123"                                   */
    T9_MODE_COUNT
} t9_mode_t;

typedef struct {
    char     buf[T9_MAX + 1];
    uint8_t  len;
    uint8_t  cursor;               /* insert position, 0..len                 */
    uint8_t  max;
    bool     callsign;             /* upper case letters, digits and '-' only */
    t9_mode_t mode;
    int8_t   key;                  /* key of the letter being chosen, -1 none */
    uint8_t  tap;                  /* which letter of that key                */
    uint32_t at;                   /* time of the last tap                    */
} t9_t;

void        t9_init(t9_t *t, uint8_t max, bool callsign, const char *initial);
/* A number key 0..9. */
void        t9_digit(t9_t *t, uint8_t digit, uint32_t now);
/* Fixes the pending letter once T9_TAP_MS passed; true if the text changed. */
bool        t9_tick(t9_t *t, uint32_t now);
void        t9_commit(t9_t *t);
void        t9_backspace(t9_t *t);
void        t9_left(t9_t *t);
void        t9_right(t9_t *t);
void        t9_next_mode(t9_t *t);
const char *t9_mode_name(const t9_t *t);   /* "Abc", "abc", "ABC", "123"     */
/* The pending letter (or 0) and where it is shown. */
char        t9_pending(const t9_t *t);

#endif
