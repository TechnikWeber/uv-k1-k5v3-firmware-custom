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

/* The messenger as the rest of the firmware sees it. Every hook in the F4HWN
 * code is one call into this header, under #ifdef ENABLE_MESSENGER. */

#ifndef MESSENGER_MSG_FW_H
#define MESSENGER_MSG_FW_H

#include <stdbool.h>
#include <stdint.h>

#include "driver/keyboard.h"

/* main.c, after the settings and the radio are set up */
void MSG_Init(void);
/* scheduler.c, SysTick_Handler (every 10 ms, interrupt) */
void MSG_SysTick10ms(void);
/* app.c, APP_TimeSlice10ms */
void MSG_TimeSlice10ms(void);

/* app.c CheckForIncoming: the squelch opened on the APRS frequency and the
 * APRS audio is off. The radio stays in the foreground (no speaker, the FM
 * radio keeps playing); only dual watch holds on this VFO. */
bool MSG_IsSilentRx(void);
/* app.c, power save: the receiver must stay awake for the APRS preamble. */
bool MSG_BlocksPowerSave(void);
/* ui/status.c: the envelope icon */
bool MSG_HasUnread(void);
/* board.c BOARD_ADC_GetBatteryInfo: the capture lends the ADC for one
 * battery conversion. */
bool MSG_CaptureYieldAdc(void);
void MSG_CaptureResumeAdc(void);

/* the messages screen (DISPLAY_MESSENGER) */
void MSG_OpenScreen(void);                 /* action "MESSAGES" */
void MSG_DisplayScreen(void);
void MSG_ProcessKeys(KEY_Code_t Key, bool bKeyPressed, bool bKeyHeld);

#endif
