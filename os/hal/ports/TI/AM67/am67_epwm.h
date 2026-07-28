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
 * @file    TI/AM67/am67_epwm.h
 * @brief   Minimal eHRPWM output driver for the AM67/J722S (classic
 *          ti,am3352-ehrpwm register map).
 * @details Register-level, up-count PWM. A generic API operates on any EPWM
 *          instance (by base address) and either output A or B; outputs A and
 *          B of one instance share the time base (TBPRD/frequency) but have
 *          independent compares (CMPA/CMPB). Pinmux, module clock and power are
 *          owned by the Linux host; this driver only touches EPWM registers.
 *
 *          The epwm0a_* helpers are thin wrappers for EPWM0 output A, kept for
 *          the existing scope-verified bring-up demo.
 */

#ifndef AM67_EPWM_H
#define AM67_EPWM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

  /* --- generic eHRPWM API (instance selected by base address) --- */

  /* Configure and start the time base (up-count, fixed prescale, TBPRD for
     frame_hz). Call once per instance; A and B share it. */
  void ehrpwm_start(uint32_t base, uint32_t frame_hz);

  /* Configure one output (false=A, true=B) for up-count PWM (high at zero,
     low at its compare), 0% duty, and release that output's software force.
     The other output is left untouched. */
  void ehrpwm_out_enable(uint32_t base, bool output_b);

  /* Set the high-time (pulse width) in microseconds on output A or B. */
  void ehrpwm_out_set_pulse_us(uint32_t base, bool output_b, uint32_t pulse_us);

  /* Force one output continuously low (counter keeps running for the shared
     time base / the other output). */
  void ehrpwm_out_low(uint32_t base, bool output_b);

  /* Register readbacks (diagnostics / running-detection). */
  uint16_t ehrpwm_read_tbctr(uint32_t base);
  uint16_t ehrpwm_read_tbprd(uint32_t base);
  uint16_t ehrpwm_read_cmp(uint32_t base, bool output_b);

  /* --- EPWM0 output A convenience wrappers (bring-up demo) --- */
  void epwm0a_init(void);
  void epwm0a_start(uint32_t frame_hz);
  void epwm0a_set_pulse_us(uint32_t pulse_us);
  void epwm0a_stop(void);
  uint16_t epwm0a_read_tbprd(void);
  uint16_t epwm0a_read_tbctr(void);
  uint16_t epwm0a_read_tbctl(void);
  uint16_t epwm0a_read_aqctla(void);
  uint16_t epwm0a_read_aqcsfrc(void);
  uint16_t epwm0a_read_cmpa(void);

#ifdef __cplusplus
}
#endif

#endif /* AM67_EPWM_H */
