*****************************************************************************
** ChibiOS/RT port for the T3 Gemstone O1 (TI AM67A/J722S) Cortex-R5F.    **
*****************************************************************************

** TARGET **

The demo runs on the Cortex-R5F (WKUP R5FSS0 core 0) of the T3 Gemstone O1
board, loaded through the Linux k3-r5 remoteproc driver running on the A53
cores.

** THE DEMO **

The demo spawns a counter thread and, when the FPU is enabled, an FPU
context-validation thread, while the main thread logs the counters once per
second into the RemoteProc trace buffer.

Memory layout (device addresses, must match the host device tree carveouts):

  0x00000000  ATCM   32K  exception vectors (boot vector)
  0x41010000  BTCM   32K  banked CPU mode stacks
  0xA2100000  DDR     4K  remoteproc resource table
  0xA2110000  DDR    16K  remoteproc trace buffer
  0xA2240000  DDR   ~14M  code, data, bss, heap

The OS tick comes from DMTIMER0 (0x02400000, 25 MHz HFOSC0, VIM IRQ 24),
interrupts are dispatched through the TI VIM controller (0x2FFF0000).

** BUILD INSTRUCTIONS **

The demo needs an arm-none-eabi GCC toolchain on the PATH:

  make

The firmware image is build/chibios-gemstone-o1-r5f.elf.

** DEPLOY INSTRUCTIONS (on the board's Linux) **

Copy the ELF to the board, then:

  # find the R5F remoteproc instance
  head /sys/class/remoteproc/remoteproc*/name

  RP=/sys/class/remoteproc/remoteprocN          # N from the step above
  cp chibios-gemstone-o1-r5f.elf /lib/firmware/
  echo stop > $RP/state 2>/dev/null || true
  echo chibios-gemstone-o1-r5f.elf > $RP/firmware
  echo start > $RP/state

  # watch the ChibiOS log
  cat /sys/kernel/debug/remoteproc/remoteprocN/trace0

Expected output:

  ChibiOS/RT on T3 Gemstone O1 R5F
  port: ARMv7-R, core: ARM Cortex-R5
  kernel started, tick at 1000 Hz
  alive: main=1 thread=10 fpu=100 fpu_errors=0
  ...

The alive line repeats once per second, thread counts 10x main, fpu 100x,
fpu_errors must stay 0.

** TROUBLESHOOTING **

- "start" fails: check dmesg, the reserved-memory carveouts for the R5F in
  the device tree must cover 0xA2100000-0xA21FFFFF and 0xA2240000 onward.
- trace0 missing or empty: the resource table was not accepted, check that
  the .resource_table section is present in the ELF (readelf -S).
- No "alive" lines but the header prints: tick interrupt not firing, check
  that DMTIMER0 is not claimed/gated by the host (its clock must be left
  running by the bootloader, IRQ 24 at the R5F VIM).

** NOTES **

- The D-cache is intentionally left disabled during bring-up (board.c),
  enable it together with cache maintenance once shared-memory paths are
  audited. The I-cache and branch prediction are enabled.
- The MPU is configured with the same region layout as the NuttX am67 port
  plus a non-cacheable window for the remoteproc shared data.
