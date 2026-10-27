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

/* Bell 202 AFSK 1200 bd demodulator and HDLC deframer for 9.6 kHz ADC samples.
 *
 * The signal processing is the one of the APRS RX overlay app by Armel F4HWN
 * (App/apps/aprsrx/aprsrx_app.c, modelled in App/apps/aprsrx/test/model_rx.py),
 * moved from a busy-wait loop to blocks of samples that the DMA interrupt
 * hands over, so it runs in the background of the normal firmware.
 *
 * Pure C: the host tests feed it simulated audio. */

#ifndef MESSENGER_APRS_DEMOD_H
#define MESSENGER_APRS_DEMOD_H

#include <stdbool.h>
#include <stdint.h>

#include "aprs_ax25.h"

#define APRS_DEMOD_FS        9600u   /* ADC sample rate                     */

/* Resets every filter, slicer and the received-frame slot. */
void aprs_demod_reset(void);

/* Runs n 12-bit ADC samples (PA4, centred on ~2048) through the demodulator.
 * Called from the DMA interrupt. */
void aprs_demod_process(const uint16_t *samples, uint16_t n);

/* A slicer is inside a preamble or a frame that looks like APRS: the channel
 * is busy (carrier sense for the transmitter). */
bool aprs_demod_busy(void);

/* Takes the last good UI frame (FCS included in len) out of the one-frame
 * slot; false when there is none. Main-loop side. */
bool aprs_demod_take(uint8_t *out, uint16_t *len);

/* Frames dropped because the slot was still full (diagnostics). */
uint16_t aprs_demod_dropped(void);

#endif
