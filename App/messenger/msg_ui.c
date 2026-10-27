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

#include "app/generic.h"
#include "audio.h"
#include "driver/st7565.h"
#include "misc.h"
#include "ui/helper.h"
#include "ui/ui.h"

#include "mailbox.h"
#include "messenger.h"
#include "msg_fw.h"
#include "msg_hw.h"
#include "msg_port.h"
#include "msg_ui.h"
#include "t9.h"

#define ROW_CHARS   18u        /* small font: 7 px per character */
#define LIST_ROWS   5u         /* lines 1-5 */
#define POPUP_MS    1500u

typedef enum {
    V_LIST = 0,
    V_READ,
    V_TO,
    V_TEXT,
    V_SETUP,
    V_EDIT_CALL,
    V_EDIT_STATUS,
    V_CONFIRM,
} view_t;

typedef enum { CONFIRM_DELETE = 0, CONFIRM_CLEAR } confirm_t;

enum {
    S_ENABLE = 0, S_CALL, S_SSID, S_FREQ, S_PATH, S_BEACON, S_STATUS, S_BEEP, S_AUDIO, S_CLEAR,
    S_COUNT
};

static const char *const SETUP_LABEL[S_COUNT] = {
    "Messenger", "Call", "SSID", "Freq", "Path", "Beacon", "Status", "Beep", "APRS audio",
    "Clear mailbox",
};

static const uint32_t FREQS[] = {14480000u, 14439000u, 14464000u, 14466000u, 14517500u, 14582500u};
#define FREQ_COUNT (sizeof FREQS / sizeof FREQS[0])

static const char *const PATH_TXT[MSG_PATH_COUNT] = {"DIRECT", "WIDE1", "WIDE1+2"};

static view_t    gView;
static view_t    gBack;          /* where a confirm or popup returns to       */
static confirm_t gConfirm;
static int16_t   gSel = MBX_NONE, gTop = MBX_NONE;
static uint8_t   gReadTop;       /* first text row shown in V_READ            */
static uint8_t   gSetupSel, gSetupTop;
static t9_t      gEdit;
static char      gTo[AX25_CALL_STR];
static char      gPopup[20];
static uint32_t  gPopupUntil;

/* ---------------------------------------------------------- drawing -- */

static void print(const char *s, uint8_t x, uint8_t line)
{
    char row[ROW_CHARS + 1];
    strncpy(row, s, ROW_CHARS);
    row[ROW_CHARS] = '\0';
    UI_PrintStringSmallNormal(row, x, 0, line);
}

static void invert_line(uint8_t line)
{
    for (uint8_t x = 0; x < LCD_WIDTH; x++)
        gFrameBuffer[line][x] ^= 0xFFu;
}

static void tiny(const char *s, uint8_t x, uint8_t line)
{
    GUI_DisplaySmallest(s, x, (uint8_t)(line * 8u + 1u), false, true);
}

static void title(const char *s)
{
    print(s, 0, 0);
    invert_line(0);
}

/* "label ..... value" in one row */
static void label_value(uint8_t line, const char *label, const char *value)
{
    char row[ROW_CHARS + 1];
    memset(row, ' ', ROW_CHARS);
    row[ROW_CHARS] = '\0';
    const size_t ll = strlen(label);
    memcpy(row, label, ll < ROW_CHARS ? ll : ROW_CHARS);
    size_t vl = strlen(value);
    if (vl > ROW_CHARS - 1u)
        vl = ROW_CHARS - 1u;
    memcpy(row + ROW_CHARS - vl, value, vl);
    print(row, 0, line);
}

static char *put_u(char *o, uint32_t v)
{
    char rev[10];
    uint8_t n = 0;
    do {
        rev[n++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v);
    while (n)
        *o++ = rev[--n];
    *o = '\0';
    return o;
}

static void freq_text(uint32_t f, char *out)
{
    char *o = put_u(out, f / 100000u);
    *o++ = '.';
    const uint32_t k = (f / 100u) % 1000u;
    *o++ = (char)('0' + k / 100u);
    *o++ = (char)('0' + (k / 10u) % 10u);
    *o++ = (char)('0' + k % 10u);
    *o = '\0';
}

static void popup(const char *text)
{
    strncpy(gPopup, text, sizeof gPopup - 1u);
    gPopup[sizeof gPopup - 1u] = '\0';
    gPopupUntil = msg_port_ms() + POPUP_MS;
    if (gPopupUntil == 0)
        gPopupUntil = 1;
    gUpdateDisplay = true;
}

/* a dark box over lines 2-4 with the text inverted, a light margin around */
static void draw_popup(void)
{
    for (uint8_t line = 2; line <= 4; line++)
        memset(gFrameBuffer[line], 0, LCD_WIDTH);
    const uint8_t len = (uint8_t)strlen(gPopup);
    const uint8_t x = (uint8_t)((LCD_WIDTH - len * 7u) / 2u);
    print(gPopup, x, 3);
    for (uint8_t xx = 6; xx < LCD_WIDTH - 6u; xx++) {
        gFrameBuffer[2][xx] = 0xE0u;
        gFrameBuffer[3][xx] ^= 0xFFu;
        gFrameBuffer[4][xx] = 0x07u;
    }
}

/* ----------------------------------------------------------- list ---- */

static char mark_of(int16_t slot, const mbx_rec_t *r)
{
    if (r->dir == MBX_DIR_IN)
        return (r->flags & MBX_F_READ) ? '*' : ' ';   /* bit still set: unread */
    if (MSG_SlotPending(slot, NULL))
        return '>';
    if (!(r->flags & MBX_F_DELIVERED))
        return '+';
    if (!(r->flags & MBX_F_FAILED))
        return '!';
    return '-';
}

static void draw_list(void)
{
    char head[ROW_CHARS + 1];
    char *o = head;
    memcpy(o, "MESSAGES", 8);
    o += 8;
    const uint16_t unread = mbx_unread();
    if (unread) {
        *o++ = ' ';
        o = put_u(o, unread);
        memcpy(o, " new", 5);
    } else {
        *o = '\0';
    }
    title(head);

    const msg_config_t *cfg = MSG_GetConfig();
    if (gTop == MBX_NONE) {
        print(cfg->enabled ? "No messages yet" : "Messenger is off", 0, 2);
        print("* write  F setup", 0, 4);
    } else {
        int16_t s = gTop;
        for (uint8_t row = 0; row < LIST_ROWS && s != MBX_NONE; row++, s = mbx_older(s)) {
            mbx_rec_t r;
            char line[ROW_CHARS + 1];
            if (!mbx_read(s, &r)) {
                strcpy(line, " (damaged)");
            } else {
                memset(line, ' ', ROW_CHARS);
                line[0] = mark_of(s, &r);
                const size_t pl = strlen(r.peer);
                memcpy(line + 1, r.peer, pl > 9u ? 9u : pl);
                const size_t tl = strlen(r.text);
                const size_t room = ROW_CHARS - 11u;
                memcpy(line + 11, r.text, tl < room ? tl : room);
                line[ROW_CHARS] = '\0';
            }
            print(line, 0, (uint8_t)(1u + row));
            if (s == gSel)
                invert_line((uint8_t)(1u + row));
        }
    }

    if (cfg->enabled && !msg_port_has_aprs_vfo()) {
        char warn[32] = "NO VFO ON ";
        freq_text(cfg->freq, warn + strlen(warn));
        tiny(warn, 0, 6);
    } else {
        tiny("MENU READ  * NEW  F SETUP", 0, 6);
    }
}

static void list_reset(void)
{
    gSel = gTop = mbx_newest();
}

static void list_down(void)
{
    if (gSel == MBX_NONE)
        return;
    const int16_t next = mbx_older(gSel);
    if (next == MBX_NONE)
        return;
    /* is gSel the last visible row? then scroll */
    int16_t s = gTop;
    uint8_t row = 0;
    while (s != MBX_NONE && s != gSel && row < LIST_ROWS) {
        s = mbx_older(s);
        row++;
    }
    if (row >= LIST_ROWS - 1u)
        gTop = mbx_older(gTop);
    gSel = next;
}

static void list_up(void)
{
    if (gSel == MBX_NONE)
        return;
    const int16_t prev = mbx_newer(gSel);
    if (prev == MBX_NONE)
        return;
    if (gSel == gTop)
        gTop = prev;
    gSel = prev;
}

/* ----------------------------------------------------------- read ---- */

/* word wrap: start offsets of up to 8 rows */
static uint8_t wrap(const char *t, uint8_t starts[8])
{
    const uint8_t len = (uint8_t)strlen(t);
    uint8_t n = 0, pos = 0;
    while (pos < len && n < 8u) {
        starts[n++] = pos;
        if ((uint8_t)(len - pos) <= ROW_CHARS) {
            pos = len;
            break;
        }
        uint8_t cut = (uint8_t)(pos + ROW_CHARS);
        uint8_t sp = cut;
        while (sp > pos && t[sp] != ' ')
            sp--;
        pos = (sp > pos) ? (uint8_t)(sp + 1u) : cut;
    }
    return n;
}

static void draw_read(void)
{
    mbx_rec_t r;
    if (!mbx_read(gSel, &r)) {
        title("MESSAGE");
        print("(damaged)", 0, 2);
        return;
    }
    char head[ROW_CHARS + 1];
    strcpy(head, r.dir == MBX_DIR_IN ? "From " : "To ");
    strcat(head, r.peer);
    title(head);

    uint8_t starts[8];
    const uint8_t rows = wrap(r.text, starts);
    if (gReadTop + LIST_ROWS > rows)
        gReadTop = rows > LIST_ROWS ? (uint8_t)(rows - LIST_ROWS) : 0u;
    for (uint8_t i = 0; i < LIST_ROWS && gReadTop + i < rows; i++) {
        const uint8_t k = (uint8_t)(gReadTop + i);
        const uint8_t end = (k + 1u < rows) ? starts[k + 1] : (uint8_t)strlen(r.text);
        char line[ROW_CHARS + 1];
        uint8_t n = (uint8_t)(end - starts[k]);
        if (n > ROW_CHARS)
            n = ROW_CHARS;
        memcpy(line, r.text + starts[k], n);
        while (n && line[n - 1u] == ' ')
            n--;
        line[n] = '\0';
        print(line, 0, (uint8_t)(1u + i));
    }

    if (r.dir == MBX_DIR_IN) {
        tiny("MENU REPLY   0 DELETE", 0, 6);
        return;
    }
    char st[32];
    uint8_t tries;
    if (MSG_SlotPending(gSel, &tries)) {
        char *o = st;
        memcpy(o, "SENDING ", 8);
        o = put_u(o + 8, tries);
        *o++ = '/';
        o = put_u(o, MSG_RETRIES);
    } else if (!(r.flags & MBX_F_DELIVERED)) {
        strcpy(st, "DELIVERED    0 DELETE");
    } else if (!(r.flags & MBX_F_FAILED)) {
        strcpy(st, "NO ACK  MENU RESEND");
    } else {
        strcpy(st, "NOT CONFIRMED MENU RESEND");
    }
    tiny(st, 0, 6);
}

/* ------------------------------------------------------- compose ---- */

static void open_compose(const char *to)
{
    if (to && to[0]) {
        strncpy(gTo, to, sizeof gTo - 1u);
        gTo[sizeof gTo - 1u] = '\0';
        t9_init(&gEdit, APRS_TEXT_MAX, false, NULL);
        gView = V_TEXT;
    } else {
        gTo[0] = '\0';
        t9_init(&gEdit, AX25_CALL_STR - 1u, true, NULL);
        gView = V_TO;
    }
}

/* the editor text with the pending letter, char-wrapped from line first */
static void draw_editor(uint8_t first, uint8_t rows)
{
    char text[T9_MAX + 2];
    const char pend = t9_pending(&gEdit);
    memcpy(text, gEdit.buf, gEdit.cursor);
    uint8_t n = gEdit.cursor;
    if (pend)
        text[n++] = pend;
    memcpy(text + n, gEdit.buf + gEdit.cursor, (size_t)(gEdit.len - gEdit.cursor));
    n = (uint8_t)(n + gEdit.len - gEdit.cursor);
    text[n] = '\0';

    /* scroll so the cursor row is visible */
    const uint8_t crow = (uint8_t)(gEdit.cursor / ROW_CHARS);
    const uint8_t top = crow >= rows ? (uint8_t)(crow - rows + 1u) : 0u;
    for (uint8_t i = 0; i < rows; i++) {
        const uint16_t off = (uint16_t)((top + i) * ROW_CHARS);
        if (off > n)
            break;
        print(text + off, 0, (uint8_t)(first + i));
    }
    /* the cursor: the pending letter inverted, or a bar under the position */
    const uint8_t line = (uint8_t)(first + crow - top);
    const uint8_t x = (uint8_t)((gEdit.cursor % ROW_CHARS) * 7u);
    for (uint8_t k = 0; k < 7u && x + k < LCD_WIDTH; k++)
        gFrameBuffer[line][x + k] ^= pend ? 0x7Fu : 0x80u;
}

static void draw_compose(void)
{
    char head[ROW_CHARS + 1];
    char hint[40];
    if (gView == V_TO) {
        title("NEW MESSAGE");
        print("To (callsign):", 0, 2);
        draw_editor(3, 1);
        strcpy(hint, "MENU NEXT  * ");
        strcat(hint, t9_mode_name(&gEdit));
        tiny(hint, 0, 6);
        return;
    }
    strcpy(head, "To ");
    strcat(head, gTo);
    title(head);
    draw_editor(1, 4);
    char *o = hint;
    strcpy(o, t9_mode_name(&gEdit));
    o += strlen(o);
    *o++ = ' ';
    o = put_u(o, gEdit.len);
    *o++ = '/';
    o = put_u(o, APRS_TEXT_MAX);
    strcpy(o, "   MENU SEND");
    tiny(hint, 0, 6);
}

static void send_now(void)
{
    t9_commit(&gEdit);
    static const char *const ERR[] = {
        [MSG_SEND_OK]          = "Queued",
        [MSG_SEND_NO_CALL]     = "Set your call",
        [MSG_SEND_BAD_CALL]    = "Bad callsign",
        [MSG_SEND_EMPTY]       = "Empty message",
        [MSG_SEND_OUTBOX_FULL] = "Outbox full",
        [MSG_SEND_FLASH]       = "Flash error",
        [MSG_SEND_DISABLED]    = "Messenger off",
    };
    const msg_send_result_t r = MSG_Send(gTo, gEdit.buf);
    popup(ERR[r]);
    if (r == MSG_SEND_OK) {
        gView = V_LIST;
        list_reset();
    }
}

/* ---------------------------------------------------------- setup ---- */

static void setup_value(uint8_t item, char *v)
{
    const msg_config_t *c = MSG_GetConfig();
    v[0] = '\0';
    switch (item) {
    case S_ENABLE: strcpy(v, c->enabled ? "ON" : "OFF"); break;
    case S_CALL:   strcpy(v, c->call[0] ? c->call : "---"); break;
    case S_SSID:   if (c->ssid) put_u(v, c->ssid); else strcpy(v, "none"); break;
    case S_FREQ:   freq_text(c->freq, v); break;
    case S_PATH:   strcpy(v, PATH_TXT[c->path]); break;
    case S_BEACON:
        strcpy(v, MSG_BeaconMinutesText(c->beacon));
        if (c->beacon)
            strcat(v, " min");
        break;
    case S_STATUS: strncpy(v, c->status, 8); v[8] = '\0'; break;
    case S_BEEP:   strcpy(v, c->beep ? "ON" : "OFF"); break;
    case S_AUDIO:  strcpy(v, c->audio ? "ON" : "OFF"); break;
    default: break;
    }
}

static void draw_setup(void)
{
    title("MESSENGER SETUP");
    for (uint8_t row = 0; row < LIST_ROWS; row++) {
        const uint8_t item = (uint8_t)(gSetupTop + row);
        if (item >= S_COUNT)
            break;
        char v[24];
        setup_value(item, v);
        label_value((uint8_t)(1u + row), SETUP_LABEL[item], v);
        if (item == gSetupSel)
            invert_line((uint8_t)(1u + row));
    }
    /* bring-up figures: frames heard / sent, DSP load, audio level */
    msg_hw_diag_t d;
    msg_hw_diag(&d);
    char diag[40];
    char *o = diag;
    memcpy(o, "RX", 2);
    o = put_u(o + 2, MSG_FramesHeard());
    memcpy(o, " TX", 3);
    o = put_u(o + 3, MSG_FramesSent());
    memcpy(o, " DSP", 4);
    o = put_u(o + 4, d.load_pm / 10u);
    *o++ = '%';
    memcpy(o, " LV", 3);
    o = put_u(o + 3, d.level_pp);
    if (d.overruns) {
        memcpy(o, " OV", 3);
        put_u(o + 3, d.overruns);
    }
    tiny(diag, 0, 6);
}

static void setup_change(int8_t dir)
{
    msg_config_t c = *MSG_GetConfig();
    switch (gSetupSel) {
    case S_ENABLE:
        if (!c.enabled && !c.call[0]) {
            popup("Set your call");
            return;
        }
        c.enabled = !c.enabled;
        break;
    case S_CALL:
        t9_init(&gEdit, AX25_CALL_LEN, true, c.call);
        gView = V_EDIT_CALL;
        return;
    case S_SSID:
        c.ssid = (uint8_t)((c.ssid + 16 + dir) % 16);
        break;
    case S_FREQ: {
        uint8_t i = 0;
        while (i < FREQ_COUNT && FREQS[i] != c.freq)
            i++;
        i = (uint8_t)(i >= FREQ_COUNT ? 0u : (i + FREQ_COUNT + (uint8_t)dir) % FREQ_COUNT);
        c.freq = FREQS[i];
        break;
    }
    case S_PATH:
        c.path = (uint8_t)((c.path + MSG_PATH_COUNT + dir) % MSG_PATH_COUNT);
        break;
    case S_BEACON:
        c.beacon = (uint8_t)((c.beacon + MSG_BEACON_COUNT + dir) % MSG_BEACON_COUNT);
        break;
    case S_STATUS:
        t9_init(&gEdit, MSG_STATUS_LEN, false, c.status);
        gView = V_EDIT_STATUS;
        return;
    case S_BEEP:  c.beep = !c.beep; break;
    case S_AUDIO: c.audio = !c.audio; break;
    case S_CLEAR:
        gConfirm = CONFIRM_CLEAR;
        gBack = V_SETUP;
        gView = V_CONFIRM;
        return;
    default:
        return;
    }
    MSG_SetConfig(&c);
}

static void draw_edit_setting(void)
{
    title(gView == V_EDIT_CALL ? "YOUR CALLSIGN" : "BEACON STATUS");
    print(gView == V_EDIT_CALL ? "Without SSID:" : "Status text:", 0, 2);
    draw_editor(3, gView == V_EDIT_CALL ? 1u : 2u);
    char hint[40] = "MENU SAVE  * ";
    strcat(hint, t9_mode_name(&gEdit));
    tiny(hint, 0, 6);
}

static void save_edit_setting(void)
{
    t9_commit(&gEdit);
    msg_config_t c = *MSG_GetConfig();
    if (gView == V_EDIT_CALL) {
        ax25_addr_t a;
        if (gEdit.len && (!ax25_addr_parse(gEdit.buf, &a) || a.ssid || strchr(gEdit.buf, '-'))) {
            popup("Bad callsign");
            return;
        }
        strcpy(c.call, gEdit.buf);
        if (!c.call[0])
            c.enabled = 0;
    } else {
        strcpy(c.status, gEdit.buf);
    }
    MSG_SetConfig(&c);
    gView = V_SETUP;
}

/* -------------------------------------------------------- confirm ---- */

static void draw_confirm(void)
{
    title(gConfirm == CONFIRM_CLEAR ? "CLEAR MAILBOX" : "DELETE");
    print(gConfirm == CONFIRM_CLEAR ? "Delete all" : "Delete this", 0, 2);
    print("messages?", 0, 3);
    tiny("MENU YES   EXIT NO", 0, 6);
}

/* ------------------------------------------------------------ API ---- */

void MSG_OpenScreen(void)
{
    gView = V_LIST;
    gPopupUntil = 0;
    list_reset();
    GUI_SelectNextDisplay(DISPLAY_MESSENGER);
}

void MSG_DisplayScreen(void)
{
    UI_DisplayClear();
    switch (gView) {
    case V_LIST:        draw_list(); break;
    case V_READ:        draw_read(); break;
    case V_TO:
    case V_TEXT:        draw_compose(); break;
    case V_SETUP:       draw_setup(); break;
    case V_EDIT_CALL:
    case V_EDIT_STATUS: draw_edit_setting(); break;
    case V_CONFIRM:     draw_confirm(); break;
    }
    if (gPopupUntil)
        draw_popup();
    ST7565_BlitFullScreen();
}

static void open_read(void)
{
    if (gSel == MBX_NONE)
        return;
    mbx_rec_t r;
    if (mbx_read(gSel, &r) && r.dir == MBX_DIR_IN)
        mbx_set_flags(gSel, MBX_F_READ);
    gReadTop = 0;
    gView = V_READ;
    gUpdateStatus = true;
}

static void keys_editor(KEY_Code_t key, bool held)
{
    const uint32_t now = msg_port_ms();
    if (key <= KEY_9 && !held) {
        t9_digit(&gEdit, (uint8_t)key, now);
        return;
    }
    switch (key) {
    case KEY_STAR: if (!held) t9_next_mode(&gEdit); break;
    case KEY_UP:   t9_left(&gEdit); break;
    case KEY_DOWN: t9_right(&gEdit); break;
    default: break;
    }
}

void MSG_ProcessKeys(KEY_Code_t Key, bool bKeyPressed, bool bKeyHeld)
{
    if (Key == KEY_PTT) {
        GENERIC_Key_PTT(bKeyPressed);
        return;
    }
    if (!bKeyPressed)
        return;                        /* act on press and on repeats */
    const bool held = bKeyHeld;
    const bool repeatable = Key == KEY_UP || Key == KEY_DOWN;
    if (held && !repeatable && Key != KEY_EXIT)
        return;

    if (gPopupUntil) {                 /* any key closes a popup */
        gPopupUntil = 0;
        gUpdateDisplay = true;
        if (Key != KEY_EXIT)
            return;
    }
    gBeepToPlay = BEEP_1KHZ_60MS_OPTIONAL;
    gUpdateDisplay = true;

    switch (gView) {
    case V_LIST:
        switch (Key) {
        case KEY_UP:   list_up(); break;
        case KEY_DOWN: list_down(); break;
        case KEY_MENU: open_read(); break;
        case KEY_STAR: open_compose(NULL); break;
        case KEY_F:
            gSetupSel = gSetupTop = 0;
            gView = V_SETUP;
            break;
        case KEY_EXIT:
            GUI_SelectNextDisplay(DISPLAY_MAIN);
            break;
        default: break;
        }
        break;

    case V_READ: {
        mbx_rec_t r;
        const bool ok = mbx_read(gSel, &r);
        switch (Key) {
        case KEY_UP:   if (gReadTop) gReadTop--; break;
        case KEY_DOWN: gReadTop++; break;          /* clamped while drawing */
        case KEY_MENU:
            if (!ok)
                break;
            if (r.dir == MBX_DIR_IN) {
                open_compose(r.peer);
            } else if (!MSG_SlotPending(gSel, NULL) && (r.flags & MBX_F_DELIVERED)) {
                popup(MSG_Resend(gSel) == MSG_SEND_OK ? "Sending again" : "Cannot send");
            }
            break;
        case KEY_0:
            gConfirm = CONFIRM_DELETE;
            gBack = V_READ;
            gView = V_CONFIRM;
            break;
        case KEY_EXIT:
            gView = V_LIST;
            break;
        default: break;
        }
        break;
    }

    case V_TO:
    case V_TEXT:
    case V_EDIT_CALL:
    case V_EDIT_STATUS:
        if (Key == KEY_EXIT) {
            if (held) {                /* long EXIT: leave the editor */
                gView = (gView == V_EDIT_CALL || gView == V_EDIT_STATUS) ? V_SETUP : V_LIST;
            } else if (gEdit.len || t9_pending(&gEdit)) {
                t9_backspace(&gEdit);
            } else if (gView == V_TEXT) {
                t9_init(&gEdit, AX25_CALL_STR - 1u, true, gTo);   /* back to the recipient */
                gView = V_TO;
            } else {
                gView = (gView == V_EDIT_CALL || gView == V_EDIT_STATUS) ? V_SETUP : V_LIST;
            }
            break;
        }
        if (Key == KEY_MENU) {
            if (gView == V_TO) {
                t9_commit(&gEdit);
                ax25_addr_t a;
                if (!ax25_addr_parse(gEdit.buf, &a) || a.repeated) {
                    popup("Bad callsign");
                    break;
                }
                ax25_addr_format(&a, gTo);
                t9_init(&gEdit, APRS_TEXT_MAX, false, NULL);
                gView = V_TEXT;
            } else if (gView == V_TEXT) {
                send_now();
            } else {
                save_edit_setting();
            }
            break;
        }
        keys_editor(Key, held);
        break;

    case V_SETUP:
        switch (Key) {
        case KEY_UP:
            gSetupSel = (uint8_t)(gSetupSel ? gSetupSel - 1u : S_COUNT - 1u);
            break;
        case KEY_DOWN:
            gSetupSel = (uint8_t)((gSetupSel + 1u) % S_COUNT);
            break;
        case KEY_MENU: setup_change(+1); break;
        case KEY_F:    setup_change(-1); break;
        case KEY_EXIT:
            gView = V_LIST;
            list_reset();
            break;
        default: break;
        }
        if (gSetupSel < gSetupTop)
            gSetupTop = gSetupSel;
        else if (gSetupSel >= gSetupTop + LIST_ROWS)
            gSetupTop = (uint8_t)(gSetupSel - LIST_ROWS + 1u);
        break;

    case V_CONFIRM:
        if (Key == KEY_MENU) {
            if (gConfirm == CONFIRM_CLEAR) {
                mbx_clear();
                popup("Mailbox cleared");
                list_reset();
                gView = V_SETUP;
            } else {
                mbx_set_flags(gSel, MBX_F_DELETED | MBX_F_READ);
                list_reset();
                gView = V_LIST;
            }
            gUpdateStatus = true;
        } else if (Key == KEY_EXIT) {
            gView = gBack;
        }
        break;
    }
}

void msg_ui_tick(void)
{
    if (gScreenToDisplay != DISPLAY_MESSENGER)
        return;
    const uint32_t now = msg_port_ms();
    if ((gView == V_TO || gView == V_TEXT || gView == V_EDIT_CALL || gView == V_EDIT_STATUS)
        && t9_tick(&gEdit, now))
        gUpdateDisplay = true;
    if (gPopupUntil && (int32_t)(now - gPopupUntil) >= 0) {
        gPopupUntil = 0;
        gUpdateDisplay = true;
    }
    /* the setup screen shows live figures, the list the sending state */
    static uint8_t refresh;
    if ((gView == V_SETUP || gView == V_LIST || gView == V_READ) && ++refresh >= 50u) {
        refresh = 0;
        gUpdateDisplay = true;
    }
}
