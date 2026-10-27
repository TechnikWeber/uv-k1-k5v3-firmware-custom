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
void     msg_port_notify(msg_notify_t kind) { (void)kind; }
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

    /* first start: no callsign yet */
    MSG_OpenScreen();
    type("3 555 ");                    /* D L */
    key(KEY_STAR);                     /* 123 */
    key(KEY_4);
    key(KEY_STAR);                     /* ABC */
    type("7 9 ");                      /* P W */
    shot("01_first_start");
    key(KEY_MENU);                     /* saved: on to "Messenger" */
    key(KEY_MENU);                     /* ON */
    key(KEY_DOWN);                     /* Call */
    key(KEY_DOWN);                     /* SSID: 0 -> 7 */
    for (int i = 0; i < 7; i++)
        key(KEY_MENU);
    key(KEY_DOWN);                     /* Channel */
    key(KEY_MENU);
    type("9 33 22 33 777 ");           /* W E B E R */
    key(KEY_MENU);
    shot("02_setup");

    /* history */
    MSG_Send("DL3QQQ", "Bin unterwegs, melde mich spaeter");
    run_ms(4000);
    rx("DL3QQQ", ":DL4PW-7  :ack1");
    run_ms(100);
    rx("DL1ABC-9", ":DL4PW-7  :Hallo Philipp, bist du heute Abend QRV auf 145.500?{12");
    run_ms(4000);
    rx("DL5ZZZ-7", ":BLN0WEBER:Essen um 19 Uhr, wer kommt?");
    run_ms(4000);
    MSG_Send("DL1ABC-9", "Ja gerne!");
    run_ms(4000);
    rx("DL1ABC-9", ":DL4PW-7  :ack2");
    run_ms(100);
    rx("DL1ABC-9", ":DL4PW-7  :Super, bis dann{13");
    run_ms(4000);

    key(KEY_EXIT);
    shot("03_list");

    key(KEY_DOWN); key(KEY_DOWN);      /* the channel message */
    key(KEY_MENU);
    shot("04_read_channel");
    key(KEY_MENU);                     /* answer in the channel */
    type("444 222 44 0 22 444 66 0 3 2 22 33 444");
    run_ms(1100);
    shot("05_write_channel");
    key(KEY_MENU);
    run_ms(2000);

    key(KEY_DOWN);                     /* "Super, bis dann" */
    key(KEY_1);                        /* chat with DL1ABC-9 */
    shot("06_chat");
    key(KEY_DOWN);                     /* "Ja gerne!" */
    key(KEY_MENU);
    shot("07_read_delivered");
    key(KEY_EXIT);
    key(KEY_STAR);                     /* write in the chat: straight to the text */
    type("666 55 0 22 444 7777 0 4 555 33 444 222 44");   /* Ok bis gleich */
    run_ms(1100);
    shot("08_write_private");
    key_long(KEY_EXIT);
    key(KEY_EXIT);                     /* leave the chat */

    key(KEY_STAR);                     /* new: recipient, F = channel */
    shot("09_new_to");
    key_long(KEY_EXIT);

    /* encryption, on a licence-free frequency */
    key(KEY_F);                        /* setup */
    for (int i = 0; i < 6; i++)
        key(KEY_DOWN);                 /* Freq */
    for (int i = 0; i < 6; i++)
        key(KEY_F);                    /* presets ... 446.00625 */
    key(KEY_DOWN); key(KEY_DOWN);      /* Key */
    key(KEY_MENU);
    type("4 33 44 33 444 6 0 444 7777 8 0 4 33 44 33 444 6 ");   /* Geheim ist geheim */
    shot("10_key");
    key(KEY_MENU);                     /* saved */
    run_ms(2000);
    key(KEY_UP);                       /* Encrypt */
    key(KEY_MENU);                     /* ON */
    run_ms(2000);
    shot("11_setup_crypto");
    key(KEY_EXIT);
    key(KEY_STAR);
    key(KEY_F);                        /* to the channel */
    type("222 666 6 444 66 4 ");       /* Coming */
    shot("12_write_encrypted");

    return TEST_REPORT("ui_sim");
}
