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

#include "msg_crypto.h"

static const uint8_t NONCE_MAC[12] = {'m', 's', 'g', '-', 'm', 'a', 'c', '-', 'k', 'e', 'y', 0};
static const uint8_t NONCE_KDF[12] = {'m', 's', 'g', '-', 'k', 'd', 'f', '-', 'v', '1', 0, 0};
static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* ------------------------------------------------------------ ChaCha20 -- */

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t rotl32(uint32_t v, uint8_t n)
{
    return (v << n) | (v >> (32u - n));
}

static void qr(uint32_t *x, uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    x[a] += x[b]; x[d] = rotl32(x[d] ^ x[a], 16);
    x[c] += x[d]; x[b] = rotl32(x[b] ^ x[c], 12);
    x[a] += x[b]; x[d] = rotl32(x[d] ^ x[a], 8);
    x[c] += x[d]; x[b] = rotl32(x[b] ^ x[c], 7);
}

void mc_chacha20_block(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12],
                       uint8_t out[64])
{
    uint32_t s[16], x[16];
    s[0] = 0x61707865u; s[1] = 0x3320646eu; s[2] = 0x79622d32u; s[3] = 0x6b206574u;
    for (uint8_t i = 0; i < 8u; i++)
        s[4 + i] = le32(key + 4u * i);
    s[12] = counter;
    for (uint8_t i = 0; i < 3u; i++)
        s[13 + i] = le32(nonce + 4u * i);
    memcpy(x, s, sizeof x);
    for (uint8_t r = 0; r < 10u; r++) {
        qr(x, 0, 4, 8, 12); qr(x, 1, 5, 9, 13); qr(x, 2, 6, 10, 14); qr(x, 3, 7, 11, 15);
        qr(x, 0, 5, 10, 15); qr(x, 1, 6, 11, 12); qr(x, 2, 7, 8, 13); qr(x, 3, 4, 9, 14);
    }
    for (uint8_t i = 0; i < 16u; i++) {
        const uint32_t v = x[i] + s[i];
        out[4 * i]     = (uint8_t)v;
        out[4 * i + 1] = (uint8_t)(v >> 8);
        out[4 * i + 2] = (uint8_t)(v >> 16);
        out[4 * i + 3] = (uint8_t)(v >> 24);
    }
}

/* ----------------------------------------------------------- SipHash -- */

static uint64_t rotl64(uint64_t v, uint8_t n)
{
    return (v << n) | (v >> (64u - n));
}

static void sipround(uint64_t *v)
{
    v[0] += v[1]; v[1] = rotl64(v[1], 13); v[1] ^= v[0]; v[0] = rotl64(v[0], 32);
    v[2] += v[3]; v[3] = rotl64(v[3], 16); v[3] ^= v[2];
    v[0] += v[3]; v[3] = rotl64(v[3], 21); v[3] ^= v[0];
    v[2] += v[1]; v[1] = rotl64(v[1], 17); v[1] ^= v[2]; v[2] = rotl64(v[2], 32);
}

static void sipblock(uint64_t *v, uint64_t m)
{
    v[3] ^= m;
    sipround(v);
    sipround(v);
    v[0] ^= m;
}

uint64_t mc_siphash24(const uint8_t key[16], const uint8_t *data, uint16_t len)
{
    const uint64_t k0 = le32(key) | ((uint64_t)le32(key + 4) << 32);
    const uint64_t k1 = le32(key + 8) | ((uint64_t)le32(key + 12) << 32);
    uint64_t v[4] = {
        k0 ^ 0x736f6d6570736575ull, k1 ^ 0x646f72616e646f6dull,
        k0 ^ 0x6c7967656e657261ull, k1 ^ 0x7465646279746573ull,
    };
    uint64_t m = 0;
    uint16_t i = 0;
    for (; i < len; i++) {
        m |= (uint64_t)data[i] << (8u * (i & 7u));
        if ((i & 7u) == 7u) {
            sipblock(v, m);
            m = 0;
        }
    }
    sipblock(v, m | ((uint64_t)len << 56));
    v[2] ^= 0xFFu;
    for (uint8_t r = 0; r < 4u; r++)
        sipround(v);
    return v[0] ^ v[1] ^ v[2] ^ v[3];
}

/* --------------------------------------------------------------- KDF -- */

void mc_kdf(const char *pass, uint8_t key[MC_KEY_LEN])
{
    uint8_t block[64];
    memset(key, 0, MC_KEY_LEN);
    for (uint8_t i = 0; pass[i] && i < MC_PASS_MAX; i++)
        key[i] = (uint8_t)pass[i];
    for (uint16_t r = 0; r < MC_KDF_ROUNDS; r++) {
        mc_chacha20_block(key, r, NONCE_KDF, block);
        memcpy(key, block, MC_KEY_LEN);
    }
    memset(block, 0, sizeof block);
}

/* ------------------------------------------------------- the message -- */

static void mac_key(const uint8_t key[MC_KEY_LEN], uint8_t out[16])
{
    uint8_t block[64];
    mc_chacha20_block(key, 0, NONCE_MAC, block);
    memcpy(out, block, 16);
}

/* "sender\0ADDRESSEE\0" (upper case, no spaces) into buf; returns the length */
static uint16_t context(char *buf, const char *sender, const char *addressee)
{
    uint16_t n = 0;
    for (const char *s = sender; *s && n < 12u; s++)
        buf[n++] = (*s >= 'a' && *s <= 'z') ? (char)(*s - 32) : *s;
    buf[n++] = '\0';
    for (const char *s = addressee; *s && n < 24u; s++)
        if (*s != ' ')
            buf[n++] = (*s >= 'a' && *s <= 'z') ? (char)(*s - 32) : *s;
    buf[n++] = '\0';
    return n;
}

/* ChaCha20 nonce: the message nonce, then the sender's SipHash; XORs the
 * keystream into buf. Returns the tag over context | nonce | ciphertext. */
static uint32_t crypt(const uint8_t key[MC_KEY_LEN], const char *sender, const char *addressee,
                      uint8_t *payload, uint16_t n, bool encrypting)
{
    uint8_t mk[16], nonce[12], ks[64];
    char ctx[24 + 4 + MC_PLAIN_MAX + 2];
    mac_key(key, mk);
    memcpy(nonce, payload, 4);
    const uint64_t sh = mc_siphash24(mk, (const uint8_t *)sender, (uint16_t)strlen(sender));
    for (uint8_t i = 0; i < 8u; i++)
        nonce[4 + i] = (uint8_t)(sh >> (8u * i));

    uint16_t c = context(ctx, sender, addressee);
    if (!encrypting) {                  /* the tag covers the ciphertext */
        memcpy(ctx + c, payload, 4u + n);
    }
    mc_chacha20_block(key, 1, nonce, ks);
    for (uint16_t i = 0; i < n; i++)
        payload[4 + i] ^= ks[i];
    if (encrypting)
        memcpy(ctx + c, payload, 4u + n);
    const uint32_t tag = (uint32_t)mc_siphash24(mk, (const uint8_t *)ctx, (uint16_t)(c + 4u + n));
    memset(ks, 0, sizeof ks);
    memset(mk, 0, sizeof mk);
    return tag;
}

uint16_t mc_encrypt(const uint8_t key[MC_KEY_LEN], const char *sender, const char *addressee,
                    uint32_t nonce, const char *plain, char *out, uint16_t out_max)
{
    const uint16_t n = (uint16_t)strlen(plain);
    const uint16_t raw = (uint16_t)(8u + n);
    const uint16_t len = (uint16_t)(2u + (raw * 4u + 2u) / 3u);
    if (n > MC_PLAIN_MAX || len + 1u > out_max)
        return 0;

    uint8_t p[8 + MC_PLAIN_MAX];
    for (uint8_t i = 0; i < 4u; i++)
        p[i] = (uint8_t)(nonce >> (8u * i));
    memcpy(p + 4, plain, n);
    const uint32_t tag = crypt(key, sender, addressee, p, n, true);
    for (uint8_t i = 0; i < 4u; i++)
        p[4 + n + i] = (uint8_t)(tag >> (8u * i));

    /* base64 without padding */
    char *o = out;
    *o++ = MC_PREFIX[0];
    *o++ = MC_PREFIX[1];
    uint32_t acc = 0;
    uint8_t bits = 0;
    for (uint16_t i = 0; i < raw; i++) {
        acc = (acc << 8) | p[i];
        bits = (uint8_t)(bits + 8u);
        while (bits >= 6u) {
            bits = (uint8_t)(bits - 6u);
            *o++ = B64[(acc >> bits) & 63u];
        }
    }
    if (bits)
        *o++ = B64[(acc << (6u - bits)) & 63u];
    *o = '\0';
    return (uint16_t)(o - out);
}

bool mc_is_encrypted(const char *text)
{
    return text[0] == MC_PREFIX[0] && text[1] == MC_PREFIX[1] && text[2];
}

bool mc_decrypt(const uint8_t key[MC_KEY_LEN], const char *sender, const char *addressee,
                const char *text, char *plain, uint16_t plain_max)
{
    if (!mc_is_encrypted(text))
        return false;
    uint8_t p[8 + MC_PLAIN_MAX];
    uint16_t raw = 0;
    uint32_t acc = 0;
    uint8_t bits = 0;
    for (const char *s = text + 2; *s; s++) {
        const char *hit = strchr(B64, *s);
        if (!hit)
            return false;
        acc = (acc << 6) | (uint32_t)(hit - B64);
        bits = (uint8_t)(bits + 6u);
        if (bits >= 8u) {
            bits = (uint8_t)(bits - 8u);
            if (raw >= sizeof p)
                return false;
            p[raw++] = (uint8_t)(acc >> bits);
        }
    }
    if (raw < 8u)
        return false;
    const uint16_t n = (uint16_t)(raw - 8u);
    if (n + 1u > plain_max)
        return false;
    const uint32_t got = le32(p + 4 + n);
    const uint32_t tag = crypt(key, sender, addressee, p, n, false);
    if (tag != got)
        return false;
    for (uint16_t i = 0; i < n; i++) {
        const uint8_t c = p[4 + i];
        plain[i] = (c >= 0x20u && c <= 0x7Eu) ? (char)c : '?';
    }
    plain[n] = '\0';
    return true;
}
