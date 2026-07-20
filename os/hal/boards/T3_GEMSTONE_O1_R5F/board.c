/*
    ChibiOS - Copyright (C) 2006-2026 Giovanni Di Sirio.

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

        http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
*/

/**
 * @file    board.c
 * @brief   T3 Gemstone O1 (AM67A/J722S) R5F early initialization.
 * @details MPU region layout mirrors the proven NuttX am67 port, with one
 *          additional non-cacheable window covering the RemoteProc resource
 *          table and trace buffer so that the Linux host always sees fresh
 *          data once the D-cache gets enabled.
 *
 *          MPU region priority grows with the region number, later regions
 *          override earlier ones on overlap.
 */

#include <stdint.h>

#include "board.h"

/* DRSR is (size_exponent - 1) << 1 | enable, region size is 2^exponent.*/
#define MPU_SIZE_32K            ((14U << 1) | 1U)
#define MPU_SIZE_512K           ((18U << 1) | 1U)
#define MPU_SIZE_1M             ((19U << 1) | 1U)
#define MPU_SIZE_2G             ((30U << 1) | 1U)

/* DRACR fields: B[0], C[1], S[2], TEX[5:3], AP[10:8], XN[12].*/
#define MPU_AP_RWRW             (3U << 8)
#define MPU_XN                  (1U << 12)
#define MPU_STRONGLY_ORDERED    0U
#define MPU_NORMAL_WBWA         ((1U << 3) | (1U << 1) | (1U << 0))
#define MPU_NORMAL_NONCACHE     (1U << 3)
#define MPU_SHARED              (1U << 2)

#define SCTLR_M                 (1U << 0)
#define SCTLR_C                 (1U << 2)
#define SCTLR_Z                 (1U << 11)
#define SCTLR_I                 (1U << 12)
#define SCTLR_BR                (1U << 17)

/* Bring-up boot markers, written into the top of the trace buffer so they
   can be read from the Linux host over /dev/mem while debugging a silent
   boot. Slots (word offsets from AM67_BOOTMARK_BASE):
     +0x00 tcm_early_init entered    +0x04 MPU programmed
     +0x08 caches enabled            +0x0C first DDR instruction
     +0x10 __cpu_init reached        +0x14 __late_init reached
     +0x20 fault type                +0x24 fault LR
     +0x28 fault status (xFSR)       +0x2C fault address (xFAR)
     +0x40 DDR store witness
   The log area of the trace buffer ends below this block, trace_init()
   never clears it (see trace.c).*/
#define AM67_BOOTMARK_BASE      (AM67_TRACEBUF_BASE + AM67_TRACEBUF_SIZE - 0x100U)

static inline void boot_mark(uint32_t slot, uint32_t value) {

  *(volatile uint32_t *)(AM67_BOOTMARK_BASE + slot) = value;
  __asm volatile ("dsb" ::: "memory");
}

static inline __attribute__((always_inline)) uint32_t sctlr_read(void) {
  uint32_t value;

  __asm volatile ("mrc p15, 0, %0, c1, c0, 0" : "=r" (value));
  return value;
}

static inline __attribute__((always_inline)) void sctlr_write(uint32_t value) {

  __asm volatile ("mcr p15, 0, %0, c1, c0, 0" :: "r" (value) : "memory");
  __asm volatile ("dsb; isb" ::: "memory");
}

static inline __attribute__((always_inline))
void mpu_set_region(uint32_t region, uint32_t base,
                    uint32_t size, uint32_t access) {

  __asm volatile ("mcr p15, 0, %0, c6, c2, 0" :: "r" (region));  /* RGNR  */
  __asm volatile ("mcr p15, 0, %0, c6, c1, 0" :: "r" (base));    /* DRBAR */
  __asm volatile ("mcr p15, 0, %0, c6, c1, 4" :: "r" (access));  /* DRACR */
  __asm volatile ("mcr p15, 0, %0, c6, c1, 2" :: "r" (size));    /* DRSR  */
}

/* MPU and cache setup MUST execute from TCM: in the ARMv7-R default memory
   map (MPU disabled) the upper 2GB is Execute-Never, so DDR code cannot
   run until the MPU is enabled, and reconfiguring the MPU from DDR would
   fault the moment it gets disabled.*/
__attribute__((section(".tcm_probe")))
static void mpu_init(void) {
  uint32_t region;

  /* MPU off and background region disabled while reconfiguring.*/
  sctlr_write(sctlr_read() & ~(SCTLR_M | SCTLR_BR));

  for (region = 0U; region < 16U; region++) {
    mpu_set_region(region, 0U, 0U, 0U);
  }

  /* Region 0: SoC registers and everything below 2GB, strongly-ordered.*/
  mpu_set_region(0U, 0x00000000U, MPU_SIZE_2G,
                 MPU_STRONGLY_ORDERED | MPU_SHARED | MPU_AP_RWRW);

  /* Region 1: TCM, normal write-back write-allocate. Only the block at
     address 0 exists on this core, see board.h.*/
  mpu_set_region(1U, AM67_TCM_BASE, MPU_SIZE_32K,
                 MPU_NORMAL_WBWA | MPU_AP_RWRW);

  /* Region 2: MCU MSRAM, normal write-back write-allocate.*/
  mpu_set_region(2U, AM67_MSRAM_BASE, MPU_SIZE_512K,
                 MPU_NORMAL_WBWA | MPU_AP_RWRW);

  /* Region 3: DDR, normal write-back write-allocate, shareable.*/
  mpu_set_region(3U, AM67_DDR_BASE, MPU_SIZE_2G,
                 MPU_NORMAL_WBWA | MPU_SHARED | MPU_AP_RWRW);

  /* Region 4: resource table and trace buffer window, non-cacheable so
     the Linux host sees coherent data, never executable.*/
  mpu_set_region(4U, AM67_RSCTABLE_BASE, MPU_SIZE_1M,
                 MPU_NORMAL_NONCACHE | MPU_SHARED | MPU_AP_RWRW | MPU_XN);

  sctlr_write(sctlr_read() | SCTLR_M);
}

__attribute__((section(".tcm_probe")))
static void caches_init(void) {

  /* Invalidate instruction cache and branch predictor.*/
  __asm volatile ("mcr p15, 0, %0, c7, c5, 0" :: "r" (0));
  __asm volatile ("mcr p15, 0, %0, c7, c5, 6" :: "r" (0));
  __asm volatile ("dsb; isb" ::: "memory");

  /* Instruction cache and branch prediction on. The data cache is left
     disabled for bring-up, enable SCTLR_C here once cache maintenance is
     in place for the shared-memory paths.*/
  sctlr_write(sctlr_read() | SCTLR_I | SCTLR_Z);
}

/*
 * TCM-resident early initialization: called from Reset_Handler on a
 * temporary stack at the top of BTCM, before any DDR code has executed.
 */
__attribute__((section(".tcm_probe"), noinline, used))
void tcm_early_init(void) {

  boot_mark(0x00U, 0xB0070001U);
  mpu_init();
  boot_mark(0x04U, 0xB0070002U);
  caches_init();
  boot_mark(0x08U, 0xB0070003U);
}

/*
 * Boot entry, placed in TCM: in the ARMv7-R default memory map (MPU off)
 * DDR is Execute-Never, so everything up to and including the MPU enable
 * must run from TCM. Reset flow:
 *   vectors (TCM) -> Reset_Handler (TCM) -> tcm_early_init (TCM, enables
 *   MPU) -> ddr_reset (DDR) -> _crt0_entry.
 * Witness markers written before any DDR execution:
 *   TCM+0x7FF0 (host address 0x79027FF0): the core executed the vector
 *   trace+0x3F40 (0xA1113F40): R5F stores to DDR actually land
 * The temporary stack starts below the reserved debug block at the top of
 * the TCM (0x7FC0..0x8000), which the linker script also keeps out of.
 */
__attribute__((naked, used, section(".tcm_probe")))
void Reset_Handler(void) {

  __asm volatile (
    "movw    r0, #0x7FF0               \n"  /* TCM witness               */
    "movt    r0, #0x0000               \n"
    "movw    r1, #0x0A7C               \n"
    "movt    r1, #0xB007               \n"
    "str     r1, [r0]                  \n"
    "dsb                               \n"
    "movw    r0, #0x3F40               \n"  /* DDR store witness         */
    "movt    r0, #0xA111               \n"
    "str     r1, [r0]                  \n"
    "dsb                               \n"
    "movw    sp, #0x7FC0               \n"  /* temporary stack, TCM top  */
    "movt    sp, #0x0000               \n"
    "bl      tcm_early_init            \n"
    "movw    r0, #:lower16:ddr_reset   \n"
    "movt    r0, #:upper16:ddr_reset   \n"
    "bx      r0                        \n");
}

/*
 * First DDR instruction after the MPU is on, proves DDR execution works.
 */
__attribute__((naked, used))
void ddr_reset(void) {

  __asm volatile (
    "movw    r0, #0x3F0C               \n"
    "movt    r0, #0xA111               \n"
    "movw    r1, #0x0004               \n"
    "movt    r1, #0xB007               \n"
    "str     r1, [r0]                  \n"
    "dsb                               \n"
    "b       _crt0_entry               \n");
}

/*
 * Fault handlers: record the fault type, return address and fault status
 * registers, then park the core.
 */
#define FAULT_HANDLER(name, type, fsr_op, far_op)                           \
__attribute__((naked, used, section(".tcm_probe")))                         \
void name(void) {                                                           \
                                                                            \
  __asm volatile (                                                          \
    "movw    r1, #" #type "            \n"                                  \
    "movt    r1, #0xDEAD               \n"                                  \
    "mrc     p15, 0, r2, " fsr_op "    \n"                                  \
    "mrc     p15, 0, r3, " far_op "    \n"                                  \
    "movw    r0, #0x7FE0               \n"  /* TCM fault block */           \
    "movt    r0, #0x0000               \n"                                  \
    "str     r1, [r0]                  \n"                                  \
    "str     lr, [r0, #4]              \n"                                  \
    "str     r2, [r0, #8]              \n"                                  \
    "str     r3, [r0, #12]             \n"                                  \
    "dsb                               \n"                                  \
    "movw    r0, #0x3F20               \n"  /* DDR fault block */           \
    "movt    r0, #0xA111               \n"                                  \
    "str     r1, [r0]                  \n"                                  \
    "str     lr, [r0, #4]              \n"                                  \
    "str     r2, [r0, #8]              \n"                                  \
    "str     r3, [r0, #12]             \n"                                  \
    "dsb                               \n"                                  \
    "1:  b   1b                        \n");                                \
}

/* Undefined instructions have no fault status registers, the DFSR/DFAR
   values recorded for them are stale leftovers.*/
FAULT_HANDLER(Und_Handler,      0x0001, "c5, c0, 0", "c6, c0, 0")
FAULT_HANDLER(Prefetch_Handler, 0x0002, "c5, c0, 1", "c6, c0, 2")  /* IFSR/IFAR */
FAULT_HANDLER(Abort_Handler,    0x0003, "c5, c0, 0", "c6, c0, 0")  /* DFSR/DFAR */

/*
 * Startup hook invoked by crt0 right after the mode stacks and FPU setup:
 * reaching it proves the banked stack pointers and FPU enable survived.
 */
void __cpu_init(void) {

  boot_mark(0x10U, 0xB0070005U);
}

/*
 * Startup hook invoked by crt0 before .data/.bss initialization. The MPU
 * and caches are already configured by tcm_early_init(), which must run
 * from TCM (see above), nothing left to do this early.
 */
void __early_init(void) {
}

/*
 * Startup hook invoked by crt0 after .data/.bss initialization: reaching
 * it proves the stack fill and the .data/.bss loops survived.
 */
void __late_init(void) {

  boot_mark(0x14U, 0xB0070006U);
}

/*
 * Board-specific initialization, invoked by halInit() after the drivers.
 * Clocks and pinmux are owned by the Linux host on this board, the MPU
 * and caches are configured by tcm_early_init() long before this point,
 * nothing is left to do here.
 */
void boardInit(void) {
}
