/* Host UI simulator: runs the real messages screen (msg_ui.c) and messenger
 * core on the host, with the firmware's own font tables, presses keys like a
 * user would, and dumps each screen as a 128x64 bitmap (OUTDIR/NN_name.lcd:
 * the status line, then the 7 frame lines). test/render_lcd.py turns them
 * into PNG files. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test_util.h"
#include "flash_sim.h"

#include "audio.h"
#include "driver/st7565.h"
#include "font.h"
#include "misc.h"
#include "ui/helper.h"
#include "ui/ui.h"

#include "../mailbox.h"
#include "../messenger.h"
#include "../msg_fw.h"
#include "../msg_hw.h"
#include "../msg_port.h"
#include "../msg_ui.h"

/* ------------------------------------------------ firmware stand-ins -- */

uint8_t           gStatusLine[LCD_WIDTH];
uint8_t           gFrameBuffer[FRAME_LINES][LCD_WIDTH];
bool              gUpdateDisplay;
uint8_t           gUpdateStatus;
GUI_DisplayType_t gScreenToDisplay;
BEEP_Type_t       gBeepToPlay;

void GUI_SelectNextDisplay(GUI_DisplayType_t d) { gScreenToDisplay = d; gUpdateDisplay = true; }
void GENERIC_Key_PTT(bool pressed) { (void)pressed; }
void ST7565_BlitFullScreen(void) {}
void UI_DisplayClear(void) { memset(gFrameBuffer, 0, sizeof gFrameBuffer); }

/* the same glyph code as ui/helper.c */
static void print_buffer(const char *s, uint8_t *buffer)
{
    const size_t len = strlen(s);
    for (size_t i = 0; i < len; i++) {
        if (s[i] > ' ' && s[i] < 127) {
            const unsigned index = (unsigned)(s[i] - ' ' - 1);
            const uint32_t offset = (uint32_t)(i * (FONT_SMALL_WIDTH + 1u) + 1u);
            uint16_t bit = (uint16_t)(index * FONT_SMALL_WIDTH * 7u);
            for (uint8_t column = 0; column < FONT_SMALL_WIDTH; column++) {
                const uint16_t byte = bit >> 3;
                const uint16_t packed = gFontSmallPacked[byte] | ((uint16_t)gFontSmallPacked[byte + 1u] << 8);
                if (offset + column < LCD_WIDTH * 2u)
                    buffer[offset + column] = (packed >> (bit & 7u)) & 0x7Fu;
                bit += 7u;
            }
        }
    }
}

void UI_PrintStringSmallNormal(const char *s, uint8_t start, uint8_t end, uint8_t line)
{
    const size_t len = strlen(s);
    if (end > start)
        start = (uint8_t)(start + (((end - start) - len * (FONT_SMALL_WIDTH + 1u)) + 1u) / 2u);
    print_buffer(s, gFrameBuffer[line] + start);
}

static void put_pixel(uint8_t x, uint8_t y, bool fill)
{
    if (x >= LCD_WIDTH || y >= FRAME_LINES * 8u)
        return;
    if (fill)
        gFrameBuffer[y / 8u][x] |= (uint8_t)(1u << (y % 8u));
    else
        gFrameBuffer[y / 8u][x] &= (uint8_t)~(1u << (y % 8u));
}

void GUI_DisplaySmallest(const char *s, uint8_t x, uint8_t y, bool statusbar, bool fill)
{
    (void)statusbar;
    uint8_t c;
    while ((c = (uint8_t)*s++) != '\0') {
        c = (uint8_t)(c - 0x20u);
        for (int i = 0; i < 3; ++i) {
            uint8_t pixels = gFont3x5[c][i];
            for (int j = 0; j < 6; ++j) {
                if (pixels & 1u)
                    put_pixel((uint8_t)(x + i), (uint8_t)(y + j), fill);
                pixels >>= 1;
            }
        }
        x = (uint8_t)(x + 4u);
    }
}

/* ---------------------------------------------------------- the port -- */

static uint32_t now_ms = 5000;
static uint8_t  rx_q[4][AX25_FRAME_MAX];
static uint16_t rx_len[4];
static int      rx_n;

uint32_t msg_port_ms(void) { return now_ms; }
uint32_t msg_port_random(void) { static uint32_t r = 7; r = r * 1103515245u + 12345u; return r >> 8; }
bool     msg_port_channel_clear(void) { return true; }
void     msg_port_notify(void) {}
bool     msg_port_has_aprs_vfo(void) { return true; }
msg_tx_result_t msg_port_transmit(const uint8_t *f, uint16_t l) { (void)f; (void)l; now_ms += 1000; return MSG_TX_OK; }

bool msg_port_take_frame(uint8_t *buf, uint16_t *len)
{
    if (!rx_n)
        return false;
    rx_n--;
    memcpy(buf, rx_q[rx_n], rx_len[rx_n]);
    *len = rx_len[rx_n];
    return true;
}

void msg_hw_diag(msg_hw_diag_t *d)
{
    memset(d, 0, sizeof *d);
    d->load_pm = 37;
    d->level_pp = 612;
}

/* ------------------------------------------------------------ driving -- */

static void rx(const char *src, const char *info)
{
    ax25_addr_t s, d, p;
    ax25_addr_parse(src, &s);
    ax25_addr_parse("APDR16", &d);
    ax25_addr_parse("WIDE1-1", &p);
    rx_len[rx_n] = ax25_build_ui(rx_q[rx_n], AX25_FRAME_MAX, &d, &s, &p, 1, info,
                                 (uint16_t)strlen(info));
    rx_n++;
}

static void run_ms(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += 10) {
        MSG_Task();
        msg_ui_tick();
        now_ms += 10;
    }
}

static void key(KEY_Code_t k)
{
    MSG_ProcessKeys(k, true, false);
    MSG_ProcessKeys(k, false, false);
    now_ms += 150;
    run_ms(10);
}

static void key_long(KEY_Code_t k)
{
    MSG_ProcessKeys(k, true, false);
    MSG_ProcessKeys(k, true, true);
    MSG_ProcessKeys(k, false, true);
    run_ms(10);
}

/* multi-tap: digits, ' ' waits for the letter to settle */
static void type(const char *seq)
{
    for (; *seq; seq++) {
        if (*seq == ' ')
            run_ms(1100);
        else
            key((KEY_Code_t)(*seq - '0'));
    }
}

static const char *g_out;

static void status_line(void)
{
    memset(gStatusLine, 0, sizeof gStatusLine);
    static const uint8_t mail[9] = {0x7E, 0x46, 0x4A, 0x52, 0x62, 0x52, 0x4A, 0x46, 0x7E};
    static const uint8_t batt[13] = {0x3E, 0x22, 0x7F, 0x41, 0x5D, 0x5D, 0x41, 0x5D, 0x5D, 0x41, 0x5D, 0x5D, 0x7F};
    if (mbx_unread())
        memcpy(gStatusLine + 104, mail, sizeof mail);
    memcpy(gStatusLine + 115, batt, sizeof batt);
}

static void shot(const char *name)
{
    MSG_DisplayScreen();
    status_line();
    char path[512];
    snprintf(path, sizeof path, "%s/%s.lcd", g_out, name);
    FILE *f = fopen(path, "wb");
    if (!f) {
        printf("cannot write %s\n", path);
        g_failed++;
        return;
    }
    fwrite(gStatusLine, 1, sizeof gStatusLine, f);
    fwrite(gFrameBuffer, 1, sizeof gFrameBuffer, f);
    fclose(f);
    g_checks++;
}

int main(int argc, char **argv)
{
    g_out = argc > 1 ? argv[1] : ".";
    sim_reset(0xFF);
    MSG_CoreInit();
    msg_config_t c = *MSG_GetConfig();
    strcpy(c.call, "DL4PW");
    c.ssid = 7;
    c.enabled = 1;
    c.beacon = MSG_BEACON_OFF;
    MSG_SetConfig(&c);

    /* history: one sent and acknowledged, one still waiting */
    MSG_Send("DL3QQQ", "Bin unterwegs, melde mich spaeter");
    run_ms(4000);
    rx("DL3QQQ", ":DL4PW-7  :ack1");
    run_ms(100);
    MSG_Send("DL5ZZZ-7", "Treffen 18 Uhr am Bahnhof?");
    run_ms(4000);
    rx("DB0XYZ-10", "}DL2XYZ-5>APDR16,TCPIP,DB0XYZ-10*::DL4PW-7  :Gruesse aus Muenchen{a1");
    run_ms(100);
    rx("DL1ABC-9", ":DL4PW-7  :Hallo Philipp, bist du heute Abend QRV auf 145.500?{12");
    run_ms(4000);

    MSG_OpenScreen();
    shot("01_list");

    key(KEY_MENU);
    shot("02_read");

    key(KEY_MENU);                     /* reply */
    type("5 2 ");                      /* J a */
    key(KEY_0);
    type("4 33 777 66 33");            /* g e r n e (last letter still pending) */
    shot("03_reply_typing");
    run_ms(1100);
    key(KEY_1); key(KEY_1); key(KEY_1); key(KEY_1);   /* ! */
    run_ms(1100);
    shot("04_reply_done");

    key(KEY_MENU);                     /* send */
    shot("05_queued");
    run_ms(2000);
    shot("06_list_after");

    key(KEY_DOWN); key(KEY_DOWN); key(KEY_DOWN);
    key(KEY_MENU);
    shot("07_read_pending");
    key(KEY_EXIT);
    key(KEY_DOWN);
    key(KEY_MENU);
    shot("08_read_delivered");
    key(KEY_EXIT);

    key(KEY_STAR);                     /* new message: recipient */
    type("3 555 ");                    /* D L */
    key(KEY_STAR);                     /* 123 */
    key(KEY_1);
    key(KEY_STAR);                     /* ABC */
    type("2 22 222 ");                 /* A B C */
    shot("09_new_to");
    key(KEY_MENU);
    shot("10_new_text");
    key_long(KEY_EXIT);

    key(KEY_F);                        /* setup */
    shot("11_setup");
    for (int i = 0; i < 6; i++)
        key(KEY_DOWN);
    shot("12_setup_more");

    return TEST_REPORT("ui_sim");
}
