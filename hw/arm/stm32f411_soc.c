/*
 * STM32F411 peripheral subset for local board emulation experiments.
 * Register definitions follow ST RM0383. No cycle-accurate timing.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "migration/vmstate.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/irq.h"
#include "hw/core/resettable.h"
#include "hw/arm/stm32f411_soc.h"
#include "system/system.h"
#include "system/address-spaces.h"
#include "trace.h"

#define CR       (0x00 / 4)
#define PLLCFGR  (0x04 / 4)
#define CFGR     (0x08 / 4)
#define AHB1RSTR (0x10 / 4)
#define APB1RSTR (0x20 / 4)
#define AHB1ENR  (0x30 / 4)
#define APB1ENR  (0x40 / 4)
#define BDCR     (0x70 / 4)
#define CSR      (0x74 / 4)
#define TR       (0x00 / 4)
#define DR       (0x04 / 4)
#define RTC_CR   (0x08 / 4)
#define ISR      (0x0c / 4)
#define PRER     (0x10 / 4)
#define WPR      (0x24 / 4)
#define SSR      (0x28 / 4)
#define BKP0R    (0x50 / 4)

static uint32_t bcd(unsigned v)
{
    return ((v / 10) << 4) | (v % 10);
}

static unsigned unbcd(unsigned v)
{
    return (v >> 4) * 10 + (v & 15);
}

static bool backup_writable(STM32F411State *s)
{
    return !!(s->rcc[APB1ENR] & BIT(28)) && !!(s->pwr[0] & BIT(8));
}

static unsigned rtc_hz(STM32F411State *s)
{
    if (!(s->rcc[BDCR] & BIT(15)) || (s->rcc[BDCR] & BIT(16))) {
        return 0;
    }
    switch ((s->rcc[BDCR] >> 8) & 3) {
    case 1:
        return (s->rcc[BDCR] & BIT(1)) ? 32768 : 0;
    case 2:
        return (s->rcc[CSR] & BIT(1)) ? 32000 : 0;
    default:
        return 0;
    }
}

static void rtc_sync(STM32F411State *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    unsigned hz = rtc_hz(s);
    int64_t period, seconds;
    struct tm tm = { 0 };
    time_t epoch, previous;
    unsigned weekday;

    if (!hz || (s->rtc[ISR] & BIT(7))) {
        s->rtc_last_ns = now;
        return;
    }
    period = (int64_t)(((s->rtc[PRER] >> 16) & 127) + 1) *
             ((s->rtc[PRER] & 32767) + 1) * NANOSECONDS_PER_SECOND / hz;
    s->rtc_fraction_ns += now - s->rtc_last_ns;
    s->rtc_last_ns = now;
    seconds = s->rtc_fraction_ns / period;
    s->rtc_fraction_ns %= period;
    if (!seconds) {
        return;
    }
    tm.tm_sec = unbcd(s->rtc[TR] & 0x7f);
    tm.tm_min = unbcd((s->rtc[TR] >> 8) & 0x7f);
    tm.tm_hour = unbcd((s->rtc[TR] >> 16) & 0x3f);
    tm.tm_mday = unbcd(s->rtc[DR] & 0x3f);
    tm.tm_mon = unbcd((s->rtc[DR] >> 8) & 0x1f) - 1;
    tm.tm_year = 100 + unbcd((s->rtc[DR] >> 16) & 0xff);
    previous = mktimegm(&tm);
    epoch = previous + seconds;
    weekday = (s->rtc[DR] >> 13) & 7;
    weekday = (weekday + (epoch / 86400 - previous / 86400) - 1) % 7 + 1;
    gmtime_r(&epoch, &tm);
    s->rtc[TR] = bcd(tm.tm_hour) << 16 | bcd(tm.tm_min) << 8 | bcd(tm.tm_sec);
    s->rtc[DR] = bcd((tm.tm_year - 100) % 100) << 16 |
                 weekday << 13 |
                 bcd(tm.tm_mon + 1) << 8 | bcd(tm.tm_mday);
}

static void rtc_backup_reset(STM32F411State *s)
{
    memset(s->rtc, 0, sizeof(s->rtc));
    s->rtc[DR] = 0x2101;
    s->rtc[PRER] = 0x007f00ff;
    s->rtc[ISR] = BIT(0) | BIT(1) | BIT(5);
    s->rtc_last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->rtc_fraction_ns = 0;
    s->rtc_shadow_locked = false;
    s->rtc_unlocked = false;
    s->rtc_key = 0;
}

static uint32_t rtc_subseconds(STM32F411State *s)
{
    unsigned hz = rtc_hz(s);
    unsigned prediv_a = ((s->rtc[PRER] >> 16) & 127) + 1;
    unsigned prediv_s = s->rtc[PRER] & 32767;
    uint64_t ticks = hz ? s->rtc_fraction_ns * hz /
                         (NANOSECONDS_PER_SECOND * prediv_a) : 0;

    return prediv_s - MIN(ticks, prediv_s);
}

static uint64_t rtc_read(void *opaque, hwaddr addr, unsigned size)
{
    STM32F411State *s = opaque;
    unsigned reg = addr / 4;
    uint32_t value;

    rtc_sync(s);
    if (reg >= ARRAY_SIZE(s->rtc)) {
        return 0;
    }
    if (reg == WPR) {
        return 0;
    }
    if (reg == ISR && (s->rtc[ISR] & BIT(7)) && rtc_hz(s)) {
        s->rtc[ISR] |= BIT(6);
    }
    if ((reg == TR || reg == SSR) && !(s->rtc[RTC_CR] & BIT(5))) {
        if (!s->rtc_shadow_locked) {
            s->rtc_shadow_tr = s->rtc[TR];
            s->rtc_shadow_dr = s->rtc[DR];
            s->rtc_shadow_ssr = rtc_subseconds(s);
            s->rtc_shadow_locked = true;
        }
        return reg == TR ? s->rtc_shadow_tr : s->rtc_shadow_ssr;
    }
    if (reg == DR) {
        value = s->rtc_shadow_locked ? s->rtc_shadow_dr : s->rtc[DR];
        s->rtc_shadow_locked = false;
        return value;
    }
    return reg == SSR ? rtc_subseconds(s) : s->rtc[reg];
}

static void rtc_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    STM32F411State *s = opaque;
    unsigned reg = addr / 4;

    rtc_sync(s);
    if (reg >= ARRAY_SIZE(s->rtc) || !backup_writable(s) ||
        (s->rcc[BDCR] & BIT(16))) {
        return;
    }
    if (reg >= BKP0R) {
        s->rtc[reg] = value;
        return;
    }
    if (reg == WPR) {
        s->rtc_unlocked = s->rtc_key == 0xca && value == 0x53;
        s->rtc_key = value;
        return;
    }
    if (!s->rtc_unlocked) {
        return;
    }
    switch (reg) {
    case ISR:
        if (value & BIT(7)) {
            s->rtc[ISR] |= BIT(7);
            if (rtc_hz(s)) {
                s->rtc[ISR] |= BIT(6);
            }
        } else {
            s->rtc[ISR] &= ~(BIT(7) | BIT(6));
        }
        s->rtc[ISR] = (s->rtc[ISR] & ~BIT(5)) |
                      (rtc_hz(s) ? BIT(5) : 0);
        break;
    case TR:
    case DR:
    case PRER:
        if (s->rtc[ISR] & BIT(6)) {
            static const uint32_t masks[] = { 0x003f7f7f, 0x00ffff3f };
            s->rtc[reg] = value & (reg == PRER ? 0x007f7fff : masks[reg]);
            s->rtc_fraction_ns = 0;
            if (reg == DR) {
                s->rtc[ISR] |= BIT(4);
            }
        }
        break;
    case RTC_CR:
        /* 24-hour mode only. Alarms/wakeup/interrupts are not implemented. */
        s->rtc[RTC_CR] = value & BIT(5);
        s->rtc_shadow_locked = false;
        if (value & ~BIT(5)) {
            qemu_log_mask(LOG_UNIMP, "f411 RTC: unsupported CR bits\n");
        }
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "f411 RTC: write at 0x%"
                      HWADDR_PRIx "\n", addr);
    }
}

static void update_clocks(STM32F411State *s)
{
    static const unsigned ahb_div[] = { 1, 1, 1, 1, 1, 1, 1, 1,
                                       2, 4, 8, 16, 64, 128, 256, 512 };
    static const unsigned apb_div[] = { 1, 1, 1, 1, 2, 4, 8, 16 };
    uint32_t pll = s->rcc[PLLCFGR];
    unsigned source = s->rcc[CFGR] & 3;
    uint64_t hz = 16000000;
    unsigned m = pll & 63, n = (pll >> 6) & 511;
    unsigned p = (((pll >> 16) & 3) + 1) * 2;
    bool pll_source_ready = (pll & BIT(22)) ?
                           (s->rcc[CR] & BIT(17)) : (s->rcc[CR] & BIT(1));

    s->rcc[CR] &= ~BIT(25);
    if ((s->rcc[CR] & BIT(24)) && pll_source_ready && m >= 2 && n >= 50) {
        s->rcc[CR] |= BIT(25);
    }
    if ((source == 0 && !(s->rcc[CR] & BIT(1))) ||
        (source == 2 && !(s->rcc[CR] & BIT(25))) ||
        (source == 1 && !(s->rcc[CR] & BIT(17))) || source == 3) {
        source = (s->rcc[CFGR] >> 2) & 3;
    }
    if (source == 1) {
        hz = 25000000;
    } else if (source == 2 && m) {
        hz = ((pll & BIT(22)) ? 25000000ULL : 16000000ULL) * n / m / p;
    }
    s->rcc[CFGR] = (s->rcc[CFGR] & ~12U) | source << 2;
    hz /= ahb_div[(s->rcc[CFGR] >> 4) & 15];
    clock_update_hz(s->hclk, hz);
    clock_update_hz(s->refclk, hz / 8);
    clock_update_hz(s->pclk1, hz / apb_div[(s->rcc[CFGR] >> 10) & 7]);
    clock_update_hz(s->pclk2, hz / apb_div[(s->rcc[CFGR] >> 13) & 7]);
}

static void gpio_update(F411GPIO *g)
{
    unsigned pin;
    uint32_t idr = g->inputs;

    for (pin = 0; pin < 16; pin++) {
        unsigned mode = (g->regs[0] >> (pin * 2)) & 3;
        bool level = (g->regs[5] >> pin) & 1;
        if (mode == 1) {
            idr = (idr & ~BIT(pin)) | (level << pin);
        }
        level = mode == 1 && level;
        if (!!(g->driven & BIT(pin)) != level) {
            trace_stm32f411_gpio(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                                pin, level);
        }
        g->driven = (g->driven & ~BIT(pin)) | (level << pin);
        qemu_set_irq(g->pins[pin], level);
    }
    g->regs[4] = idr;
}

static void gpio_reset(F411GPIO *g)
{
    memset(g->regs, 0, sizeof(g->regs));
    g->regs[0] = 0xa8000000;
    g->regs[2] = 0x0c000000;
    g->regs[3] = 0x64000000;
    gpio_update(g);
}

static void gpio_input(void *opaque, int pin, int level)
{
    F411GPIO *g = opaque;
    g->inputs = (g->inputs & ~BIT(pin)) | (!!level << pin);
    gpio_update(g);
}

static uint64_t gpio_read(void *opaque, hwaddr addr, unsigned size)
{
    F411GPIO *g = opaque;
    return addr < sizeof(g->regs) && addr != 0x18 ? g->regs[addr / 4] : 0;
}

static void gpio_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    F411GPIO *g = opaque;
    uint32_t old = g->regs[5];

    if (!(g->soc->rcc[AHB1ENR] & 1) || (g->soc->rcc[AHB1RSTR] & 1)) {
        return;
    }
    switch (addr) {
    case 0: case 8: case 12: case 32: case 36:
        g->regs[addr / 4] = value;
        break;
    case 4: case 20:
        g->regs[addr / 4] = value & 0xffff;
        break;
    case 24:
        g->regs[5] = ((old & ~(value >> 16)) | value) & 0xffff;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "f411 GPIOA: write at 0x%"
                      HWADDR_PRIx "\n", addr);
        return;
    }
    gpio_update(g);

}

static uint64_t rcc_read(void *opaque, hwaddr addr, unsigned size)
{
    STM32F411State *s = opaque;
    return addr < sizeof(s->rcc) ? s->rcc[addr / 4] : 0;
}

static void rcc_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    STM32F411State *s = opaque;
    unsigned reg = addr / 4;
    uint32_t old;

    if (reg >= ARRAY_SIZE(s->rcc)) {
        qemu_log_mask(LOG_UNIMP, "f411 RCC: write at 0x%"
                      HWADDR_PRIx "\n", addr);
        return;
    }
    rtc_sync(s);
    old = s->rcc[reg];
    switch (reg) {
    case CR:
        /* The active system-clock source cannot be switched off. */
        switch ((s->rcc[CFGR] >> 2) & 3) {
        case 0:
            value |= BIT(0);
            break;
        case 1:
            value |= BIT(16);
            break;
        case 2:
            value |= BIT(24) | ((s->rcc[PLLCFGR] & BIT(22)) ?
                               BIT(16) : BIT(0));
            break;
        }
        s->rcc[reg] = (value & ~(BIT(1) | BIT(17) | BIT(25))) |
                     ((value & BIT(0)) ? BIT(1) : 0) |
                     ((value & BIT(16)) ? BIT(17) : 0);
        break;
    case PLLCFGR:
        if (!(s->rcc[CR] & BIT(24))) {
            s->rcc[reg] = value;
        }
        break;
    case CFGR:
        s->rcc[reg] = (value & ~12U) | (old & 12);
        break;
    case CSR:
        s->rcc[reg] = (old & 0xfe000000) | (value & 1) |
                     ((value & 1) ? 2 : 0);
        if (value & BIT(24)) {
            s->rcc[reg] &= ~0xfe000000;
        }
        break;
    case BDCR:
        if (!backup_writable(s)) {
            return;
        }
        if (value & BIT(16)) {
            rtc_backup_reset(s);
            s->rcc[reg] = BIT(16);
        } else {
            /* RTCSEL can only change after backup-domain reset. */
            s->rcc[reg] = value & 0x8305;
            if (old & 0x300) {
                s->rcc[reg] = (s->rcc[reg] & ~0x300) | (old & 0x300);
            }
            if (value & 1) {
                s->rcc[reg] |= 2;
            }
        }
        break;
    case AHB1RSTR:
    case APB1RSTR:
    case AHB1ENR:
    case APB1ENR:
        s->rcc[reg] = value;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "f411 RCC: unsupported register 0x%"
                      HWADDR_PRIx "\n", addr);
        return;
    }
    if (reg == AHB1RSTR && (value & 1)) {
        gpio_reset(&s->gpioa);
    }
    if (reg == APB1RSTR && (value & BIT(17))) {
        device_cold_reset(DEVICE(&s->usart2));
    }
    if (reg == APB1RSTR && (value & BIT(28))) {
        s->pwr[0] = 0x8000;
        s->pwr[1] = 0x4000;
    }
    update_clocks(s);
}

static uint64_t pwr_read(void *opaque, hwaddr addr, unsigned size)
{
    STM32F411State *s = opaque;
    return addr < sizeof(s->pwr) ? s->pwr[addr / 4] : 0;
}

static void pwr_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    STM32F411State *s = opaque;
    if (addr == 0 && (s->rcc[APB1ENR] & BIT(28))) {
        s->pwr[0] = value & 0xc3f3;
        s->pwr[1] |= BIT(14); /* Voltage scaling settles immediately. */
    }
}

static uint64_t flashctl_read(void *opaque, hwaddr addr, unsigned size)
{
    STM32F411State *s = opaque;
    return addr < sizeof(s->flashctl) ? s->flashctl[addr / 4] : 0;
}

static void flashctl_write(void *opaque, hwaddr addr, uint64_t value,
                           unsigned size)
{
    STM32F411State *s = opaque;
    if (addr == 0) {
        s->flashctl[0] = value & 0x1f07;
    } else {
        qemu_log_mask(LOG_UNIMP, "f411 flash programming is not implemented\n");
    }
}

#define F411_OPS(name) \
    static const MemoryRegionOps name##_ops = { \
        .read = name##_read, .write = name##_write, \
        .endianness = DEVICE_LITTLE_ENDIAN, \
        .valid = { .min_access_size = 4, .max_access_size = 4 }, \
    }
F411_OPS(rcc);
F411_OPS(pwr);
F411_OPS(flashctl);
F411_OPS(rtc);
F411_OPS(gpio);

static void f411_reset(DeviceState *dev)
{
    STM32F411State *s = STM32F411_SOC(dev);
    uint32_t bdcr = s->rcc[BDCR];

    rtc_sync(s);
    memset(s->rcc, 0, sizeof(s->rcc));
    s->rcc[CR] = 0x83;
    s->rcc[PLLCFGR] = 0x24003010;
    s->rcc[CSR] = 0x0e000000;
    s->rcc[BDCR] = bdcr;
    s->pwr[0] = 0x8000;
    s->pwr[1] = 0x4000;
    memset(s->flashctl, 0, sizeof(s->flashctl));
    s->flashctl[4] = BIT(31);
    s->flashctl[5] = 0x0fffaaed;
    gpio_reset(&s->gpioa);
    update_clocks(s);
}

static void f411_init(Object *obj)
{
    STM32F411State *s = STM32F411_SOC(obj);

    object_initialize_child(obj, "armv7m", &s->armv7m, TYPE_ARMV7M);
    object_initialize_child(obj, "usart2", &s->usart2, TYPE_STM32F2XX_USART);
    s->hclk = qdev_init_clock_out(DEVICE(obj), "hclk");
    s->refclk = qdev_init_clock_out(DEVICE(obj), "refclk");
    s->pclk1 = qdev_init_clock_out(DEVICE(obj), "pclk1");
    s->pclk2 = qdev_init_clock_out(DEVICE(obj), "pclk2");
    clock_set_hz(s->hclk, 16000000);
    clock_set_hz(s->refclk, 2000000);
    s->gpioa.soc = s;
    qdev_init_gpio_out_named(DEVICE(obj), s->gpioa.pins, "gpioa-out", 16);
    qdev_init_gpio_in_named_with_opaque(DEVICE(obj), gpio_input,
                                      &s->gpioa, "gpioa-in", 16);
    rtc_backup_reset(s);
}

static void f411_realize(DeviceState *dev, Error **errp)
{
    STM32F411State *s = STM32F411_SOC(dev);
    MemoryRegion *mem = get_system_memory();
    DeviceState *cpu = DEVICE(&s->armv7m);

    memory_region_init_rom(&s->flash, OBJECT(s), "f411.flash", F411_FLASH_SIZE,
                           &error_fatal);
    memory_region_init_alias(&s->flash_alias, OBJECT(s), "f411.boot",
                             &s->flash, 0, F411_FLASH_SIZE);
    memory_region_init_ram(&s->sram, OBJECT(s), "f411.sram", F411_SRAM_SIZE,
                           &error_fatal);
    memory_region_add_subregion(mem, 0x08000000, &s->flash);
    memory_region_add_subregion(mem, 0, &s->flash_alias);
    memory_region_add_subregion(mem, 0x20000000, &s->sram);
    qdev_prop_set_string(cpu, "cpu-type", ARM_CPU_TYPE_NAME("cortex-m4"));
    qdev_prop_set_uint32(cpu, "num-irq", 86);
    qdev_prop_set_uint8(cpu, "num-prio-bits", 4);
    qdev_prop_set_bit(cpu, "enable-bitband", true);
    qdev_connect_clock_in(cpu, "cpuclk", s->hclk);
    qdev_connect_clock_in(cpu, "refclk", s->refclk);
    object_property_set_link(OBJECT(cpu), "memory", OBJECT(mem), &error_abort);
    if (!sysbus_realize(SYS_BUS_DEVICE(cpu), errp)) {
        return;
    }
    qdev_prop_set_chr(DEVICE(&s->usart2), "chardev", serial_hd(0));
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->usart2), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->usart2), 0, 0x40004400);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->usart2), 0,
                       qdev_get_gpio_in(cpu, 38));
#define MAP(region, ops, opaque, base, label) do { \
    memory_region_init_io(&(region), OBJECT(s), &(ops), (opaque), \
                          label, 0x400); \
    memory_region_add_subregion(mem, base, &(region)); \
} while (0)
    MAP(s->rcc_mmio, rcc_ops, s, 0x40023800, "f411.rcc");
    MAP(s->pwr_mmio, pwr_ops, s, 0x40007000, "f411.pwr");
    MAP(s->flashctl_mmio, flashctl_ops, s, 0x40023c00, "f411.flashctl");
    MAP(s->rtc_mmio, rtc_ops, s, 0x40002800, "f411.rtc");
    MAP(s->gpioa.mmio, gpio_ops, &s->gpioa, 0x40020000, "f411.gpioa");
#undef MAP
}

/* This experimental subset deliberately rejects migration/snapshots. */
static const VMStateDescription f411_vmstate = {
    .name = TYPE_STM32F411_SOC,
    .unmigratable = 1,
};

static void f411_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = f411_realize;
    device_class_set_legacy_reset(dc, f411_reset);
    dc->vmsd = &f411_vmstate;
}

static const TypeInfo f411_type = {
    .name = TYPE_STM32F411_SOC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(STM32F411State),
    .instance_init = f411_init,
    .class_init = f411_class_init,
};

static void f411_register(void)
{
    type_register_static(&f411_type);
}
type_init(f411_register)
