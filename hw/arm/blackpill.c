/* Local Black Pill F411CE model. SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/boards.h"
#include "hw/arm/boot.h"
#include "hw/arm/stm32f411_soc.h"

static void blackpill_init(MachineState *machine)
{
    DeviceState *dev = qdev_new(TYPE_STM32F411_SOC);

    object_property_add_child(OBJECT(machine), "soc", OBJECT(dev));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    armv7m_load_kernel(STM32F411_SOC(dev)->armv7m.cpu,
                      machine->kernel_filename, 0, F411_FLASH_SIZE);
}

static void blackpill_class_init(MachineClass *mc)
{
    static const char * const cpus[] = { ARM_CPU_TYPE_NAME("cortex-m4"), NULL };

    mc->desc = "Black Pill STM32F411CE (local experimental subset)";
    mc->init = blackpill_init;
    mc->valid_cpu_types = cpus;
}

DEFINE_MACHINE("blackpill-f411ce", blackpill_class_init)
