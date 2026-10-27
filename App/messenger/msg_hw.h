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

/* Messenger hardware: the background RX audio capture (TIM3 -> ADC1 on PA4 ->
 * DMA1 channel 1 -> demodulator) and the AFSK transmitter (BK4829 tone
 * generator, bit timing from TIM3). */

#ifndef MESSENGER_MSG_HW_H
#define MESSENGER_MSG_HW_H

#include <stdbool.h>
#include <stdint.h>

#include "msg_port.h"

struct VFO_Info_t;                 /* radio.h */

typedef struct {
    uint16_t load_pm;          /* demodulator share of the CPU, per mille (average) */
    uint16_t load_max_pm;      /* worst block since boot                            */
    uint16_t level_pp;         /* audio peak-to-peak of the last 100 ms, ADC codes  */
    uint16_t overruns;         /* blocks the interrupt was too late for             */
    uint32_t blocks;           /* 128-sample blocks processed                       */
} msg_hw_diag_t;

void            msg_hw_capture_start(void);
void            msg_hw_capture_stop(void);
bool            msg_hw_capture_running(void);
void            msg_hw_diag(msg_hw_diag_t *d);

/* The battery conversion borrows the ADC for a moment. */
bool            msg_hw_yield_adc(void);
void            msg_hw_resume_adc(void);

/* One frame on vfo's TX frequency, blocking. The capture is stopped and
 * restarted around it; the radio registers are restored afterwards. */
msg_tx_result_t msg_hw_transmit(struct VFO_Info_t *vfo, const uint8_t *frame, uint16_t len);

#endif
