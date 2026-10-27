/* Simulated NOR flash for the host tests: programming can only clear bits,
 * an erase sets a whole 4 KiB sector to 0xFF. Covers the messenger region. */
#ifndef MESSENGER_TEST_FLASH_SIM_H
#define MESSENGER_TEST_FLASH_SIM_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../mailbox.h"
#include "../msg_flash.h"

#define SIM_SIZE (MSG_FLASH_SECTORS * MSG_FLASH_SECTOR)

static uint8_t  sim[SIM_SIZE];
static int      sim_violations;     /* program that tried to set a bit */
static int      sim_erases;
static int      sim_drop_programs;  /* >0: ignore the next N programs (power loss) */

static void sim_reset(uint8_t fill)
{
    memset(sim, fill, sizeof sim);
    sim_violations = sim_erases = sim_drop_programs = 0;
}

static uint32_t sim_off(uint32_t addr, uint32_t len)
{
    if (addr < MSG_FLASH_BASE || addr + len > MSG_FLASH_BASE + SIM_SIZE) {
        printf("  flash access outside the region: %06x+%u\n", addr, len);
        sim_violations++;
        return 0;
    }
    return addr - MSG_FLASH_BASE;
}

void msg_flash_read(uint32_t addr, void *buf, uint32_t len)
{
    memcpy(buf, sim + sim_off(addr, len), len);
}

void msg_flash_program(uint32_t addr, const void *buf, uint32_t len)
{
    if (sim_drop_programs > 0) {
        sim_drop_programs--;
        return;
    }
    uint8_t *p = sim + sim_off(addr, len);
    const uint8_t *s = buf;
    for (uint32_t i = 0; i < len; i++) {
        if ((p[i] & s[i]) != s[i])
            sim_violations++;
        p[i] &= s[i];
    }
}

void msg_flash_erase(uint32_t sector_addr)
{
    if (sector_addr % MSG_FLASH_SECTOR) {
        sim_violations++;
        return;
    }
    memset(sim + sim_off(sector_addr, MSG_FLASH_SECTOR), 0xFF, MSG_FLASH_SECTOR);
    sim_erases++;
}

#endif
