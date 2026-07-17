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
 * @file    board.h
 * @brief   T3 Gemstone O1 (TI AM67A/J722S) Cortex-R5F board definitions.
 * @details Addresses taken from the TI J722S TRM, cross-checked against the
 *          NuttX am67 port and the TI MCU+ SDK (j722s).
 */

#ifndef BOARD_H
#define BOARD_H

/*
 * Board identifier.
 */
#define BOARD_T3_GEMSTONE_O1_R5F
#define BOARD_NAME              "T3 Gemstone O1 (AM67A/J722S) R5F"

/*
 * Memory map as seen by the R5FSS0 core 0 (WKUP domain).
 */
#define AM67_ATCM_BASE          0x00000000U
#define AM67_ATCM_SIZE          (32U * 1024U)
#define AM67_BTCM_BASE          0x41010000U
#define AM67_BTCM_SIZE          (32U * 1024U)
#define AM67_MSRAM_BASE         0x60000000U
#define AM67_MSRAM_SIZE         (512U * 1024U)
#define AM67_DDR_BASE           0x80000000U

/*
 * DDR carveout assigned to this core by the Linux device tree, must match
 * the reserved-memory nodes used by the k3-r5 remoteproc driver.
 */
#define AM67_RSCTABLE_BASE      0xA2100000U
#define AM67_TRACEBUF_BASE      0xA2110000U
#define AM67_TRACEBUF_SIZE      (16U * 1024U)

/*
 * VIM interrupt controller (WKUP_R5FSS0 VIM).
 */
#define AM67_VIM_BASE           0x2FFF0000U
#define AM67_VIM_NUM_IRQS       512U

/*
 * DMTIMER0 used as the OS tick source.
 * Input clock is HFOSC0 at 25 MHz, IRQ number from cslr_intr_r5fss0_core0.h.
 */
#define AM67_TIMER0_BASE        0x02400000U
#define AM67_TIMER0_CLK_HZ      25000000U
#define AM67_TIMER0_IRQ         24U

#if !defined(_FROM_ASM_)
#ifdef __cplusplus
extern "C" {
#endif
  void boardInit(void);
#ifdef __cplusplus
}
#endif
#endif /* _FROM_ASM_ */

#endif /* BOARD_H */
