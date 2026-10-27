/* Host test: the messenger core with a simulated radio. */
#include <stdint.h>

#include "test_util.h"
#include "flash_sim.h"
#include "../mailbox.h"
#include "../messenger.h"
#include "../msg_port.h"

/* ------------------------------------------------------------- mock port -- */

static uint32_t        now_ms = 1000;
static bool            clear = true;
static msg_tx_result_t tx_result = MSG_TX_OK;
static int             notified;
static uint32_t        rnd = 12345;

static uint8_t  rx_q[8][AX25_FRAME_MAX];
static uint16_t rx_len[8];
static int      rx_n;

static char     tx_log[64][160];
static int      tx_n;

uint32_t msg_port_ms(void)     { return now_ms; }
uint32_t msg_port_random(void) { rnd = rnd * 1103515245u + 12345u; return rnd >> 8; }
bool     msg_port_channel_clear(void) { return clear; }
void     msg_port_notify(void) { notified++; }

bool msg_port_take_frame(uint8_t *buf, uint16_t *len)
{
    if (rx_n == 0)
        return false;
    memcpy(buf, rx_q[0], rx_len[0]);
    *len = rx_len[0];
    rx_n--;
    memmove(rx_q[0], rx_q[1], sizeof rx_q[0] * (size_t)rx_n);
    memmove(&rx_len[0], &rx_len[1], sizeof rx_len[0] * (size_t)rx_n);
    return true;
}

static void frame_text(const uint8_t *f, uint16_t len, char *out)
{
    ax25_frame_t fr;
    if (!ax25_parse_ui(f, len, &fr)) {
        strcpy(out, "?");
        return;
    }
    char a[AX25_CALL_STR];
    ax25_addr_format(&fr.src, a);
    strcpy(out, a);
    strcat(out, ">");
    ax25_addr_format(&fr.dst, a);
    strcat(out, a);
    for (uint8_t i = 0; i < fr.ndigi; i++) {
        strcat(out, ",");
        ax25_addr_format(&fr.digi[i], a);
        strcat(out, a);
    }
    strcat(out, ":");
    size_t o = strlen(out);
    memcpy(out + o, fr.info, fr.info_len);
    out[o + fr.info_len] = '\0';
}

msg_tx_result_t msg_port_transmit(const uint8_t *frame, uint16_t len)
{
    /* the FCS must be right */
    uint16_t crc = 0xFFFF;
    for (uint16_t i = 0; i < len; i++)
        crc = ax25_crc_byte(crc, frame[i]);
    CHECK_INT(crc, AX25_FCS_GOOD);
    if (tx_result != MSG_TX_OK)
        return tx_result;
    if (tx_n < 64)
        frame_text(frame, len, tx_log[tx_n++]);
    now_ms += 1000;                    /* a frame takes ~1 s on the air */
    return MSG_TX_OK;
}

/* ---------------------------------------------------------------- helpers -- */

static void rx(const char *src, const char *info)
{
    ax25_addr_t s, d, p;
    ax25_addr_parse(src, &s);
    ax25_addr_parse("APRS", &d);
    ax25_addr_parse("WIDE1-1", &p);
    rx_len[rx_n] = ax25_build_ui(rx_q[rx_n], AX25_FRAME_MAX, &d, &s, &p, 1, info,
                                 (uint16_t)strlen(info));
    rx_n++;
}

/* runs the 10 ms task for a while */
static void run_ms(uint32_t ms)
{
    const uint32_t end = now_ms + ms;
    while ((int32_t)(now_ms - end) < 0) {
        MSG_Task();
        now_ms += 10;
    }
}

static void reset_all(void)
{
    sim_reset(0xFF);
    now_ms = 1000;
    clear = true;
    tx_result = MSG_TX_OK;
    notified = 0;
    rx_n = 0;
    tx_n = 0;
    MSG_CoreInit();
    msg_config_t c = *MSG_GetConfig();
    strcpy(c.call, "DL4PW");
    c.ssid = 7;
    c.enabled = 1;
    c.beacon = MSG_BEACON_OFF;        /* most tests: no beacon */
    MSG_SetConfig(&c);
}

static uint8_t out_flags(int16_t slot)
{
    mbx_rec_t r;
    return mbx_read(slot, &r) ? r.flags : 0;
}

/* ------------------------------------------------------------------ tests -- */

static void test_config(void)
{
    sim_reset(0xFF);
    MSG_CoreInit();
    CHECK(!MSG_GetConfig()->enabled);
    CHECK_INT(MSG_GetConfig()->freq, MSG_DEFAULT_FREQ);
    ax25_addr_t me;
    CHECK(!MSG_MyAddr(&me));

    msg_config_t c = *MSG_GetConfig();
    strcpy(c.call, "DL4PW");
    c.ssid = 7;
    c.enabled = 1;
    for (int i = 0; i < 150; i++) {    /* more than one sector of saves */
        c.next_id = (uint16_t)(1 + i % 999);
        MSG_SetConfig(&c);
    }
    MSG_CoreInit();                    /* reboot */
    CHECK_STR(MSG_GetConfig()->call, "DL4PW");
    CHECK_INT(MSG_GetConfig()->ssid, 7);
    CHECK_INT(MSG_GetConfig()->next_id, 150);
    CHECK(MSG_MyAddr(&me));

    /* invalid settings are refused */
    msg_config_t bad = c;
    strcpy(bad.call, "D L");
    MSG_SetConfig(&bad);
    CHECK_STR(MSG_GetConfig()->call, "DL4PW");
    CHECK_INT(sim_violations, 0);
}

static void test_receive_and_ack(void)
{
    reset_all();
    rx("DL1ABC-9", ":DL4PW-7  :Hallo Philipp{42");
    run_ms(100);
    CHECK_INT(notified, 1);
    CHECK_INT(mbx_unread(), 1);
    mbx_rec_t r;
    CHECK(mbx_read(mbx_newest(), &r));
    CHECK_STR(r.peer, "DL1ABC-9");
    CHECK_STR(r.text, "Hallo Philipp");
    CHECK_INT(tx_n, 0);                /* waits a random 0.7-3 s first */

    run_ms(3500);
    CHECK_INT(tx_n, 1);
    CHECK_STR(tx_log[0], "DL4PW-7>APZK5M,WIDE1-1,WIDE2-1::DL1ABC-9 :ack42");

    /* a digipeater's copy 5 s later: nothing */
    rx("DL1ABC-9", ":DL4PW-7  :Hallo Philipp{42");
    run_ms(5000);
    CHECK_INT(tx_n, 1);
    CHECK_INT(notified, 1);

    /* the sender's retry 40 s later: ack again, not stored again */
    run_ms(40000);
    rx("DL1ABC-9", ":DL4PW-7  :Hallo Philipp{42");
    run_ms(5000);
    CHECK_INT(tx_n, 2);
    CHECK_INT(mbx_count(), 1);
    CHECK_INT(notified, 1);

    /* for another station, from myself, without id */
    rx("DL1ABC-9", ":DL4PW    :not me (no SSID){1");
    rx("DL4PW-7", ":DL4PW-7  :from myself{5");
    rx("DL2XYZ", ":DL4PW-7  :ohne Nummer");
    run_ms(5000);
    CHECK_INT(tx_n, 2);
    CHECK_INT(mbx_count(), 2);
    CHECK_INT(notified, 2);

    /* gated from the Internet: the ack goes to the station inside */
    rx("DB0XYZ-10", "}DL3QQQ>APDR16,TCPIP,DB0XYZ-10*::DL4PW-7  :via iGate{a1");
    run_ms(5000);
    CHECK_INT(tx_n, 3);
    CHECK_STR(tx_log[2], "DL4PW-7>APZK5M,WIDE1-1,WIDE2-1::DL3QQQ   :acka1");
}

static void test_send_retries(void)
{
    reset_all();
    CHECK_INT(MSG_Send("dl1abc-9", "Hallo zurueck"), MSG_SEND_OK);
    const int16_t slot = mbx_newest();
    CHECK_INT(mbx_unread(), 0);
    run_ms(4000);
    CHECK_INT(tx_n, 1);
    CHECK_STR(tx_log[0], "DL4PW-7>APZK5M,WIDE1-1,WIDE2-1::DL1ABC-9 :Hallo zurueck{1");

    /* no ack: 5 transmissions over 30 + 60 + 120 + 240 s */
    run_ms(30000 + 60000 + 120000 + 240000 + 20000);
    CHECK_INT(tx_n, 5);
    CHECK_INT(MSG_OutboxCount(), 1);
    CHECK(out_flags(slot) & MBX_F_FAILED);
    run_ms(120000);                    /* the last ack window closes */
    CHECK_INT(MSG_OutboxCount(), 0);
    CHECK(!(out_flags(slot) & MBX_F_FAILED));
    CHECK(out_flags(slot) & MBX_F_DELIVERED);

    /* a late ack still counts */
    rx("DL1ABC-9", ":DL4PW-7  :ack1");
    run_ms(100);
    CHECK(!(out_flags(slot) & MBX_F_DELIVERED));
}

static void test_send_acked(void)
{
    reset_all();
    CHECK_INT(MSG_Send("DL1ABC", "eins"), MSG_SEND_OK);
    const int16_t a = mbx_newest();
    CHECK_INT(MSG_Send("DL2XYZ", "zwei"), MSG_SEND_OK);
    const int16_t b = mbx_newest();
    run_ms(10000);
    CHECK_INT(tx_n, 2);
    CHECK_STR(tx_log[0], "DL4PW-7>APZK5M,WIDE1-1,WIDE2-1::DL1ABC   :eins{1");
    CHECK_STR(tx_log[1], "DL4PW-7>APZK5M,WIDE1-1,WIDE2-1::DL2XYZ   :zwei{2");

    /* ack from another SSID of the same station (APRS-IS client) */
    rx("DL1ABC-10", ":DL4PW-7  :ack1");
    /* a reply that carries the ack for message 2 */
    rx("DL2XYZ", ":DL4PW-7  :Danke!{7}2");
    run_ms(5000);
    CHECK(!(out_flags(a) & MBX_F_DELIVERED));
    CHECK(!(out_flags(b) & MBX_F_DELIVERED));
    CHECK_INT(MSG_OutboxCount(), 0);
    CHECK_INT(tx_n, 3);                /* only the ack for the reply */
    CHECK_STR(tx_log[2], "DL4PW-7>APZK5M,WIDE1-1,WIDE2-1::DL2XYZ   :ack7");
    run_ms(600000);
    CHECK_INT(tx_n, 3);                /* no retries */

    /* rejected */
    CHECK_INT(MSG_Send("DL1ABC", "drei"), MSG_SEND_OK);
    const int16_t c = mbx_newest();
    run_ms(5000);
    rx("DL1ABC", ":DL4PW-7  :rej3");
    run_ms(100);
    CHECK(!(out_flags(c) & MBX_F_FAILED));
    CHECK_INT(MSG_OutboxCount(), 0);
}

static void test_channel_and_errors(void)
{
    reset_all();
    clear = false;
    CHECK_INT(MSG_Send("DL1ABC", "warte"), MSG_SEND_OK);
    run_ms(60000);
    CHECK_INT(tx_n, 0);
    clear = true;
    run_ms(4000);
    CHECK_INT(tx_n, 1);

    /* no APRS VFO: the try is not used up */
    reset_all();
    tx_result = MSG_TX_NO_VFO;
    CHECK_INT(MSG_Send("DL1ABC", "kein VFO"), MSG_SEND_OK);
    run_ms(5 * 60000);
    CHECK_INT(MSG_LastTxResult(), MSG_TX_NO_VFO);
    tx_result = MSG_TX_OK;
    run_ms(40000);
    CHECK_INT(tx_n, 1);
    CHECK_INT(MSG_LastTxResult(), MSG_TX_OK);
    run_ms(30000 + 60000 + 120000 + 240000 + 20000);
    CHECK_INT(tx_n, 5);                /* all five tries still there */

    /* refusals */
    reset_all();
    CHECK_INT(MSG_Send("NOT A CALL", "x"), MSG_SEND_BAD_CALL);
    CHECK_INT(MSG_Send("DL1ABC", ""), MSG_SEND_EMPTY);
    for (unsigned i = 0; i < MSG_OUTBOX_MAX; i++)
        CHECK_INT(MSG_Send("DL1ABC", "voll"), MSG_SEND_OK);
    CHECK_INT(MSG_Send("DL1ABC", "zu viel"), MSG_SEND_OUTBOX_FULL);
    msg_config_t c = *MSG_GetConfig();
    c.enabled = 0;
    MSG_SetConfig(&c);
    CHECK_INT(MSG_Send("DL1ABC", "aus"), MSG_SEND_DISABLED);
}

static void test_beacon_and_limits(void)
{
    reset_all();
    msg_config_t c = *MSG_GetConfig();
    c.beacon = MSG_BEACON_30;
    MSG_SetConfig(&c);
    run_ms(15000);
    CHECK_INT(tx_n, 0);
    run_ms(10000);
    CHECK_INT(tx_n, 1);                /* 20 s after switching on */
    CHECK_STR(tx_log[0], "DL4PW-7>APZK5M,WIDE1-1,WIDE2-1:>UV-K5 messenger");
    run_ms(29 * 60000);
    CHECK_INT(tx_n, 1);
    run_ms(2 * 60000);
    CHECK_INT(tx_n, 2);

    /* a flood of messages: the token bucket caps the acks */
    reset_all();
    char call[12], info[64];
    for (int i = 0; i < 20; i++) {
        if (rx_n == 8)
            run_ms(10);
        snprintf(call, sizeof call, "DL%dAA", i % 10);
        snprintf(info, sizeof info, ":DL4PW-7  :flood %d{%d", i, i);
        rx(call, info);
    }
    run_ms(60000);
    /* the ack queue keeps the last 4 (the senders of the others retry and get
     * their ack then); the token bucket allows 6 at once, then one per 30 s */
    CHECK(tx_n <= 8);
    CHECK(tx_n >= 4);

    /* path settings */
    reset_all();
    c = *MSG_GetConfig();
    c.path = MSG_PATH_DIRECT;
    MSG_SetConfig(&c);
    MSG_Send("DL1ABC", "direkt");
    run_ms(4000);
    CHECK_STR(tx_log[0], "DL4PW-7>APZK5M::DL1ABC   :direkt{1");
    CHECK_INT(sim_violations, 0);
}

int main(void)
{
    test_config();
    test_receive_and_ack();
    test_send_retries();
    test_send_acked();
    test_channel_and_errors();
    test_beacon_and_limits();
    return TEST_REPORT("messenger");
}
