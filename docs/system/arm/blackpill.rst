Black Pill STM32F411CE (local experiment)
========================================

``blackpill-f411ce`` is a local, partial STM32F411CE model intended to run
``demo411`` FreeRTOS firmware unchanged. It is not a complete Black Pill board
simulation. It does not change existing STM32 machines. The implementation and
this documentation are AI-assisted local experimental work, not an upstream
contribution.

Build and run
-------------

Build QEMU with ``--target-list=arm-softmmu``. On macOS 27, configure with
``--disable-pvg`` because the existing Apple graphics device uses removed SDK
APIs. Documentation generation can be disabled with ``--disable-docs``.

From the QEMU source directory::

  mkdir -p build
  cd build
  ../configure --target-list=arm-softmmu --disable-pvg --disable-docs
  ninja -j 10
  ./qemu-system-arm -M blackpill-f411ce -kernel /path/to/DemoRTOSProject.elf \
      -display none -monitor none -serial stdio

On Homebrew, configuration/regeneration may require
``PATH="/opt/homebrew/opt/bison/bin:$PATH"`` to select Bison 3 instead of the
older macOS Bison. ``pkgconf``, GLib, Pixman, Python and Ninja are also needed.

To keep the firmware build and launcher in the same worktree, from the QEMU
source directory::

  cmake -S /path/to/demo411 -B build/demo411 -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=/path/to/demo411/cmake/toolchain_arm.cmake
  cmake --build build/demo411 -j 10
  scripts/blackpill/run-demo411.sh

The launcher locates QEMU relative to itself and defaults to
``build/demo411/DemoRTOSProject.elf``. An optional first argument selects a
different firmware image; subsequent arguments are passed to QEMU.

The first serial backend is **USART2**, matching the firmware's PA2/PA3 UART.
Serial transport does not model baud-rate timing or GPIO alternate-function
routing. Raw ``.bin`` images are also accepted by ``-kernel``. The firmware
runs using TCG; Apple's HVF does not accelerate Cortex-M guests.

Type ``?`` for the menu, ``TS?`` to read time, ``TS123456`` to set 12:34:56,
and ``TD?`` to read the date. The current demo411 date-setting parser uses
incorrect pointer types with ``sscanf``; successful execution on one build
is not proof that this firmware defect is harmless. No firmware changes are
part of this QEMU experiment.

Debugging and GPIO traces
------------------------

Append ``-S -gdb tcp:127.0.0.1:1234`` to stop before boot. In an Arm-aware GDB::

  file /path/to/DemoRTOSProject.elf
  target remote 127.0.0.1:1234
  break main
  continue

Append ``-trace enable=stm32f411_gpio -D gpio.log`` to observe GPIOA output
transitions. Each event records QEMU virtual nanoseconds, the pin number,
and the level. ``demo411`` toggles PA6 every 1500 ms and PA7 every 800 ms;
it does not use the usual onboard PC13 LED. There is no graphical board view.

For deterministic timing tests, use ``-icount shift=7,sleep=off``. Virtual time
then runs independently of host wall time. This is useful for testing delays,
but is not cycle-accurate CPU timing. Ordinary interactive runs can omit it.
Add ``-d unimp,guest_errors`` when investigating unsupported device accesses.

Implemented subset
------------------

* Cortex-M4F, 86 external interrupt slots, four priority bits, SysTick,
  bit-banding, and normal flash boot. Flash is 512 KiB at ``0x08000000`` with
  a boot alias at zero. SRAM is 128 KiB at ``0x20000000``. There is no CCM RAM.
* RCC: HSI (16 MHz), a simplified fixed 25 MHz HSE, main PLL, clock selection,
  AHB/APB prescalers, GPIOA/USART2/PWR resets, and the LSI/LSE RTC clock paths.
  Oscillator/PLL ready flags settle immediately. HCLK/refclk drive the CPU
  and SysTick. APB clock outputs are calculated; UART transport is untimed.
  Reset starts at HSI; demo411 selects 100 MHz HCLK and 50 MHz APB1.
* GPIOA: mode, speed, pull configuration and alternate-function register
  storage; input/output data; atomic set/reset; output signals and traces.
  Output mode reflects the output latch into IDR. External input signals are
  exposed as ``/machine/soc`` GPIOs ``gpioa-in``; outputs are ``gpioa-out``.
  Pull resistors, open-drain electrical behavior, peripheral pin muxing and
  locking are not simulated. GPIO writes require its RCC enable and respect
  peripheral reset.
* USART2 at ``0x40004400``, IRQ 38, using the existing STM32F2xx USART device.
  Its existing synchronous transmit/RXNE behavior is retained. RCC enable
  gating, baud timing and a held-reset bus interface are not modeled for UART.
* PWR voltage-scaling and backup-write-access controls needed by startup;
  flash access-control register storage. Voltage scaling settles immediately.
* RTC: 24-hour BCD calendar, weekday progression, leap years in 2000--2099,
  prescalers, subseconds, initialization, write protection, coherent shadow
  reads, and 20 backup registers. LSI is nominally 32 kHz; LSE is 32768 Hz.
  Calendar progression uses QEMU virtual time, not the host date/time. The
  reset date is 2000-01-01; the weekday register has the hardware reset value.
  Oscillator stabilization and RTC synchronization delays are simplified.
  System reset preserves RTC/backup state and clears LSI enable; backup-domain
  reset clears the RTC. State is not persisted across QEMU processes.

The new control-register MMIO handlers accept aligned 32-bit accesses, matching
this firmware. They are not general-purpose models for arbitrary firmware.
Other GPIO ports, external board devices, USB, DMA, I2C, SPI, general-purpose
timers, ADC, watchdogs, RTC alarms/wakeup/interrupts, RTC HSE clocking, flash
programming, boot ROM, boot-pin remapping and low-power modes are unsupported.
Migration and snapshots are explicitly blocked instead of saving incomplete
peripheral state. No physical-board or cycle-accuracy validation is claimed.

Implementation and tests
------------------------

``hw/arm/blackpill.c`` defines the board. ``hw/arm/stm32f411_soc.c`` and its
header compose the ARMv7-M core and USART2 with the local RCC/GPIO/RTC/PWR/flash
subset. These controllers are kept inside this SoC for the experiment rather
than modifying shared STM32 devices. Their reference is ST RM0383 and the
vendor's ``stm32f411xe.h`` CMSIS definitions:

* https://www.st.com/resource/en/reference_manual/dm00119316.pdf
* https://github.com/STMicroelectronics/cmsis-device-f4/blob/master/Include/stm32f411xe.h

Run the register tests and neighboring STM32 regressions from ``build``::

  pyvenv/bin/meson test --print-errorlogs 'qtest-arm/stm32*'

From the source directory, run the firmware integration test::

  python3 scripts/blackpill/test-demo411.py \
      --qemu build/qemu-system-arm \
      --firmware /path/to/DemoRTOSProject.elf \
      --output build/demo411-test

The integration harness uses only Python's standard library. It checks the
banner, serial menu, advancing RTC, time/date commands, GPIO virtual-time
periods, and system reset with RTC preservation. It leaves ``serial.log``,
``qemu.log`` and ``report.json`` in the selected output directory. It requires
the default QEMU log tracing backend. Repeat with a ``.bin`` image to test
raw-binary loading. The application source is not part of this repository;
pass the separately built ELF or BIN explicitly.
