/* Host test: multi-tap text entry. */
#include <stdint.h>

#include "test_util.h"
#include "../t9.h"

static uint32_t now;

static void keys(t9_t *t, const char *seq)
{
    /* digits are taps 300 ms apart; ' ' waits for the timeout */
    for (; *seq; seq++) {
        if (*seq == ' ') {
            now += T9_TAP_MS + 10;
            t9_tick(t, now);
        } else {
            t9_digit(t, (uint8_t)(*seq - '0'), now);
            now += 300;
        }
    }
    now += T9_TAP_MS + 10;
    t9_tick(t, now);
}

int main(void)
{
    t9_t t;

    /* "Hallo" in sentence mode: capital H only */
    t9_init(&t, 67, false, NULL);
    keys(&t, "44 2 555 555 666");
    CHECK_STR(t.buf, "Hallo");

    /* space, then a new sentence after '!' */
    keys(&t, "1111 0 9 2");
    CHECK_STR(t.buf, "Hallo! Wa");

    /* another key fixes the letter at once */
    t9_init(&t, 67, false, NULL);
    t9_next_mode(&t);                  /* abc */
    CHECK_STR(t9_mode_name(&t), "abc");
    t9_digit(&t, 2, now);
    t9_digit(&t, 3, now + 100);
    CHECK_INT(t.len, 1);
    CHECK_INT(t9_pending(&t), 'd');
    t9_commit(&t);
    CHECK_STR(t.buf, "ad");

    /* the letters wrap round: 2 x5 = a b c 2 a */
    t9_init(&t, 67, false, NULL);
    t9_next_mode(&t);
    keys(&t, "22222");
    CHECK_STR(t.buf, "a");

    /* digits mode */
    t9_init(&t, 67, false, NULL);
    t9_next_mode(&t); t9_next_mode(&t); t9_next_mode(&t);
    CHECK_STR(t9_mode_name(&t), "123");
    t9_digit(&t, 1, now); t9_digit(&t, 4, now); t9_digit(&t, 4, now);
    CHECK_STR(t.buf, "144");

    /* editing in the middle, backspace */
    t9_init(&t, 67, false, "Hllo");
    t9_left(&t); t9_left(&t); t9_left(&t);
    CHECK_INT(t.cursor, 1);
    t9_next_mode(&t);                  /* abc */
    keys(&t, "2");
    CHECK_STR(t.buf, "Hallo");
    CHECK_INT(t.cursor, 2);
    t9_backspace(&t);
    CHECK_STR(t.buf, "Hllo");
    t9_digit(&t, 2, now);              /* pending letter: backspace drops it */
    t9_backspace(&t);
    t9_commit(&t);
    CHECK_STR(t.buf, "Hllo");
    t9_right(&t); t9_right(&t); t9_right(&t); t9_right(&t);
    CHECK_INT(t.cursor, 4);

    /* the limit */
    t9_init(&t, 3, false, "abc");
    keys(&t, "2");
    CHECK_STR(t.buf, "abc");

    /* callsign mode: upper case, '-' on key 1, digits mode toggle */
    t9_init(&t, 9, true, NULL);
    keys(&t, "3 555");
    t9_next_mode(&t);
    t9_digit(&t, 4, now);
    t9_next_mode(&t);
    keys(&t, "7 1 99999");             /* P - 9 (W X Y Z 9) */
    CHECK_STR(t.buf, "DL4P-9");

    return TEST_REPORT("t9");
}
