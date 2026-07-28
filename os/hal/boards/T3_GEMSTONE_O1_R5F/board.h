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
 * Memory map as seen by the MCU_R5FSS0 core 0, see
 * cslr_mcu_r5fss0_baseaddress.h in the TI MCU+ SDK.
 *
 * Only one TCM is usable on this core. The Linux device tree gives it
 * ti,loczrama = <0>, which puts the BTCM at address 0 and the ATCM at
 * 0x41010000, and then ti,atcm-enable = <0> leaves the ATCM switched off.
 * Address 0 is not negotiable: Cortex-R5 has no VBAR, so the vectors, the
 * pre-MPU boot code and the banked mode stacks all share this 32K block.
 * The Main domain core has the opposite layout (loczrama = 1, both TCMs
 * enabled), which is why the linker script differs from the Main build.
 */
#define AM67_TCM_BASE           0x00000000U
#define AM67_TCM_SIZE           (32U * 1024U)
#define AM67_MSRAM_BASE         0x60000000U
#define AM67_MSRAM_SIZE         (512U * 1024U)
#define AM67_DDR_BASE           0x80000000U

/*
 * DDR carveout assigned to this core by the Linux device tree, must match
 * the reserved-memory nodes used by the k3-r5 remoteproc driver. For the
 * MCU core these are mcu_r5fss0_core0_memory_region@a1100000 (15M), 16M
 * below the Main core's carveout so both cores can run at the same time.
 */
#define AM67_RSCTABLE_BASE      0xA1100000U
#define AM67_TRACEBUF_BASE      0xA1110000U
#define AM67_TRACEBUF_SIZE      (16U * 1024U)

/*
 * VIM interrupt controller (MCU_R5FSS0 VIC_CFG, in this core's own view).
 */
#define AM67_VIM_BASE           0x07FF0000U
#define AM67_VIM_NUM_IRQS       512U

/*
 * MCU_TIMER0 used as the OS tick source, IRQ number from
 * cslr_intr_mcu_r5fss0_core0.h.
 *
 * The device tree leaves this timer's clock mux at its reset default,
 * unlike main_timer0 which is explicitly reparented, so the 25 MHz below
 * is the expected HFOSC0 default rather than a verified value. A wrong
 * parent shows up as a tick rate off by a clean integer ratio, not as a
 * boot failure, so it is safe to confirm on hardware.
 */
#define AM67_TIMER0_BASE        0x04800000U
#define AM67_TIMER0_CLK_HZ      25000000U
#define AM67_TIMER0_IRQ         28U

/*
 * EPWM0 (eHRPWM), first PWM output on EHRPWM0_A -> Gemstone 40-pin header
 * pin 29 (GPIO5 pad, muxed to EHRPWM0_A by the Linux DT overlay
 * k3-am67a-t3-gem-o1-pwm-epwm0-gpio5.dtbo). The DT owns pinmux + the module
 * clock/power; the firmware only drives EPWM registers.
 *
 * AM67_EPWM0_CLK_HZ is the EPWM counter input clock (SYSCLKOUT), i.e. the
 * clock BEFORE the TBCTL HSPCLKDIV/CLKDIV prescale. It is the module "fck":
 * confirmed 250 MHz on this board via /sys/kernel/debug/clk/clk_summary and
 * the Linux pwm-tiehrpwm driver (it derives period/duty from clk_get_rate of
 * "fck"). The separate epwm_tbclk gate must be enabled for the counter to
 * run but is NOT a divider. Final confirmation pending scope measurement.
 */
#define AM67_EPWM0_BASE         0x23000000U
#define AM67_EPWM0_CLK_HZ       250000000U   /* fck = SYSCLKOUT, pre-prescale. */

/*
 * EPWM1 (eHRPWM) second instance: EHRPWM1_A -> GPIO6 (pin 31), EHRPWM1_B ->
 * GPIO13 (pin 33). Same IP and 250 MHz fck as EPWM0; A and B share the time
 * base but have independent CMPA/CMPB compares.
 *
 * NOTE: EHRPWM0_B is deliberately NOT defined/used -- its pad is physical pin 8,
 * reserved for the UART1 console TX.
 */
#define AM67_EPWM1_BASE         0x23010000U

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
