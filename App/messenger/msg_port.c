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

/* The messenger on the radio: msg_port.h and msg_flash.h on top of the
 * F4HWN firmware, and the hooks of msg_fw.h. */

#include <string.h>

#include "app/chFrScanner.h"
#include "app/scanner.h"
#ifdef ENABLE_FMRADIO_EMBEDDED
    #include "app/fm.h"
    #include "driver/bk1080.h"
#endif
#include "audio.h"
#include "driver/backlight.h"
#include "driver/bk4819.h"
#include "driver/bk4819-regs.h"
#include "driver/py25q16.h"
#include "driver/system.h"
#include "functions.h"
#include "misc.h"
#include "radio.h"
#include "settings.h"
#include "ui/ui.h"

#include "aprs_demod.h"
#include "mailbox.h"
#include "messenger.h"
#include "msg_flash.h"
#include "msg_fw.h"
#include "msg_hw.h"
#include "msg_port.h"
#include "msg_ui.h"

#define BUSY_HOLD_MS     1500u     /* the channel counts as busy this long after activity */
#define AF_KEEP_TICKS    10u       /* re-assert the BK4829 AF output every 100 ms */

static volatile uint32_t gTick10ms;
static uint32_t gBusyUntil;
static uint32_t gListenUntil, gListenAfter;
static uint32_t gRand = 0x6D2B79F5u;
static bool     gReady;
static bool     gNotify;
static msg_notify_t gNotifyKind;
static uint8_t  gAfTick;

/* ------------------------------------------------------------ flash -- */

void msg_flash_read(uint32_t addr, void *buf, uint32_t len)
{
    PY25Q16_ReadBufferSafe(addr, buf, len);
}

void msg_flash_program(uint32_t addr, const void *buf, uint32_t len)
{
    /* only erased bytes or cleared bits: the driver programs without an erase */
    PY25Q16_WriteBuffer(addr, buf, len, false);
}

void msg_flash_erase(uint32_t sector_addr)
{
    PY25Q16_SectorErase(sector_addr);
}

/* ---------------------------------------------------------- helpers -- */

/* The VFO tuned to the APRS frequency, the receiving one first. */
static VFO_Info_t *aprs_vfo(void)
{
    const uint32_t f = MSG_GetConfig()->freq;
    if (gRxVfo && gRxVfo->pRX->Frequency == f)
        return gRxVfo;
    for (uint8_t i = 0; i < 2u; i++)
        if (gEeprom.VfoInfo[i].pRX->Frequency == f)
            return &gEeprom.VfoInfo[i];
    return NULL;
}

static bool rx_on_aprs(void)
{
    return gRxVfo && gRxVfo->pRX->Frequency == MSG_GetConfig()->freq;
}

static bool scanning(void)
{
    if (gScanStateDir != SCAN_OFF || gCssBackgroundScan || SCANNER_IsScanning())
        return true;
#ifdef ENABLE_FMRADIO_EMBEDDED
    if (gFmRadioMode && gFM_ScanState != FM_SCAN_OFF)
        return true;
#endif
    return false;
}

/* The notification: two rising tones, the way AUDIO_PlayBeep plays its beeps
 * (FM radio muted around them, speaker restored). */
static void play_notify(void)
{
    if (gCurrentFunction == FUNCTION_RECEIVE || gCurrentFunction == FUNCTION_MONITOR ||
        gCurrentFunction == FUNCTION_TRANSMIT || gCurrentFunction == FUNCTION_INCOMING)
        return;
#ifdef ENABLE_FMRADIO_EMBEDDED
    if (gFmRadioMode)
        BK1080_Mute(true);
#endif
    AUDIO_AudioPathOff();
    SYSTEM_DelayMs(20);
    const uint16_t toneConfig = BK4819_ReadRegister(BK4819_REG_71);
    BK4819_PrepareToPlayTone(true);
    SYSTEM_DelayMs(2);
    AUDIO_AudioPathOn();
    SYSTEM_DelayMs(60);
    BK4819_PlayToneRaw(1200, 70);
    SYSTEM_DelayMs(30);
    BK4819_PlayToneRaw(1800, 110);
    AUDIO_AudioPathOff();
    SYSTEM_DelayMs(5);
    BK4819_TurnsOffTones_TurnsOnRX();
    SYSTEM_DelayMs(5);
    BK4819_WriteRegister(BK4819_REG_71, toneConfig);
#ifdef ENABLE_FMRADIO_EMBEDDED
    if (gFmRadioMode)
        SYSTEM_DelayMs(10);
#endif
    if (gEnableSpeaker)
        AUDIO_AudioPathOn();
#ifdef ENABLE_FMRADIO_EMBEDDED
    if (gFmRadioMode)
        BK1080_Mute(false);
#endif
    gAfTick = AF_KEEP_TICKS;           /* the AF output back for the decoder */
}

/* "DL4PW" from the boot message, when it looks like a callsign */
static void default_callsign(void)
{
    msg_config_t c = *MSG_GetConfig();
    if (c.call[0])
        return;
    char raw[16];
    PY25Q16_ReadBuffer(SETTINGS_BOOT_MESSAGE_LINE1_ADDR, raw, sizeof raw);
    char call[AX25_CALL_LEN + 1];
    uint8_t n = 0;
    bool digit = false;
    for (uint8_t i = 0; i < sizeof raw && n <= AX25_CALL_LEN; i++) {
        char ch = raw[i];
        if (ch >= 'a' && ch <= 'z')
            ch = (char)(ch - 32);
        if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')) {
            digit |= ch >= '0' && ch <= '9';
            call[n++] = ch;
        } else {
            break;
        }
    }
    if (n < 3u || n > AX25_CALL_LEN || !digit)
        return;
    call[n] = '\0';
    strcpy(c.call, call);
    MSG_SetConfig(&c);
}

/* ------------------------------------------------------------- port -- */

uint32_t msg_port_ms(void)
{
    return gTick10ms * 10u;
}

uint32_t msg_port_random(void)
{
    uint32_t x = gRand ^ SysTick->VAL ^ (gTick10ms << 16);
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    gRand = x;
    return x;
}

bool msg_port_take_frame(uint8_t *buf, uint16_t *len)
{
    return aprs_demod_take(buf, len);
}

/* With squelch 0 the squelch is always open: then only the decoder tells
 * whether the channel is busy. */
static bool squelch_busy(void)
{
    return g_SquelchLost && gEeprom.SQUELCH_LEVEL != 0;
}

bool msg_port_channel_clear(void)
{
    return rx_on_aprs()
        && (int32_t)(msg_port_ms() - gBusyUntil) >= 0
        && gCurrentFunction == FUNCTION_FOREGROUND
        && !gPttIsPressed
        && !squelch_busy()
        && !scanning();
}

msg_tx_result_t msg_port_transmit(const uint8_t *frame, uint16_t len)
{
    VFO_Info_t *v = aprs_vfo();
    if (!v)
        return MSG_TX_NO_VFO;
    const msg_tx_result_t r = msg_hw_transmit(v, frame, len);
    gUpdateStatus = true;
    gUpdateDisplay = true;
    return r;
}

void msg_port_notify(msg_notify_t kind)
{
    if (!gNotify || kind < gNotifyKind)
        gNotifyKind = kind;            /* private beats channel beats public */
    gNotify = true;
}

/* -------------------------------------------------------------- hooks -- */

void MSG_Init(void)
{
    MSG_CoreInit();
    default_callsign();
    gRand ^= SysTick->VAL ^ ((uint32_t)gBatteryVoltageAverage << 20);
    gReady = true;
}

void MSG_SysTick10ms(void)
{
    gTick10ms++;
}

void MSG_TimeSlice10ms(void)
{
    if (!gReady)
        return;
    const msg_config_t *cfg = MSG_GetConfig();
    const uint32_t now = msg_port_ms();

    /* Reserve a continuous APRS listening window before an automatic TX.
     * The normal dual-watch dwell is shorter than the CSMA holdoff. */
    if (cfg->enabled && rx_on_aprs() && MSG_TxPending() && !scanning()
        && gCurrentFunction == FUNCTION_FOREGROUND && !gPttIsPressed) {
        if ((int32_t)(now - gListenAfter) >= 0) {
            gListenUntil = now + 6000u;
            gListenAfter = now + 8000u;
        }
        if ((int32_t)(now - gListenUntil) < 0) {
            gDualWatchCountdown_10ms = 10;
            gScheduleDualWatch = false;
        }
    }
    /* Decode only the currently tuned APRS channel. */
    const bool want = cfg->enabled && rx_on_aprs() && !scanning() && gCurrentFunction != FUNCTION_TRANSMIT;
    if (want && !msg_hw_capture_running()) {
        gBusyUntil = now + BUSY_HOLD_MS;
        msg_hw_capture_start();
    }
    else if (!want && msg_hw_capture_running())
        msg_hw_capture_stop();

    if (msg_hw_capture_running() && rx_on_aprs()) {
        if (squelch_busy() || aprs_demod_busy())
            gBusyUntil = now + BUSY_HOLD_MS;

        /* A key beep leaves the BK4829 AF output muted until the next
         * reception; the decoder needs it. With the FM radio playing, only
         * while the squelch is open, or the receiver noise would be heard. */
        if (++gAfTick >= AF_KEEP_TICKS) {
            gAfTick = 0;
            bool af = gCurrentFunction == FUNCTION_FOREGROUND && gRxVfo->Modulation == MODULATION_FM;
#ifdef ENABLE_FMRADIO_EMBEDDED
            if (gFmRadioMode && !g_SquelchLost)
                af = false;
#endif
            if (af)
                BK4819_SetAF(BK4819_AF_FM);
#ifdef ENABLE_FMRADIO_EMBEDDED
            else if (gFmRadioMode && gCurrentFunction == FUNCTION_FOREGROUND)
                BK4819_SetAF(BK4819_AF_MUTE);
#endif
        }
    }

    MSG_Task();

    if (gNotify) {
        gNotify = false;
        gUpdateStatus = true;
        if (gScreenToDisplay == DISPLAY_MESSENGER)
            gUpdateDisplay = true;
        /* public bulletins only show the envelope */
        if (gNotifyKind != MSG_NOTIFY_PUBLIC)
            BACKLIGHT_TurnOn();
        const bool beep = cfg->beep == MSG_BEEP_ALL     ? gNotifyKind != MSG_NOTIFY_PUBLIC
                        : cfg->beep == MSG_BEEP_PRIVATE ? gNotifyKind == MSG_NOTIFY_PRIVATE
                        : false;
        if (beep)
            play_notify();
    }

    msg_ui_tick();
}

bool MSG_IsSilentRx(void)
{
    const msg_config_t *cfg = MSG_GetConfig();
    return gReady && cfg->enabled && !cfg->audio && !gMonitor && !scanning() && rx_on_aprs();
}

bool MSG_BlocksPowerSave(void)
{
    return gReady && MSG_GetConfig()->enabled && aprs_vfo() != NULL;
}

bool MSG_HasUnread(void)
{
    return gReady && mbx_unread() > 0;
}

bool MSG_CaptureYieldAdc(void)
{
    return msg_hw_yield_adc();
}

void MSG_CaptureResumeAdc(void)
{
    msg_hw_resume_adc();
}

/* for the setup screen */
bool msg_port_has_aprs_vfo(void)
{
    return aprs_vfo() != NULL;
}
