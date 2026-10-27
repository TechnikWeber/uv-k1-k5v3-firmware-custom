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

/* The messages screen: mailbox list, reading, writing with multi-tap, setup. */

#ifndef MESSENGER_MSG_UI_H
#define MESSENGER_MSG_UI_H

#include <stdbool.h>

/* every 10 ms: multi-tap timeout, popups */
void msg_ui_tick(void);

/* msg_port.c */
bool msg_port_has_aprs_vfo(void);

#endif
