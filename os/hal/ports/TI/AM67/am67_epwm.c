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
 * @file    TI/AM67/am67_epwm.c
 * @brief   Minimal single-channel EPWM0 output (EHRPWM0_A) for the AM67/J722S.
 * @details Register map is the TI ETPWM "v1" layout (base + spread-out 16-bit
 *          registers, 32-bit CMPA whose *integer* compare is the high half).
 *          Offsets and the write recipe are taken from the TI MCU+SDK
 *          drivers/epwm/v1 (cslr_etpwm.h, etpwm.h). All EPWM registers are
 *          accessed 16-bit, matching TI's HW_WR_REG16 usage.
 *
 *          Up-count PWM: the A output is set HIGH at counter zero and cleared
 *          LOW when the counter reaches CMPA, so pulse width = CMPA / TBCLK
 *          and the frame period = TBPRD / TBCLK, with
 *          TBCLK = EPWMCLK / (CLKDIV * HSPCLKDIV).
 */

#include "hal.h"
#include "am67_epwm.h"

/*===========================================================================*/
/* ETPWM v1 register offsets (16-bit registers).                             */
/*===========================================================================*/

#define EPWM_TBCTL              0x00U  /* Time-base control.                  */
#define EPWM_TBCTR              0x08U  /* Time-base counter.                  */
#define EPWM_CMPCTL             0x10U  /* Compare control (reset = shadow).   */
#define EPWM_AQCTLA             0x80U  /* Action qualifier, output A.         */
#define EPWM_AQCSFRC            0x92U  /* Continuous software force.          */
#define EPWM_TBPRD              0xC6U  /* Time-base period (16-bit).          */
#define EPWM_CMPA_INT           0xD6U  /* CMPA integer compare (high half of  */
                                       /* the 32-bit CMPA at 0xD4).           */

/* TBCTL fields. */
#define TBCTL_CTRMODE_UP        (0U << 0)   /* Count up.                      */
#define TBCTL_CTRMODE_STOP      (3U << 0)   /* Stop-freeze.                   */
#define TBCTL_HSPCLKDIV_DIV4    (2U << 7)   /* High-speed prescale /4.        */
#define TBCTL_CLKDIV_DIV8       (3U << 10)  /* Prescale /8 (2^3).             */

/* Fixed prescale used here: HSPCLKDIV(/4) * CLKDIV(/8) = 32. Chosen so a
   50 Hz frame fits the 16-bit TBPRD at EPWMCLK = 100 MHz (100M/32/50 =
   62500 <= 65535). If AM67_EPWM0_CLK_HZ is re-measured, this may need to
   change to keep TBPRD in range. */
#define EPWM_PRESCALE           32U
#define TBCTL_PRESCALE          (TBCTL_HSPCLKDIV_DIV4 | TBCTL_CLKDIV_DIV8)

/* Action qualifier for output A, up-count PWM: set HIGH at ZERO (bits[1:0]=2),
   clear LOW on the up-count CMPA match (bits[5:4]=1). Value = 0x0012. */
#define AQ_ZRO_SET              (2U << 0)
#define AQ_CAU_CLEAR            (1U << 4)
#define AQCTLA_UP_PWM           (AQ_ZRO_SET | AQ_CAU_CLEAR)

/* Continuous software force on output A (AQCSFRC.CSFA[1:0]). */
#define AQCSFRC_CSFA_NONE       (0U << 0)   /* Normal action-qualifier drive. */
#define AQCSFRC_CSFA_LOW        (1U << 0)   /* Force output continuously LOW. */

/* Effective time-base clock after the fixed prescale. */
#define EPWM0_TBCLK_HZ          (AM67_EPWM0_CLK_HZ / EPWM_PRESCALE)

/*===========================================================================*/
/* Local helpers.                                                            */
/*===========================================================================*/

static inline void epwm_wr16(uint32_t offset, uint16_t value) {

  *(volatile uint16_t *)(AM67_EPWM0_BASE + offset) = value;
}

static inline uint16_t epwm_rd16(uint32_t offset) {

  return *(volatile uint16_t *)(AM67_EPWM0_BASE + offset);
}

/* Cached period so set_pulse can clamp the high-time to one frame. */
static uint16_t epwm0_period_ticks;

/*===========================================================================*/
/* Driver exported functions.                                                */
/*===========================================================================*/

void epwm0a_init(void) {

  /* Safe default: drive the pin continuously LOW and freeze the counter, so
     nothing is produced on the header pin until epwm0a_start(). */
  epwm_wr16(EPWM_AQCSFRC, AQCSFRC_CSFA_LOW);
  epwm_wr16(EPWM_TBCTL, TBCTL_CTRMODE_STOP | TBCTL_PRESCALE);
  epwm_wr16(EPWM_TBPRD, 0U);
  epwm_wr16(EPWM_CMPA_INT, 0U);
  epwm_wr16(EPWM_AQCTLA, AQCTLA_UP_PWM);
  epwm0_period_ticks = 0U;
}

void epwm0a_start(uint32_t frame_hz) {
  uint32_t prd;

  prd = EPWM0_TBCLK_HZ / frame_hz;
  if (prd > 0xFFFFU) {
    prd = 0xFFFFU;                 /* Prescale should prevent this; clamp.   */
  }
  epwm0_period_ticks = (uint16_t)prd;

  epwm_wr16(EPWM_TBPRD, epwm0_period_ticks);
  epwm_wr16(EPWM_CMPA_INT, 0U);    /* 0% duty: pin rests LOW until a pulse.  */
  epwm_wr16(EPWM_TBCTR, 0U);
  epwm_wr16(EPWM_AQCTLA, AQCTLA_UP_PWM);
  epwm_wr16(EPWM_AQCSFRC, AQCSFRC_CSFA_NONE);   /* Release the force.        */
  epwm_wr16(EPWM_TBCTL, TBCTL_CTRMODE_UP | TBCTL_PRESCALE);
}

void epwm0a_set_pulse_us(uint32_t pulse_us) {
  uint32_t cmp;

  cmp = (uint32_t)(((uint64_t)pulse_us * EPWM0_TBCLK_HZ) / 1000000ULL);
  if (cmp > epwm0_period_ticks) {
    cmp = epwm0_period_ticks;      /* Never exceed one frame (100% cap).     */
  }
  epwm_wr16(EPWM_CMPA_INT, (uint16_t)cmp);
}

void epwm0a_stop(void) {

  epwm_wr16(EPWM_AQCSFRC, AQCSFRC_CSFA_LOW);    /* Force pin LOW.            */
  epwm_wr16(EPWM_TBCTL, TBCTL_CTRMODE_STOP | TBCTL_PRESCALE);
}

uint16_t epwm0a_read_tbprd(void) {

  return epwm_rd16(EPWM_TBPRD);
}
