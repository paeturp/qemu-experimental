/* Local STM32F411 experiment. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_ARM_STM32F411_SOC_H
#define HW_ARM_STM32F411_SOC_H

#include "hw/arm/armv7m.h"
#include "hw/char/stm32f2xx_usart.h"
#include "hw/core/clock.h"
#include "qom/object.h"

#define TYPE_STM32F411_SOC "stm32f411-soc"
OBJECT_DECLARE_SIMPLE_TYPE(STM32F411State, STM32F411_SOC)
#define F411_FLASH_SIZE (512 * 1024)
#define F411_SRAM_SIZE (128 * 1024)

typedef struct F411GPIO {
    uint32_t regs[10];
    uint16_t inputs, driven;
    qemu_irq pins[16];
    STM32F411State *soc;
    MemoryRegion mmio;
} F411GPIO;

struct STM32F411State {
    SysBusDevice parent_obj;
    ARMv7MState armv7m;
    STM32F2XXUsartState usart2;
    MemoryRegion flash, flash_alias, sram;
    MemoryRegion rcc_mmio, pwr_mmio, flashctl_mmio, rtc_mmio;
    Clock *hclk, *refclk, *pclk1, *pclk2;
    uint32_t rcc[37], pwr[2], flashctl[6], rtc[40];
    F411GPIO gpioa;
    int64_t rtc_last_ns, rtc_fraction_ns;
    uint32_t rtc_shadow_tr, rtc_shadow_dr, rtc_shadow_ssr;
    bool rtc_shadow_locked, rtc_unlocked;
    uint8_t rtc_key;
};
#endif
