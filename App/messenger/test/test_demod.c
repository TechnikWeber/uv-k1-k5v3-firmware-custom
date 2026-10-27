/* Host test: demodulator on simulated audio (gen_audio.py), and the
 * transmitter's bit stream looped back through the demodulator. */
#define _USE_MATH_DEFINES
#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <stdint.h>
#include <stdlib.h>

#include "test_util.h"
#include "../aprs_afsk.h"
#include "../aprs_ax25.h"
#include "../aprs_demod.h"

#define BLOCK 128u               /* samples per DMA half-buffer on the radio */

typedef struct {
    uint8_t  buf[8][AX25_FRAME_MAX];
    uint16_t len[8];
    uint8_t  n;
} got_t;

static void run(const uint16_t *s, size_t n, got_t *got)
{
    got->n = 0;
    aprs_demod_reset();
    for (size_t i = 0; i < n; i += BLOCK) {
        const uint16_t k = (uint16_t)((n - i) < BLOCK ? (n - i) : BLOCK);
        aprs_demod_process(s + i, k);
        if (got->n < 8 && aprs_demod_take(got->buf[got->n], &got->len[got->n]))
            got->n++;
    }
}

static void *load(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    *size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    void *p = malloc(*size ? *size : 1);
    if (fread(p, 1, *size, f) != *size) {
        free(p);
        p = NULL;
    }
    fclose(f);
    return p;
}

static void test_cases(const char *dir)
{
    char path[512], name[64];
    snprintf(path, sizeof path, "%s/cases.txt", dir);
    FILE *idx = fopen(path, "r");
    if (!idx) {
        printf("  no %s: run gen_audio.py first\n", path);
        g_failed++;
        return;
    }
    int want;
    while (fscanf(idx, "%63s %d", name, &want) == 2) {
        size_t ns, nf;
        snprintf(path, sizeof path, "%s/%s.u16", dir, name);
        uint16_t *s = load(path, &ns);
        snprintf(path, sizeof path, "%s/%s.frames", dir, name);
        uint8_t *fr = load(path, &nf);
        if (!s || !fr) {
            printf("  missing files for %s\n", name);
            g_failed++;
            continue;
        }
        got_t got;
        run(s, ns / 2, &got);

        int ok = got.n == want;
        size_t off = 0;
        for (int k = 0; ok && k < want; k++) {
            uint16_t len = (uint16_t)(fr[off] | (fr[off + 1] << 8));
            off += 2;
            ok = got.len[k] == len && memcmp(got.buf[k], fr + off, len) == 0;
            off += len;
        }
        g_checks++;
        if (!ok) {
            g_failed++;
            printf("  FAIL %-14s got %d frame(s), expected %d\n", name, got.n, want);
        } else {
            printf("  ok   %-14s %d frame(s)\n", name, got.n);
        }
        free(s);
        free(fr);
    }
    fclose(idx);
}

/* The transmitter's bit stream as audio at the ADC: NRZI tones, level and
 * twist like a real radio, white noise. continuous=false restarts the tone at
 * phase 0 at every change (a tone generator that does not keep its phase). */
static size_t synth(const uint8_t *frame, uint16_t len, uint16_t *out, size_t max,
                    bool continuous, double space_gain, double noise, unsigned seed)
{
    srand(seed);
    afsk_bits_t g;
    afsk_bits_init(&g, frame, len, AFSK_PREAMBLE, AFSK_TAIL);
    size_t n = 0;
    double ph = 0, t_bit = 0;
    int space = 0;
    const double spb = (double)APRS_DEMOD_FS / AFSK_BAUD;   /* 8 */
    for (int i = 0; i < 1500 && n < max; i++)               /* noise first */
        out[n++] = (uint16_t)(2048 + noise * ((rand() / (double)RAND_MAX) - 0.5));
    int8_t b;
    while ((b = afsk_bits_next(&g)) >= 0) {
        if (b == 0) {
            space ^= 1;
            if (!continuous)
                ph = 0;
        }
        const double f = space ? AFSK_SPACE_HZ : AFSK_MARK_HZ;
        const double a = space ? 150.0 * space_gain : 150.0;
        t_bit += spb;
        while (t_bit >= 1.0 && n < max) {
            ph += 2 * M_PI * f / APRS_DEMOD_FS;
            double v = 2048 + a * sin(ph) + noise * ((rand() / (double)RAND_MAX) - 0.5);
            out[n++] = (uint16_t)(v < 0 ? 0 : v > 4095 ? 4095 : v);
            t_bit -= 1.0;
        }
    }
    for (int i = 0; i < 1500 && n < max; i++)
        out[n++] = (uint16_t)(2048 + noise * ((rand() / (double)RAND_MAX) - 0.5));
    return n;
}

static void test_loopback(void)
{
    uint8_t frame[AX25_FRAME_MAX];
    ax25_addr_t src, dst, path[2];
    ax25_addr_parse("DL4PW-7", &src);
    ax25_addr_parse("APZK5M", &dst);
    ax25_addr_parse("WIDE1-1", &path[0]);
    ax25_addr_parse("WIDE2-1", &path[1]);

    /* text with long runs of 1s (0xFF bytes are not allowed in text, '~' =
     * 0x7E and '?' = 0x3F give many stuffed bits) */
    const char *infos[] = {
        ":DL1ABC   :Hallo aus der Tasche{17",
        ":DL1ABC   :~~~~???~~~~???{99",
        ":DL1ABC-12:ack3",
    };
    static uint16_t audio[200000];
    for (unsigned i = 0; i < sizeof infos / sizeof infos[0]; i++) {
        const uint16_t len = ax25_build_ui(frame, sizeof frame, &dst, &src, path, 2,
                                           infos[i], (uint16_t)strlen(infos[i]));
        CHECK(len > 0);
        struct { bool cont; double twist, noise; } v[] = {
            {true, 1.0, 20}, {false, 1.0, 20}, {true, 0.5, 40}, {true, 2.0, 40},
        };
        for (unsigned k = 0; k < sizeof v / sizeof v[0]; k++) {
            size_t n = synth(frame, len, audio, sizeof audio / 2, v[k].cont, v[k].twist,
                             v[k].noise, 7u + k);
            got_t got;
            run(audio, n, &got);
            g_checks++;
            if (!(got.n == 1 && got.len[0] == len && memcmp(got.buf[0], frame, len) == 0)) {
                g_failed++;
                printf("  FAIL loopback frame %u variant %u: %d frame(s)\n", i, k, got.n);
            }
        }
    }
    printf("  ok   loopback (%u frames x 4 variants)\n", (unsigned)(sizeof infos / sizeof infos[0]));
}

/* The bit generator against a reference: stuffing after five 1s, also at
 * the very end of the frame. */
static void test_bits(void)
{
    const uint8_t frame[] = {0xFF, 0x1F};          /* 8 ones, then 5 ones at the end */
    afsk_bits_t g;
    afsk_bits_init(&g, frame, 2, 1, 1);
    char got[64] = {0};
    int n = 0;
    int8_t b;
    while ((b = afsk_bits_next(&g)) >= 0 && n < 63)
        got[n++] = (char)('0' + b);
    /* flag 0x7E LSB first = 01111110; 0xFF -> 11111 0 111; 0x1F -> 11 0 111000 + stuffed? */
    /* 0xFF: bits 1,1,1,1,1,(0),1,1,1 ; ones=3 carried
     * 0x1F: 1,1 -> ones=5 -> (0), then 1,1,1,0,0,0 */
    CHECK_STR(got, "01111110" "11111" "0" "111" "11" "0" "111000" "01111110");
}

int main(int argc, char **argv)
{
    test_bits();
    test_loopback();
    if (argc > 1)
        test_cases(argv[1]);
    return TEST_REPORT("demod");
}
