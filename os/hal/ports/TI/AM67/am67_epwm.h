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
 * @brief   Minimal single-channel EPWM0 output (EHRPWM0_A) for the AM67/J722S.
 * @details Register-level, polling-free bring-up driver: the eHRPWM hardware
 *          generates the waveform once configured. One channel, up-count,
 *          fixed frame frequency, configurable pulse width, safe default-low
 *          startup. Pinmux, module clock and power are owned by the Linux
 *          host (device tree) -- this driver only touches EPWM registers.
 */

#ifndef AM67_EPWM_H
#define AM67_EPWM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
  /* Put the output in its safe state: pin forced LOW, counter frozen. No
     pulses are produced until epwm0a_start(). Call once before start. */
  void epwm0a_init(void);

  /* Configure the time base for a fixed frame frequency (e.g. 50 Hz for a
     20 ms servo/ESC frame), start at 0% duty (pin resting low), and let the
     action qualifier drive the pin. */
  void epwm0a_start(uint32_t frame_hz);

  /* Set the high-time (pulse width) in microseconds. Clamped to the frame. */
  void epwm0a_set_pulse_us(uint32_t pulse_us);

  /* Return the output to its safe state (pin forced LOW, counter frozen). */
  void epwm0a_stop(void);

  /* Read the TBPRD register straight back from hardware. Diagnostic for "is
     the module actually clocked/powered by the Linux host": after start() it
     must equal the programmed period; a 0 (or a bus fault) means EPWM0 is not
     clocked and the DT/clock arrangement is wrong. */
  uint16_t epwm0a_read_tbprd(void);
#ifdef __cplusplus
}
#endif

#endif /* AM67_EPWM_H */
