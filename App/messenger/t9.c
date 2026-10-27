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

#include "t9.h"

static const char *const KEYS_TEXT[10] = {
    " 0", ".,?!'-/:@()+=*#1", "abc2", "def3", "ghi4", "jkl5", "mno6", "pqrs7", "tuv8", "wxyz9",
};
static const char *const KEYS_CALL[10] = {
    "0", "-1", "ABC2", "DEF3", "GHI4", "JKL5", "MNO6", "PQRS7", "TUV8", "WXYZ9",
};
static const char *const MODE_NAMES[T9_MODE_COUNT] = {"Abc", "abc", "ABC", "123"};

void t9_init(t9_t *t, uint8_t max, bool callsign, const char *initial)
{
    memset(t, 0, sizeof(*t));
    t->max = max > T9_MAX ? T9_MAX : max;
    t->callsign = callsign;
    t->mode = callsign ? T9_MODE_UPPER : T9_MODE_SENTENCE;
    t->key = -1;
    if (initial) {
        strncpy(t->buf, initial, t->max);
        t->buf[t->max] = '\0';
        t->len = (uint8_t)strlen(t->buf);
        t->cursor = t->len;
    }
}

static bool capital_here(const t9_t *t)
{
    if (t->mode == T9_MODE_UPPER || t->callsign)
        return true;
    if (t->mode != T9_MODE_SENTENCE)
        return false;
    /* start of text, or after ". ", "! ", "? " */
    int8_t i = (int8_t)(t->cursor - 1);
    while (i >= 0 && t->buf[i] == ' ')
        i--;
    return i < 0 || t->buf[i] == '.' || t->buf[i] == '!' || t->buf[i] == '?';
}

char t9_pending(const t9_t *t)
{
    if (t->key < 0)
        return 0;
    const char *set = (t->callsign ? KEYS_CALL : KEYS_TEXT)[t->key];
    char c = set[t->tap % strlen(set)];
    if (c >= 'a' && c <= 'z' && capital_here(t))
        c = (char)(c - 32);
    return c;
}

static void insert(t9_t *t, char c)
{
    if (t->len >= t->max)
        return;
    memmove(t->buf + t->cursor + 1, t->buf + t->cursor, (size_t)(t->len - t->cursor + 1u));
    t->buf[t->cursor++] = c;
    t->len++;
}

void t9_commit(t9_t *t)
{
    const char c = t9_pending(t);
    t->key = -1;
    t->tap = 0;
    if (c)
        insert(t, c);
}

void t9_digit(t9_t *t, uint8_t digit, uint32_t now)
{
    if (digit > 9u)
        return;
    if (t->mode == T9_MODE_DIGITS) {
        t9_commit(t);
        insert(t, (char)('0' + digit));
        return;
    }
    if (t->key == (int8_t)digit && now - t->at < T9_TAP_MS) {
        t->tap++;                      /* next letter on the same key */
    } else {
        t9_commit(t);
        if (t->len >= t->max)
            return;
        t->key = (int8_t)digit;
        t->tap = 0;
    }
    t->at = now;
}

bool t9_tick(t9_t *t, uint32_t now)
{
    if (t->key >= 0 && now - t->at >= T9_TAP_MS) {
        t9_commit(t);
        return true;
    }
    return false;
}

void t9_backspace(t9_t *t)
{
    if (t->key >= 0) {                 /* drop the letter being chosen */
        t->key = -1;
        t->tap = 0;
        return;
    }
    if (t->cursor == 0)
        return;
    memmove(t->buf + t->cursor - 1, t->buf + t->cursor, (size_t)(t->len - t->cursor + 1u));
    t->cursor--;
    t->len--;
}

void t9_left(t9_t *t)
{
    t9_commit(t);
    if (t->cursor > 0)
        t->cursor--;
}

void t9_right(t9_t *t)
{
    t9_commit(t);
    if (t->cursor < t->len)
        t->cursor++;
}

void t9_next_mode(t9_t *t)
{
    t9_commit(t);
    if (t->callsign) {                 /* letters or digits only */
        t->mode = t->mode == T9_MODE_DIGITS ? T9_MODE_UPPER : T9_MODE_DIGITS;
        return;
    }
    t->mode = (t9_mode_t)((t->mode + 1) % T9_MODE_COUNT);
}

const char *t9_mode_name(const t9_t *t)
{
    return MODE_NAMES[t->mode];
}
