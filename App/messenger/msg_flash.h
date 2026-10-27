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

/* External flash access used by the mailbox and the settings: the radio maps
 * these onto the PY25Q16 driver (msg_port.c), the host tests onto a simulated
 * NOR flash. msg_flash_program() is only ever called on erased bytes or to
 * clear bits, so the driver never needs an erase for it. */

#ifndef MESSENGER_MSG_FLASH_H
#define MESSENGER_MSG_FLASH_H

#include <stdint.h>

void msg_flash_read(uint32_t addr, void *buf, uint32_t len);
void msg_flash_program(uint32_t addr, const void *buf, uint32_t len);
void msg_flash_erase(uint32_t sector_addr);

#endif
