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

/* The PA4 audio path and the AFSK keying follow the overlay services of
 * App/apps/app_overlay.c (af_capture_*, tx_*) and the APRS TX app by Armel
 * F4HWN; the capture is timer-triggered with DMA here instead of polled. */

#include <string.h>

#include "py32f071_ll_adc.h"
#include "py32f071_ll_bus.h"
#include "py32f071_ll_dma.h"
#include "py32f071_ll_system.h"
#include "py32f071_ll_tim.h"

#include "audio.h"
#include "driver/bk4819.h"
#include "driver/bk4819-regs.h"
#include "driver/systick.h"
#include "frequencies.h"
#include "functions.h"
#include "helper/battery.h"
#include "misc.h"
#include "radio.h"
#ifdef ENABLE_FMRADIO_EMBEDDED
    #include "app/fm.h"
#endif

#include "aprs_afsk.h"
#include "aprs_demod.h"
#include "msg_hw.h"

/* ---- capture ---- */
#define CAP_N          256u                /* two halves of 128 samples, 13.3 ms each */
#define CAP_HALF       (CAP_N / 2u)
#define TIM_FS_ARR     (48000000u / APRS_DEMOD_FS - 1u)   /* 9.6 kHz */
#define TIM_BAUD_ARR   (48000000u / AFSK_BAUD - 1u)       /* 1200 Hz */
#define ADC_CH_PA4     4u
#define ADC_SMP4_POS   12u
#define ADC_SMP8_POS   24u
#define DAC_BIAS_CR    ((1u << 0) | (1u << 1) | (1u << 2) | (7u << 3))  /* EN1 BOFF1 TEN1 TSEL1=SW */
#define DAC_BIAS_CODE  2048u                /* PA4 held at mid-scale (EPIRB 406 lab) */
#define LEVEL_BLOCKS   8u                   /* level window: 8 blocks = 107 ms */

/* ---- transmitter ---- */
#define TONE_LEVEL     66u                  /* BK4819_TransmitTone's tone gain (APRS TX) */
#define F_MARK         12389u               /* REG_71 for 1200 Hz (scale_freq) */
#define F_SPACE        22714u               /* REG_71 for 2200 Hz */
#define BIT_WAIT_MAX   200000u              /* polls of one bit period before giving up */

static uint16_t gBuf[CAP_N] __attribute__((aligned(4)));
static volatile bool gRunning;

static struct {
    uint32_t moder, apbenr1, dac_cr, dac_dhr, sqr3, smpr3, cr2;
} gSave;

static volatile uint32_t gBlocks;
static volatile uint16_t gOverruns;
static volatile uint16_t gLoadPm, gLoadMaxPm;
static volatile uint16_t gLevelPp;
static uint16_t gLvMin = 0xFFFFu, gLvMax;
static uint8_t  gLvBlocks;

static void adc_select_audio(void)
{
    ADC1->SQR3 = (gSave.sqr3 & ~0x1Fu) | ADC_CH_PA4;
}

void msg_hw_capture_start(void)
{
    if (gRunning)
        return;
    aprs_demod_reset();

    gSave.moder   = GPIOA->MODER;
    gSave.apbenr1 = RCC->APBENR1;
    gSave.dac_cr  = DAC1->CR;
    gSave.dac_dhr = DAC1->DHR12R1;
    gSave.sqr3    = ADC1->SQR3;
    gSave.smpr3   = ADC1->SMPR3;
    gSave.cr2     = ADC1->CR2;

    /* PA4 analog, held at mid-scale by the DAC (output buffer off) */
    GPIOA->MODER  = gSave.moder | (3u << 8);
    RCC->APBENR1  = gSave.apbenr1 | RCC_APBENR1_DACEN;
    DAC1->CR      = DAC_BIAS_CR;
    DAC1->DHR12R1 = DAC_BIAS_CODE;
    DAC1->SWTRIGR = 1u;

    /* ADC channel 4 with the battery channel's sampling time */
    ADC1->SMPR3 = (gSave.smpr3 & ~(7u << ADC_SMP4_POS)) |
                  (((gSave.smpr3 >> ADC_SMP8_POS) & 7u) << ADC_SMP4_POS);
    adc_select_audio();

    /* DMA1 channel 1: ADC1->DR -> gBuf, circular, an interrupt per half */
    LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_DMA1);
    LL_APB1_GRP2_EnableClock(LL_APB1_GRP2_PERIPH_SYSCFG);
    LL_DMA_DisableChannel(DMA1, LL_DMA_CHANNEL_1);
    LL_SYSCFG_SetDMARemap(DMA1, LL_DMA_CHANNEL_1, LL_SYSCFG_DMA_MAP_ADC1);
    LL_DMA_ConfigTransfer(DMA1, LL_DMA_CHANNEL_1,
                          LL_DMA_DIRECTION_PERIPH_TO_MEMORY | LL_DMA_MODE_CIRCULAR |
                          LL_DMA_PERIPH_NOINCREMENT | LL_DMA_MEMORY_INCREMENT |
                          LL_DMA_PDATAALIGN_HALFWORD | LL_DMA_MDATAALIGN_HALFWORD |
                          LL_DMA_PRIORITY_LOW);
    LL_DMA_ConfigAddresses(DMA1, LL_DMA_CHANNEL_1, (uint32_t)&ADC1->DR, (uint32_t)gBuf,
                           LL_DMA_DIRECTION_PERIPH_TO_MEMORY);
    LL_DMA_SetDataLength(DMA1, LL_DMA_CHANNEL_1, CAP_N);
    LL_DMA_ClearFlag_GI1(DMA1);
    LL_DMA_EnableIT_HT(DMA1, LL_DMA_CHANNEL_1);
    LL_DMA_EnableIT_TC(DMA1, LL_DMA_CHANNEL_1);
    NVIC_SetPriority(DMA1_Channel1_IRQn, 3);     /* below SysTick, flash DMA and USB */
    NVIC_EnableIRQ(DMA1_Channel1_IRQn);
    LL_DMA_EnableChannel(DMA1, LL_DMA_CHANNEL_1);

    /* the ADC converts on TIM3 TRGO and hands every result to the DMA */
    (void)ADC1->DR;
    MODIFY_REG(ADC1->CR2, ADC_CR2_EXTSEL, LL_ADC_REG_TRIG_EXT_TIM3_TRGO);
    SET_BIT(ADC1->CR2, ADC_CR2_DMA | ADC_CR2_EXTTRIG);

    /* TIM3 at 9.6 kHz */
    LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_TIM3);
    LL_TIM_DisableCounter(TIM3);
    LL_TIM_SetPrescaler(TIM3, 0);
    LL_TIM_SetAutoReload(TIM3, TIM_FS_ARR);
    LL_TIM_SetTriggerOutput(TIM3, LL_TIM_TRGO_UPDATE);
    LL_TIM_SetCounter(TIM3, 0);
    gLvMin = 0xFFFFu;
    gLvMax = 0;
    gLvBlocks = 0;
    gRunning = true;
    LL_TIM_EnableCounter(TIM3);
}

void msg_hw_capture_stop(void)
{
    if (!gRunning)
        return;
    gRunning = false;
    LL_TIM_DisableCounter(TIM3);
    SYSTICK_DelayUs(10);                          /* a conversion in flight ends */
    NVIC_DisableIRQ(DMA1_Channel1_IRQn);
    LL_DMA_DisableChannel(DMA1, LL_DMA_CHANNEL_1);
    LL_DMA_ClearFlag_GI1(DMA1);

    MODIFY_REG(ADC1->CR2, ADC_CR2_EXTSEL | ADC_CR2_DMA | ADC_CR2_EXTTRIG,
               gSave.cr2 & (ADC_CR2_EXTSEL | ADC_CR2_DMA | ADC_CR2_EXTTRIG));
    ADC1->SQR3  = gSave.sqr3;
    ADC1->SMPR3 = gSave.smpr3;
    (void)ADC1->DR;

    DAC1->CR      = gSave.dac_cr;
    DAC1->DHR12R1 = gSave.dac_dhr;
    RCC->APBENR1  = (RCC->APBENR1 & ~RCC_APBENR1_DACEN) | (gSave.apbenr1 & RCC_APBENR1_DACEN);
    GPIOA->MODER  = (GPIOA->MODER & ~(3u << 8)) | (gSave.moder & (3u << 8));
    gLevelPp = 0;
}

bool msg_hw_capture_running(void)
{
    return gRunning;
}

bool msg_hw_yield_adc(void)
{
    if (!gRunning)
        return false;
    LL_TIM_DisableCounter(TIM3);
    SYSTICK_DelayUs(6);                           /* 4.5 us per conversion */
    CLEAR_BIT(ADC1->CR2, ADC_CR2_DMA);
    MODIFY_REG(ADC1->CR2, ADC_CR2_EXTSEL, LL_ADC_REG_TRIG_SOFTWARE);
    ADC1->SQR3 = gSave.sqr3;                      /* the battery channel */
    (void)ADC1->DR;                               /* no stale end of conversion */
    return true;
}

void msg_hw_resume_adc(void)
{
    if (!gRunning)
        return;
    adc_select_audio();
    MODIFY_REG(ADC1->CR2, ADC_CR2_EXTSEL, LL_ADC_REG_TRIG_EXT_TIM3_TRGO);
    (void)ADC1->DR;
    SET_BIT(ADC1->CR2, ADC_CR2_DMA | ADC_CR2_EXTTRIG);
    LL_TIM_EnableCounter(TIM3);
}

static void level(const uint16_t *s, uint16_t n)
{
    for (uint16_t i = 0; i < n; i++) {
        const uint16_t v = s[i] & 0x0FFFu;
        if (v < gLvMin) gLvMin = v;
        if (v > gLvMax) gLvMax = v;
    }
    if (++gLvBlocks >= LEVEL_BLOCKS) {
        gLevelPp  = (uint16_t)(gLvMax - gLvMin);
        gLvMin    = 0xFFFFu;
        gLvMax    = 0;
        gLvBlocks = 0;
    }
}

static void block(const uint16_t *s, bool firstHalf)
{
    aprs_demod_process(s, CAP_HALF);
    level(s, CAP_HALF);
    gBlocks++;
    /* the DMA must be in the other half by now, or it overwrote this one */
    const uint32_t pos = CAP_N - LL_DMA_GetDataLength(DMA1, LL_DMA_CHANNEL_1);
    if ((pos < CAP_HALF) == firstHalf)
        gOverruns++;
}

void DMA1_Channel1_IRQHandler(void)
{
    const uint32_t t0 = SysTick->VAL;
    bool did = false;
    if (LL_DMA_IsActiveFlag_HT1(DMA1)) {
        LL_DMA_ClearFlag_HT1(DMA1);
        if (gRunning)
            block(gBuf, true);
        did = true;
    }
    if (LL_DMA_IsActiveFlag_TC1(DMA1)) {
        LL_DMA_ClearFlag_TC1(DMA1);
        if (gRunning)
            block(gBuf + CAP_HALF, false);
        did = true;
    }
    if (!did) {
        LL_DMA_ClearFlag_GI1(DMA1);
        return;
    }
    const uint32_t t1 = SysTick->VAL;
    const uint32_t cyc = t0 >= t1 ? t0 - t1 : t0 + (SysTick->LOAD + 1u - t1);
    const uint32_t pm = cyc / (CAP_HALF * (48000000u / APRS_DEMOD_FS) / 1000u);
    gLoadPm = (uint16_t)(gLoadPm + (((int32_t)pm - (int32_t)gLoadPm) >> 4));
    if (pm > gLoadMaxPm)
        gLoadMaxPm = (uint16_t)(pm > 0xFFFFu ? 0xFFFFu : pm);
}

void msg_hw_diag(msg_hw_diag_t *d)
{
    d->load_pm     = gLoadPm;
    d->load_max_pm = gLoadMaxPm;
    d->level_pp    = gRunning ? gLevelPp : 0;
    d->overruns    = gOverruns;
    d->blocks      = gBlocks;
}

/* ---- transmitter ---- */

static void set_tone(bool space)
{
    BK4819_WriteRegister(BK4819_REG_71, space ? F_SPACE : F_MARK);
    BK4819_WriteRegister(BK4819_REG_70, (uint16_t)(BK4819_REG_70_ENABLE_TONE1 |
                                                    (TONE_LEVEL << BK4819_REG_70_SHIFT_TONE1_TUNING_GAIN)));
}

static bool wait_bit(void)
{
    for (uint32_t n = 0; n < BIT_WAIT_MAX; n++) {
        if (LL_TIM_IsActiveFlag_UPDATE(TIM3)) {
            LL_TIM_ClearFlag_UPDATE(TIM3);
            return true;
        }
    }
    return false;                                 /* the timer does not run: stop */
}

msg_tx_result_t msg_hw_transmit(struct VFO_Info_t *vfo, const uint8_t *frame, uint16_t len)
{
    if (vfo->Modulation != MODULATION_FM)
        return MSG_TX_LOCKED;
    if (TX_freq_check(vfo->pTX->Frequency) != 0 && vfo->TX_LOCK)
        return MSG_TX_LOCKED;
    if (gBatteryDisplayLevel == 0 || gBatteryDisplayLevel > 6)
        return MSG_TX_BATTERY;
    if (SerialConfigInProgress() || gPttIsPressed || gCurrentFunction == FUNCTION_TRANSMIT)
        return MSG_TX_BUSY;

    const bool capture = gRunning;
    msg_hw_capture_stop();

    /* RADIO_SetTxParameters takes the frequency, bandwidth and power from
     * gCurrentVfo and the compander from gRxVfo: both on the APRS VFO */
    VFO_Info_t *const savedCurrent = gCurrentVfo;
    VFO_Info_t *const savedRx = gRxVfo;
    gCurrentVfo = vfo;
    gRxVfo = vfo;
    RADIO_SetTxParameters();                      /* carrier and PA on the vfo's TX frequency */
    gRxVfo = savedRx;
    BK4819_SetCompander(0);                       /* no compander on AFSK */
    BK4819_ToggleGpioOut(BK4819_GPIO5_PIN1_RED, true);
    BK4819_WriteRegister(BK4819_REG_51, 0);       /* no CTCSS/DCS under the AFSK */
    BK4819_TransmitTone(false, AFSK_MARK_HZ);     /* tone path on, mic off, 50 ms settle */

    LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_TIM3);
    LL_TIM_DisableCounter(TIM3);
    LL_TIM_SetPrescaler(TIM3, 0);
    LL_TIM_SetAutoReload(TIM3, TIM_BAUD_ARR);
    LL_TIM_SetCounter(TIM3, 0);
    LL_TIM_ClearFlag_UPDATE(TIM3);
    LL_TIM_EnableCounter(TIM3);

    bool space = false;
    set_tone(space);
    afsk_bits_t g;
    afsk_bits_init(&g, frame, len, AFSK_PREAMBLE, AFSK_TAIL);
    bool ok = true;
    int8_t b;
    while (ok && (b = afsk_bits_next(&g)) >= 0) {
        ok = wait_bit();                          /* the bit starts at the next edge */
        if (b == 0) {                             /* NRZI: a 0 changes the tone */
            space = !space;
            set_tone(space);
        }
    }
    if (ok)
        wait_bit();                               /* the last bit plays out */
    LL_TIM_DisableCounter(TIM3);

    BK4819_EnterTxMute();
    BK4819_ToggleGpioOut(BK4819_GPIO1_PIN29_PA_ENABLE, false);
    BK4819_ToggleGpioOut(BK4819_GPIO5_PIN1_RED, false);
    gCurrentVfo = savedCurrent;
    RADIO_SetupRegisters(true);                   /* RX as before */

#ifdef ENABLE_FMRADIO_EMBEDDED
    if (gFmRadioMode) {                           /* the FM radio plays on */
        AUDIO_AudioPathOn();
        gEnableSpeaker = true;
    }
#endif
    if (capture)
        msg_hw_capture_start();
    return ok ? MSG_TX_OK : MSG_TX_BUSY;
}
