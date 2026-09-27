Black Pill STM32F411CE (local experiment)
========================================

``blackpill-f411ce`` is a local, partial STM32F411CE microcontroller model.
It is not a complete Black Pill board simulation. It does not change existing
STM32 machines. The implementation and this documentation are AI-assisted local experimental work, not an upstream
contribution.

Build and run
-------------

Use a checkout containing the ``blackpill-f411ce`` machine. This experimental
model is not included in standard QEMU releases.

Ubuntu prerequisites
~~~~~~~~~~~~~~~~~~~~

On a current Ubuntu release, install the host compiler and development packages::

  sudo apt update
  sudo apt install build-essential git ninja-build pkg-config \
      python3 python3-venv libglib2.0-dev libpixman-1-dev zlib1g-dev \
      libfdt-dev bison flex

These packages support the ARM system-emulator build below. Additional QEMU
features may need more dependencies. See the `QEMU build-environment guide
<https://www.qemu.org/docs/master/devel/build-environment.html>`_ for broader
builds. The first configuration may need Internet access to obtain build tools
or subprojects. QEMU is compiled for the host; an Arm bare-metal cross compiler
is only required separately when compiling guest firmware.

Configure, build and run
~~~~~~~~~~~~~~~~~~~~~~~~

From the QEMU repository root::

  mkdir -p build
  cd build
  ../configure --target-list=arm-softmmu --disable-docs
  ninja
  ./qemu-system-arm -machine help | grep blackpill-f411ce
  ./qemu-system-arm -M blackpill-f411ce -kernel /path/to/firmware.elf \
      -display none -monitor none -serial stdio

Replace ``/path/to/firmware.elf`` with an absolute path to your firmware.
The executable is ``build/qemu-system-arm`` relative to the repository root;
no system installation is required. Rebuild from the repository root with::

  ninja -C build

Use a fresh build directory when changing host platforms. The experimental
model has been tested on macOS; an Ubuntu build has not yet been verified.

macOS differences
~~~~~~~~~~~~~~~~~

On macOS 27, add ``--disable-pvg`` to the configure command because the existing
Apple graphics device uses removed SDK APIs. Ubuntu does not need this option.
On Homebrew, configuration/regeneration may require
``PATH="/opt/homebrew/opt/bison/bin:$PATH"`` to select Bison 3 instead of the
older macOS Bison. ``pkgconf``, GLib, Pixman, Python and Ninja are also needed.

Firmware console
~~~~~~~~~~~~~~~~

The first serial backend is **USART2**, whose pins are PA2/PA3.
Serial transport does not model baud-rate timing or GPIO alternate-function
routing. Raw ``.bin`` images are also accepted by ``-kernel``. The firmware
runs using TCG; Apple's HVF does not accelerate Cortex-M guests.

Debugging and GPIO traces
------------------------

Append ``-S -gdb tcp:127.0.0.1:1234`` to stop before boot. In an Arm-aware GDB::

  file /path/to/firmware.elf
  target remote 127.0.0.1:1234
  break main
  continue

Append ``-trace enable=stm32f411_gpio -D gpio.log`` to observe GPIOA output
transitions. Each event records QEMU virtual nanoseconds, the pin number,
and the level. Only GPIOA is implemented; the usual onboard PC13 LED is
not modeled. There is no graphical board view.

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
  Reset starts at HSI. Guest firmware selects PLL parameters and prescalers.
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

The control-register MMIO handlers accept aligned 32-bit accesses. Firmware
must operate within the supported peripheral subset.
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

The qtests exercise memory mapping, clocks/SysTick, GPIO, and RTC behavior
without an external firmware project. Build and test application firmware in
its own repository, passing an ELF or BIN image through ``-kernel``.
