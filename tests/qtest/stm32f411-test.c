/* Local F411 register tests. SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "libqtest.h"

#define RCC 0x40023800
#define RTC 0x40002800
#define PWR 0x40007000
#define GPIO 0x40020000
#define BIT(n) (1U << (n))

static QTestState *start(void)
{
    return qtest_init("-M blackpill-f411ce -display none -serial null");
}

static void clocks(void)
{
    QTestState *s = start();
    uint32_t count;
    uint32_t pll = 16 | (400 << 6) | BIT(16) | (8 << 24);

    g_assert_cmphex(qtest_readl(s, RCC) & 3, ==, 3);
    /* A request to use an unlocked PLL must not report PLL as active. */
    qtest_writel(s, RCC + 8, 2);
    g_assert_cmphex(qtest_readl(s, RCC + 8) & 12, ==, 0);
    qtest_writel(s, RCC + 4, pll);
    qtest_writel(s, RCC, BIT(24) | 1);
    g_assert_cmphex(qtest_readl(s, RCC) & BIT(25), ==, BIT(25));
    qtest_writel(s, RCC + 8, (4 << 10) | 2);
    g_assert_cmphex(qtest_readl(s, RCC + 8) & 15, ==, 10);
    /* SysTick counting proves the clock actually reaches the CPU. */
    qtest_writel(s, 0xe000e014, 999999);
    qtest_writel(s, 0xe000e018, 0);
    qtest_writel(s, 0xe000e010, 5);
    qtest_clock_step(s, 1000000);
    count = qtest_readl(s, 0xe000e018);
    g_assert_cmpuint(count, >=, 899998);
    g_assert_cmpuint(count, <=, 900001);
    qtest_writel(s, RCC, 0);
    g_assert_cmphex(qtest_readl(s, RCC) & BIT(25), ==, BIT(25));
    qtest_system_reset(s);
    g_assert_cmphex(qtest_readl(s, RCC + 8), ==, 0);
    g_assert_cmphex(qtest_readl(s, RCC) & BIT(25), ==, 0);
    qtest_quit(s);
}

static void gpio(void)
{
    QTestState *s = start();

    qtest_irq_intercept_out_named(s, "/machine/soc", "gpioa-out");
    qtest_writel(s, GPIO + 20, BIT(6));
    g_assert_cmphex(qtest_readl(s, GPIO + 20), ==, 0);
    qtest_writel(s, RCC + 0x30, 1);
    qtest_writel(s, GPIO, (1 << 12) | (1 << 14));
    qtest_writel(s, GPIO + 24, BIT(6) | BIT(7));
    g_assert_cmphex(qtest_readl(s, GPIO + 20), ==, BIT(6) | BIT(7));
    g_assert_cmphex(qtest_readl(s, GPIO + 16) & 0xc0, ==, 0xc0);
    g_assert_true(qtest_get_irq(s, 6));
    g_assert_true(qtest_get_irq(s, 7));
    qtest_writel(s, GPIO + 24, BIT(22));
    g_assert_cmphex(qtest_readl(s, GPIO + 20), ==, BIT(7));
    /* Set wins when both BSRR halves target the same bit. */
    qtest_writel(s, GPIO + 24, BIT(23) | BIT(7));
    g_assert_cmphex(qtest_readl(s, GPIO + 20), ==, BIT(7));
    qtest_writel(s, RCC + 0x10, 1);
    g_assert_cmphex(qtest_readl(s, GPIO + 20), ==, 0);
    qtest_writel(s, GPIO + 24, BIT(6));
    g_assert_cmphex(qtest_readl(s, GPIO + 20), ==, 0);
    qtest_quit(s);
}

static void rtc_enable(QTestState *s)
{
    qtest_writel(s, RCC + 0x40, BIT(28));
    qtest_writel(s, PWR, BIT(8));
    qtest_writel(s, RCC + 0x74, 1);
    g_assert_cmphex(qtest_readl(s, RCC + 0x74) & 3, ==, 3);
    qtest_writel(s, RCC + 0x70, BIT(15) | (2 << 8));
    qtest_writel(s, RTC + 0x24, 0xca);
    qtest_writel(s, RTC + 0x24, 0x53);
}

static void rtc_set(QTestState *s, uint32_t tr, uint32_t dr)
{
    qtest_writel(s, RTC + 0x0c, BIT(7));
    g_assert_cmphex(qtest_readl(s, RTC + 0x0c) & BIT(6), ==, BIT(6));
    qtest_writel(s, RTC + 0x10, (127 << 16) | 249);
    qtest_writel(s, RTC, tr);
    qtest_writel(s, RTC + 4, dr);
    qtest_writel(s, RTC + 0x0c, 0);
}

static void calendar(void)
{
    QTestState *s = start();

    rtc_enable(s);
    rtc_set(s, 0x235959, 0x242228); /* 2024-02-28, weekday field incidental */
    qtest_clock_step(s, 1000000000);
    g_assert_cmphex(qtest_readl(s, RTC), ==, 0);
    g_assert_cmphex(qtest_readl(s, RTC + 4) & 0xff1f3f, ==, 0x240229);
    rtc_set(s, 0x235959, 0x242229);
    qtest_clock_step(s, 1000000000);
    g_assert_cmphex(qtest_readl(s, RTC + 4) & 0xff1f3f, ==, 0x240301);
    rtc_set(s, 0x235959, 0x252228);
    qtest_clock_step(s, 1000000000);
    g_assert_cmphex(qtest_readl(s, RTC + 4) & 0xff1f3f, ==, 0x250301);
    rtc_set(s, 0x235959, 0x253231);
    qtest_clock_step(s, 1000000000);
    g_assert_cmphex(qtest_readl(s, RTC + 4) & 0xff1f3f, ==, 0x260101);
    qtest_quit(s);
}

static void rtc_protection_shadow_reset(void)
{
    QTestState *s = start();
    uint32_t old_date;

    rtc_enable(s);
    rtc_set(s, 0x235959, 0x243231);
    qtest_writel(s, RTC + 0x50, 0x12345677);
    /* TR locks the date shadow until DR is read. */
    g_assert_cmphex(qtest_readl(s, RTC), ==, 0x235959);
    qtest_clock_step(s, 1000000000);
    old_date = qtest_readl(s, RTC + 4);
    g_assert_cmphex(old_date & 0xff1f3f, ==, 0x241231);
    g_assert_cmphex(qtest_readl(s, RTC + 4) & 0xff1f3f, ==, 0x250101);
    qtest_writel(s, RTC + 0x24, 0xff);
    qtest_writel(s, RTC + 0x0c, BIT(7));
    g_assert_cmphex(qtest_readl(s, RTC + 0x0c) & BIT(7), ==, 0);
    qtest_system_reset(s);
    g_assert_cmphex(qtest_readl(s, RTC + 0x50), ==, 0x12345677);
    g_assert_cmphex(qtest_readl(s, RCC + 0x74) & 3, ==, 0);
    qtest_clock_step(s, 2000000000);
    g_assert_cmphex(qtest_readl(s, RTC), ==, 0);
    qtest_readl(s, RTC + 4);
    rtc_enable(s);
    qtest_clock_step(s, 1000000000);
    g_assert_cmphex(qtest_readl(s, RTC), ==, 1);
    qtest_readl(s, RTC + 4);
    qtest_writel(s, RCC + 0x70, BIT(16));
    g_assert_cmphex(qtest_readl(s, RTC + 0x50), ==, 0);
    g_assert_cmphex(qtest_readl(s, RTC + 4), ==, 0x2101);
    qtest_quit(s);
}


static void memory(void)
{
    g_autofree char *path = NULL;
    uint8_t image[0x104] = { [0x100] = 0x78, 0x56, 0x34, 0x12 };
    int fd = g_file_open_tmp("f411-test-XXXXXX", &path, NULL);
    QTestState *s;

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, image, sizeof(image)), ==, sizeof(image));
    close(fd);
    s = qtest_initf("-M blackpill-f411ce -display none -serial null "
                    "-kernel '%s'", path);
    unlink(path);

    g_assert_cmphex(qtest_readl(s, 0x08000100), ==, 0x12345678);
    g_assert_cmphex(qtest_readl(s, 0x00000100), ==, 0x12345678);
    qtest_writel(s, 0x20000000, 0xaabbccdd);
    qtest_writel(s, 0x2001fffc, 0x87654321);
    g_assert_cmphex(qtest_readl(s, 0x20000000), ==, 0xaabbccdd);
    g_assert_cmphex(qtest_readl(s, 0x2001fffc), ==, 0x87654321);
    qtest_quit(s);
}

static void rtc_stopped_clock(void)
{
    QTestState *s = start();

    rtc_enable(s);
    qtest_writel(s, RCC + 0x74, 0);
    qtest_writel(s, RTC + 0x0c, BIT(7));
    g_assert_cmphex(qtest_readl(s, RTC + 0x0c) & BIT(6), ==, 0);
    qtest_writel(s, RCC + 0x74, 1);
    g_assert_cmphex(qtest_readl(s, RTC + 0x0c) & BIT(6), ==, BIT(6));
    rtc_set(s, 0, 0x242101);
    qtest_clock_step(s, 500000000);
    g_assert_cmpuint(qtest_readl(s, RTC + 0x28), ==, 124);
    qtest_readl(s, RTC + 4); /* Unlock the shadow. */
    qtest_writel(s, RCC + 0x70, 2 << 8); /* Gate the RTC clock. */
    qtest_clock_step(s, 10000000000LL);
    g_assert_cmphex(qtest_readl(s, RTC), ==, 0);
    qtest_readl(s, RTC + 4);
    qtest_writel(s, RCC + 0x70, (2 << 8) | BIT(15));
    qtest_clock_step(s, 500000000);
    g_assert_cmphex(qtest_readl(s, RTC), ==, 1);
    qtest_readl(s, RTC + 4);
    qtest_writel(s, RTC + 0x50, 42);
    qtest_writel(s, PWR, 0);
    qtest_writel(s, RTC + 0x50, 43);
    g_assert_cmpuint(qtest_readl(s, RTC + 0x50), ==, 42);
    qtest_writel(s, PWR, BIT(8));
    qtest_writel(s, RCC + 0x70, BIT(16));
    qtest_writel(s, RTC + 0x50, 44);
    g_assert_cmpuint(qtest_readl(s, RTC + 0x50), ==, 0);
    qtest_quit(s);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/stm32f411/memory", memory);
    qtest_add_func("/stm32f411/clocks", clocks);
    qtest_add_func("/stm32f411/rtc-stopped-clock", rtc_stopped_clock);
    qtest_add_func("/stm32f411/gpio", gpio);
    qtest_add_func("/stm32f411/calendar", calendar);
    qtest_add_func("/stm32f411/rtc-protection-shadow-reset",
                   rtc_protection_shadow_reset);
    return g_test_run();
}
