/* Copyright 2026 Armel F4HWN, Philipp Weber (DL4PW)
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

/* Derived from App/apps/aprsrx/aprsrx_app.c (Armel F4HWN): the front end
 * (DC removal, band-pass, sliding 1200/2200 Hz correlators, per-tone AGC),
 * the three slicers with their DPLL, NRZI and HDLC, and the UI-frame check are
 * the same integer arithmetic. Changes: block processing for a DMA interrupt,
 * a one-frame hand-off slot instead of the on-screen history, frames limited
 * to AX25_FRAME_MAX bytes, cos tables as const data. */

#include <string.h>

#include "aprs_demod.h"

/* ---- demodulator constants (aprsrx: test/model_rx.py, class Demod) ---- */
#define BP_B0       5356   /* band-pass 1625 Hz, Q 0.9, Q14 (b2 = -b0, b1 = 0) */
#define BP_A1     (-10714)
#define BP_A2       5673
#define AGC_ATT     5      /* peak tracker: attack 1/32 ...                 */
#define AGC_DEC     10     /* ... decay 1/1024 per sample                   */
#define PK_MIN      16
#define PLL_STEP    8192   /* 65536 per bit / 8 samples per bit             */
#define PLL_CTR     36864  /* transitions pulled to mid-bit + half a sample */
#define PLL_SHIFT   2      /* 1/4 of the error per transition               */
#define NSL         3      /* slicers                                       */
#define PRE_BITS    24u    /* busy for 3 bytes after two back-to-back flags */
#define DUP_SAMPLES APRS_DEMOD_FS   /* same frame from another slicer: 1 s  */

/* 127 * cos(2 pi f n / 9600): one period of 1200 Hz, 11 periods of 2200 Hz */
static const int8_t COS1200[8] = {127, 90, 0, -90, -127, -90, 0, 90};
static const int8_t COS2200[48] = {
    127, 17, -123, -49, 110, 77, -90, -101, 64, 117, -33, -126, 0, 126, 33, -117,
    -63, 101, 90, -77, -110, 49, 123, -17, -127, -17, 123, 49, -110, -77, 90, 101,
    -64, -117, 33, 126, 0, -126, -33, 117, 63, -101, -90, 77, 110, -49, -123, 17,
};

typedef struct {
    int32_t  dc;                       /* DC tracker, Q4                      */
    int32_t  x1, x2, y1, y2;           /* band-pass state                     */
    int32_t  im, qm, is, qs;           /* sliding correlator sums             */
    int32_t  pm, ps;                   /* per-tone magnitude peaks (AGC)      */
    uint32_t r, ks;                    /* 1200 Hz phase = ring slot, 2200 Hz phase */
    int16_t  ring[8][4];               /* the products of the last bit        */
} dem_t;

typedef struct {
    int32_t  dprev, phase;             /* last decision, DPLL phase (65536/bit) */
    uint16_t n, crc;                   /* bytes, running FCS                  */
    uint8_t  wa, wb, last;             /* mark/space weights, last level      */
    uint8_t  sr, ones, byte, nb;       /* HDLC: shift reg, 1 run, byte, bit count */
    uint8_t  inframe, hdr;             /* between flags; first bytes look like a call */
    uint8_t  bits;                     /* bits since the last flag            */
    bool     pre;                      /* this flag came right after another one */
    uint8_t  buf[AX25_FRAME_MAX];
} sl_t;

static dem_t    gDem;
static sl_t     gSl[NSL];
static uint32_t gSampleCount;

/* the hand-off slot: written by the interrupt while empty, emptied by the main loop */
static uint8_t           gSlot[AX25_FRAME_MAX];
static volatile uint16_t gSlotLen;
static uint16_t          gDropped;
static uint16_t          gLastLen, gLastFcs;
static uint32_t          gLastAt;

void aprs_demod_reset(void)
{
    memset(&gDem, 0, sizeof(gDem));
    gDem.dc = 2048 << 4;
    gDem.pm = 64;
    gDem.ps = 64;
    memset(gSl, 0, sizeof(gSl));
    gSl[0].wa = 2; gSl[0].wb = 3;      /* favours space */
    gSl[1].wa = 1; gSl[1].wb = 1;
    gSl[2].wa = 3; gSl[2].wb = 2;      /* favours mark  */
    gSampleCount = 0;
    gLastLen = 0;
    gSlotLen = 0;
}

static bool call_char(uint8_t c)
{
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ';
}

/* An APRS frame is a UI frame: the address field ends (bit 0 of an SSID byte)
 * and is followed by control 0x03 and PID 0xF0. Rejects the noise burst that
 * passes the 16-bit FCS about once a day in continuous decoding. */
static bool is_ui(const uint8_t *f, uint16_t n)
{
    uint16_t end = (uint16_t)(n - 2u), e = 13;
    while (!(f[e] & 1u) && e + 7u < end)
        e += 7;
    return (f[e] & 1u) && e + 2u < end && f[e + 1] == 0x03u && f[e + 2] == 0xF0u;
}

static void accept(const sl_t *m)
{
    const uint16_t n = m->n;
    const uint16_t fcs = (uint16_t)(m->buf[n - 2u] | (m->buf[n - 1u] << 8));
    if (n == gLastLen && fcs == gLastFcs && gSampleCount - gLastAt < DUP_SAMPLES) {
        gLastAt = gSampleCount;        /* the same frame from another slicer */
        return;
    }
    gLastLen = n;
    gLastFcs = fcs;
    gLastAt  = gSampleCount;
    if (gSlotLen) {
        gDropped++;
        return;
    }
    memcpy(gSlot, m->buf, n);
    gSlotLen = n;
}

/* HDLC: one bit after NRZI. A UI frame with a good FCS goes to accept()
 * before the closing flag opens the next frame. */
static void hdlc_bit(sl_t *m, uint8_t b)
{
    if (m->bits < 255u)
        m->bits++;
    m->sr = (uint8_t)((m->sr >> 1) | (b << 7));
    if (m->sr == 0x7Eu) {
        const bool ok = m->inframe && m->n >= AX25_FRAME_MIN && m->crc == AX25_FCS_GOOD
                     && is_ui(m->buf, m->n);
        m->pre  = m->bits == 8u;
        m->bits = 0;
        if (ok)
            accept(m);
        m->inframe = 1; m->hdr = 1; m->n = 0; m->crc = 0xFFFFu;
        m->ones = m->byte = m->nb = 0;
        return;
    }
    if (b) {
        if (++m->ones > 6u) {          /* abort / noise */
            m->ones = 7;
            m->inframe = 0;
            return;
        }
    } else {
        if (m->ones == 5u) {           /* stuffed zero */
            m->ones = 0;
            return;
        }
        m->ones = 0;
    }
    if (!m->inframe)
        return;
    m->byte |= (uint8_t)(b << m->nb);
    if (++m->nb == 8u) {
        if (m->n < AX25_FRAME_MAX) {
            uint16_t c = m->crc ^ m->byte;   /* CRC-16/X.25, reflected 0x1021 */
            for (uint8_t i = 0; i < 8u; i++)
                c = (c & 1u) ? (uint16_t)((c >> 1) ^ 0x8408u) : (uint16_t)(c >> 1);
            m->crc = c;
            if (m->n < 6u && ((m->byte & 1u) || !call_char((uint8_t)(m->byte >> 1))))
                m->hdr = 0;
            m->buf[m->n++] = m->byte;
        } else {
            m->inframe = 0;            /* longer than anything we keep */
        }
        m->byte = m->nb = 0;
    }
}

static bool slicer_busy(const sl_t *m)
{
    return (m->pre && m->bits < PRE_BITS) || (m->inframe && m->n && m->hdr);
}

bool aprs_demod_busy(void)
{
    for (uint8_t k = 0; k < NSL; k++)
        if (slicer_busy(&gSl[k]))
            return true;
    return false;
}

/* |(i, q)| ~ max + 3/8 min */
static int32_t mag(int32_t i, int32_t q)
{
    if (i < 0) i = -i;
    if (q < 0) q = -q;
    if (i < q) { int32_t t = i; i = q; q = t; }
    return i + (q >> 2) + (q >> 3);
}

/* Magnitude of one tone and its peak tracker (AGC). */
static int32_t tone(int32_t i, int32_t q, int32_t *pk)
{
    int32_t mt = mag(i, q), p = *pk;
    p += mt > p ? (mt - p) >> AGC_ATT : -(p >> AGC_DEC);
    if (p < PK_MIN)
        p = PK_MIN;
    *pk = p;
    return mt;
}

/* Front end, one ADC sample: *a = Mm*Ps, *b = Ms*Pm (each tone over its own
 * peak, without division; both stay below ~2^21). */
static void front(dem_t *m, int32_t adc, int32_t *a, int32_t *b)
{
    m->dc += ((adc << 4) - m->dc) >> 6;
    int32_t x = adc - (m->dc >> 4);
    int32_t y = (BP_B0 * (x - m->x2) - BP_A1 * m->y1 - BP_A2 * m->y2) >> 14;
    m->x2 = m->x1; m->x1 = x; m->y2 = m->y1; m->y1 = y;

    /* sliding correlators over one bit; the sine is the cos table 3/4 period
     * ahead. At 9.6 kHz one bit and one 1200 Hz period are both 8 samples: the
     * ring slot is also the 1200 Hz table phase. */
    int16_t *o = m->ring[m->r];
    uint32_t k = m->ks + 12u;
    if (k >= 48u)
        k -= 48u;
    int32_t p;
    p = (y * COS1200[m->r]) >> 8;               m->im += p - o[0]; o[0] = (int16_t)p;
    p = (y * COS1200[(m->r + 6u) & 7u]) >> 8;   m->qm += p - o[1]; o[1] = (int16_t)p;
    p = (y * COS2200[m->ks]) >> 8;              m->is += p - o[2]; o[2] = (int16_t)p;
    p = (y * COS2200[k]) >> 8;                  m->qs += p - o[3]; o[3] = (int16_t)p;
    m->r = (m->r + 1u) & 7u;
    if (++m->ks >= 48u)
        m->ks = 0;

    const int32_t mm = tone(m->im, m->qm, &m->pm);
    const int32_t ms = tone(m->is, m->qs, &m->ps);
    *a = mm * m->ps;
    *b = ms * m->pm;
}

/* Slicer: decision sign(wa*a - wb*b), DPLL, NRZI, HDLC. */
static void slice(sl_t *m, int32_t a, int32_t b)
{
    const int32_t d = a * m->wa - b * m->wb;
    /* DPLL: a bit at each phase wrap (mid-bit), transitions pulled to mid-phase */
    m->phase += PLL_STEP;
    if ((d > 0) != (m->dprev > 0))
        m->phase += (PLL_CTR - m->phase) >> PLL_SHIFT;
    m->dprev = d;
    if (m->phase >= 65536) {
        m->phase -= 65536;
        const uint8_t lv = d > 0;
        hdlc_bit(m, lv == m->last);    /* NRZI: no change = 1 */
        m->last = lv;
    }
}

void aprs_demod_process(const uint16_t *samples, uint16_t n)
{
    while (n--) {
        int32_t a, b;
        front(&gDem, (int32_t)(*samples++ & 0x0FFFu), &a, &b);
        for (uint8_t k = 0; k < NSL; k++)
            slice(&gSl[k], a, b);
        gSampleCount++;
    }
}

bool aprs_demod_take(uint8_t *out, uint16_t *len)
{
    const uint16_t n = gSlotLen;
    if (n == 0)
        return false;
    memcpy(out, gSlot, n);
    *len = n;
    gSlotLen = 0;                      /* the interrupt may fill it again */
    return true;
}

uint16_t aprs_demod_dropped(void)
{
    return gDropped;
}
