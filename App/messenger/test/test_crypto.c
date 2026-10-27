/* Host test: ChaCha20 and SipHash against their published test vectors, and
 * the message encryption (round trip, wrong key, tampering, context binding). */
#include <stdint.h>

#include "test_util.h"
#include "../aprs_msg.h"
#include "../msg_crypto.h"

static void test_vectors(void)
{
    /* RFC 8439, 2.3.2: block function */
    uint8_t key[32], nonce[12] = {0, 0, 0, 0x09, 0, 0, 0, 0x4a, 0, 0, 0, 0}, out[64];
    for (int i = 0; i < 32; i++)
        key[i] = (uint8_t)i;
    mc_chacha20_block(key, 1, nonce, out);
    static const uint8_t want[16] = {0x10, 0xf1, 0xe7, 0xe4, 0xd1, 0x3b, 0x59, 0x15,
                                     0x50, 0x0f, 0xdd, 0x1f, 0xa3, 0x20, 0x71, 0xc4};
    CHECK(memcmp(out, want, 16) == 0);
    static const uint8_t want_end[4] = {0xa2, 0x50, 0x3c, 0x4e};
    CHECK(memcmp(out + 60, want_end, 4) == 0);

    /* SipHash-2-4 paper, appendix A: key 00..0f, message 00..0e */
    uint8_t k16[16], msg[15];
    for (int i = 0; i < 16; i++)
        k16[i] = (uint8_t)i;
    for (int i = 0; i < 15; i++)
        msg[i] = (uint8_t)i;
    CHECK(mc_siphash24(k16, msg, 15) == 0xa129ca6149be45e5ull);
    CHECK(mc_siphash24(k16, msg, 0) == 0x726fdb47dd0e0e31ull);
}

static void test_messages(void)
{
    uint8_t key[MC_KEY_LEN], other[MC_KEY_LEN];
    mc_kdf("Familie Weber 2026!", key);
    mc_kdf("Familie Weber 2026?", other);
    CHECK(memcmp(key, other, MC_KEY_LEN) != 0);

    char wire[80], plain[64];
    uint16_t n = mc_encrypt(key, "DL4PW-7", "DL1ABC-9", 0x12345678u, "Treffen 18 Uhr am Bahnhof", wire,
                            sizeof wire);
    CHECK(n > 2);
    CHECK(mc_is_encrypted(wire));
    CHECK(strstr(wire, "Treffen") == NULL);
    /* only characters an APRS message may carry */
    CHECK(strpbrk(wire, "|~{") == NULL);

    CHECK(mc_decrypt(key, "DL4PW-7", "DL1ABC-9", wire, plain, sizeof plain));
    CHECK_STR(plain, "Treffen 18 Uhr am Bahnhof");
    /* the addressee as it is padded on the air, lower case sender */
    CHECK(mc_decrypt(key, "dl4pw-7", "DL1ABC-9 ", wire, plain, sizeof plain));

    CHECK(!mc_decrypt(other, "DL4PW-7", "DL1ABC-9", wire, plain, sizeof plain));
    CHECK(!mc_decrypt(key, "DL4PW-8", "DL1ABC-9", wire, plain, sizeof plain));   /* other sender */
    CHECK(!mc_decrypt(key, "DL4PW-7", "DL1ABC-8", wire, plain, sizeof plain));   /* other recipient */

    char bad[80];
    strcpy(bad, wire);
    bad[10] = bad[10] == 'A' ? 'B' : 'A';                    /* one character changed */
    CHECK(!mc_decrypt(key, "DL4PW-7", "DL1ABC-9", bad, plain, sizeof plain));
    strcpy(bad, wire);
    bad[n - 3] = '\0';                                       /* truncated */
    CHECK(!mc_decrypt(key, "DL4PW-7", "DL1ABC-9", bad, plain, sizeof plain));
    CHECK(!mc_decrypt(key, "DL4PW-7", "DL1ABC-9", "@@not base64!", plain, sizeof plain));
    CHECK(!mc_decrypt(key, "DL4PW-7", "DL1ABC-9", "plain text", plain, sizeof plain));

    /* another nonce, another ciphertext */
    char wire2[80];
    mc_encrypt(key, "DL4PW-7", "DL1ABC-9", 0x12345679u, "Treffen 18 Uhr am Bahnhof", wire2, sizeof wire2);
    CHECK(strcmp(wire, wire2) != 0);

    /* the longest plain text still fits an APRS message with "{999" */
    char longest[MC_PLAIN_MAX + 2];
    memset(longest, 'x', MC_PLAIN_MAX);
    longest[MC_PLAIN_MAX] = '\0';
    n = mc_encrypt(key, "DL4PW-7", "BLN0WEBER", 1, longest, wire, sizeof wire);
    CHECK(n > 0 && n <= APRS_TEXT_MAX);
    CHECK(mc_decrypt(key, "DL4PW-7", "BLN0WEBER", wire, plain, sizeof plain));
    CHECK_STR(plain, longest);
    longest[MC_PLAIN_MAX] = 'x';
    longest[MC_PLAIN_MAX + 1] = '\0';
    CHECK_INT(mc_encrypt(key, "DL4PW-7", "BLN0WEBER", 1, longest, wire, sizeof wire), 0);

    /* empty text */
    n = mc_encrypt(key, "DL4PW-7", "DL1ABC", 5, "", wire, sizeof wire);
    CHECK(n > 0);
    CHECK(mc_decrypt(key, "DL4PW-7", "DL1ABC", wire, plain, sizeof plain));
    CHECK_STR(plain, "");
}

int main(void)
{
    test_vectors();
    test_messages();
    return TEST_REPORT("crypto");
}
