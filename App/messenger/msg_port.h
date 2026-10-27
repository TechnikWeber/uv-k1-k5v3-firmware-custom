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

/* What the messenger core needs from the radio. msg_port.c implements it on
 * the firmware, test/test_messenger.c with mocks. */

#ifndef MESSENGER_MSG_PORT_H
#define MESSENGER_MSG_PORT_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    MSG_TX_OK = 0,
    MSG_TX_NO_VFO,        /* no VFO is tuned to the APRS frequency          */
    MSG_TX_LOCKED,        /* TX not allowed on that frequency               */
    MSG_TX_BATTERY,       /* battery too low (or too high)                  */
    MSG_TX_BUSY,          /* the radio is receiving or transmitting         */
} msg_tx_result_t;

uint32_t        msg_port_ms(void);              /* monotonic milliseconds   */
uint32_t        msg_port_random(void);
/* A good frame from the demodulator (len includes the FCS). */
bool            msg_port_take_frame(uint8_t *buf, uint16_t *len);
/* The APRS channel is clear right now: an APRS VFO is the receiving one, the
 * squelch is closed, the demodulator hears no preamble, the user is not
 * transmitting, nothing is scanning. */
bool            msg_port_channel_clear(void);
/* Sends one frame on the APRS frequency, blocking (~1 s). */
msg_tx_result_t msg_port_transmit(const uint8_t *frame, uint16_t len);
/* A new message arrived: beep, backlight, status bar. */
void            msg_port_notify(void);

#endif
