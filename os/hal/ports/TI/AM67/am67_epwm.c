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
 * @details Register map is the CLASSIC eHRPWM (ti,am3352-ehrpwm) layout, which
 *          is what the J722S EPWM instances use. Offsets confirmed against the
 *          J722S register spreadsheet (62_EPWM0) and the Linux pwm-tiehrpwm
 *          driver. All EPWM registers are 16-bit (readw/writew); CMPA is a
 *          single 16-bit compare at 0x12 (no high-res split for basic PWM).
 *
 *          Up-count PWM: the A output is set HIGH at counter zero and cleared
 *          LOW when the counter reaches CMPA, so pulse width = CMPA / TBCLK
 *          and the frame period = TBPRD / TBCLK, with
 *          TBCLK = EPWMCLK / (CLKDIV * HSPCLKDIV).
 */

#include "hal.h"
#include "am67_epwm.h"

/*===========================================================================*/
/* eHRPWM register offsets (16-bit registers).                               */
/*===========================================================================*/

/* CLASSIC eHRPWM (ti,am3352-ehrpwm) register map -- this is the J722S EPWM
   layout, confirmed against the J722S register spreadsheet (62_EPWM0) AND the
   Linux pwm-tiehrpwm driver (which drives this same instance). All registers
   are 16-bit (readw/writew). The earlier "ETPWM v1" offsets (0xC6/0xD4/0x80)
   were WRONG for this SoC -- only TBCTL/TBCTR happened to coincide, which is
   why TBCTR advanced but TBPRD read back 0. */
#define EPWM_TBCTL              0x00U  /* Time-base control.                  */
#define EPWM_TBCTR              0x08U  /* Time-base counter.                  */
#define EPWM_TBPRD              0x0AU  /* Time-base period (16-bit).          */
#define EPWM_CMPCTL             0x0EU  /* Compare control (reset = shadow).   */
#define EPWM_CMPA               0x12U  /* Counter-compare A (16-bit, 15:0).   */
#define EPWM_AQCTLA             0x16U  /* Action qualifier, output A.         */
#define EPWM_AQCSFRC            0x1CU  /* Continuous software force.          */

/* TBCTL fields. */
#define TBCTL_CTRMODE_UP        (0U << 0)   /* Count up.                      */
#define TBCTL_CTRMODE_STOP      (3U << 0)   /* Stop-freeze.                   */
#define TBCTL_HSPCLKDIV_DIV10   (5U << 7)   /* High-speed prescale /10 (0b101).*/
#define TBCTL_CLKDIV_DIV8       (3U << 10)  /* Prescale /8 (2^3, 0b011).      */

/* Fixed prescale = HSPCLKDIV(/10) * CLKDIV(/8) = 80. Sized for the confirmed
   250 MHz EPWM input clock (fck) so a 50 Hz frame fits the 16-bit TBPRD:
   250e6 / 80 / 50 = 62500 <= 65535. TBCLK_eff = 250e6/80 = 3.125 MHz
   (0.32 us resolution). If AM67_EPWM0_CLK_HZ changes, revisit this so TBPRD
   stays <= 65535. */
#define EPWM_PRESCALE           80U
#define TBCTL_PRESCALE          (TBCTL_HSPCLKDIV_DIV10 | TBCTL_CLKDIV_DIV8)

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
  epwm_wr16(EPWM_CMPA, 0U);
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
  epwm_wr16(EPWM_CMPA, 0U);    /* 0% duty: pin rests LOW until a pulse.  */
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
  epwm_wr16(EPWM_CMPA, (uint16_t)cmp);
}

void epwm0a_stop(void) {

  epwm_wr16(EPWM_AQCSFRC, AQCSFRC_CSFA_LOW);    /* Force pin LOW.            */
  epwm_wr16(EPWM_TBCTL, TBCTL_CTRMODE_STOP | TBCTL_PRESCALE);
}

uint16_t epwm0a_read_tbprd(void) {

  return epwm_rd16(EPWM_TBPRD);
}

uint16_t epwm0a_read_tbctr(void) {

  return epwm_rd16(EPWM_TBCTR);
}

/* Raw register readbacks for bring-up debugging. */
uint16_t epwm0a_read_tbctl(void)   { return epwm_rd16(EPWM_TBCTL);   }
uint16_t epwm0a_read_aqctla(void)  { return epwm_rd16(EPWM_AQCTLA);  }
uint16_t epwm0a_read_aqcsfrc(void) { return epwm_rd16(EPWM_AQCSFRC); }
uint16_t epwm0a_read_cmpa(void)    { return epwm_rd16(EPWM_CMPA);    }
