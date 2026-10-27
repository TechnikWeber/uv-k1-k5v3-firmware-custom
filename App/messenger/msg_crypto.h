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

/* Optional message encryption, for bands where encryption is allowed (never
 * amateur radio). Encrypt-then-MAC with two standard primitives that are small
 * on a Cortex-M0:
 *
 *   ChaCha20 (RFC 8439)   confidentiality
 *   SipHash-2-4           authenticity, 32-bit tag
 *
 * Keys: key = KDF(passphrase), 32 bytes; the MAC key is the first 16 bytes of
 * a ChaCha20 block of key under a fixed domain nonce. KDF: the passphrase as a
 * ChaCha20 key, iterated MC_KDF_ROUNDS times (key = first half of the next
 * block), so every guess costs that many blocks.
 *
 * On the air the message text is "@@" + base64(nonce(4) | ciphertext | tag(4)).
 * The ChaCha20 nonce is nonce(4) | SipHash(macKey, sender)(8): unique per
 * sender and message. The tag covers sender, addressee, nonce and ciphertext,
 * so a message cannot be replayed under another sender or to another
 * recipient. The message number ({id) and acks stay in the clear: APRS needs
 * them to deliver.
 *
 * Pure C, built by the host tests (test_crypto: RFC test vectors). */

#ifndef MESSENGER_MSG_CRYPTO_H
#define MESSENGER_MSG_CRYPTO_H

#include <stdbool.h>
#include <stdint.h>

#define MC_KEY_LEN      32u
#define MC_PASS_MAX     32u         /* passphrase characters                  */
#define MC_PLAIN_MAX    40u         /* text that still fits 67 APRS characters */
#define MC_KDF_ROUNDS   4096u
#define MC_PREFIX       "@@"

/* primitives, exposed for the test vectors */
void     mc_chacha20_block(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12],
                           uint8_t out[64]);
uint64_t mc_siphash24(const uint8_t key[16], const uint8_t *data, uint16_t len);

void     mc_kdf(const char *pass, uint8_t key[MC_KEY_LEN]);

/* Builds the encrypted text ("@@...", NUL-terminated) for plain (at most
 * MC_PLAIN_MAX characters). Returns its length, 0 if it does not fit. */
uint16_t mc_encrypt(const uint8_t key[MC_KEY_LEN], const char *sender, const char *addressee,
                    uint32_t nonce, const char *plain, char *out, uint16_t out_max);

bool     mc_is_encrypted(const char *text);

/* Decrypts text in place into plain (NUL-terminated). false: not decodable, or
 * the tag does not match (another key, or tampered). */
bool     mc_decrypt(const uint8_t key[MC_KEY_LEN], const char *sender, const char *addressee,
                    const char *text, char *plain, uint16_t plain_max);

#endif
