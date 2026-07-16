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
 * @file    am67_tick.c
 * @brief   DMTIMER0 system tick for the AM67A/J722S R5F.
 * @details The timer counts up from a preloaded value and interrupts on
 *          overflow, auto-reloading for a periodic tick. Register layout
 *          from the TI J722S TRM, matches the NuttX am67 tick timer.
 */

#include "ch.h"
#include "board.h"
#include "am67_vim.h"
#include "am67_tick.h"

#define TIMER_IRQ_EOI           0x20U
#define TIMER_IRQSTATUS         0x28U
#define TIMER_IRQSTATUS_SET     0x2CU
#define TIMER_IRQSTATUS_CLR     0x30U
#define TIMER_TCLR              0x38U
#define TIMER_TCRR              0x3CU
#define TIMER_TLDR              0x40U

#define TIMER_TCLR_ST           (1U << 0)
#define TIMER_TCLR_AR           (1U << 1)
#define TIMER_IRQ_OVF           (1U << 1)

static inline volatile uint32_t *tmr_reg(uint32_t offset) {

  return (volatile uint32_t *)(AM67_TIMER0_BASE + offset);
}

static void tick_clear_irq(void) {

  *tmr_reg(TIMER_IRQSTATUS) = TIMER_IRQ_OVF;

  /* Read back to make sure the write reached the peripheral, the interrupt
     is level-sensitive at the VIM.*/
  if ((*tmr_reg(TIMER_IRQSTATUS) & TIMER_IRQ_OVF) != 0U) {
    *tmr_reg(TIMER_IRQSTATUS) = TIMER_IRQ_OVF;
  }
}

static bool tick_irq_handler(void *arg) {
  bool preemption_required;

  (void)arg;

  tick_clear_irq();

  chSysLockFromISR();
  chSysTimerHandlerI();
  preemption_required = chSchIsPreemptionRequired();
  chSysUnlockFromISR();

  return preemption_required;
}

void am67_tick_init(void) {
  uint32_t reload;

  /* Counter value producing CH_CFG_ST_FREQUENCY overflows per second.*/
  reload = 0xFFFFFFFFU - (AM67_TIMER0_CLK_HZ / CH_CFG_ST_FREQUENCY) + 1U;

  /* Stop the timer and clear any pending overflow interrupt.*/
  *tmr_reg(TIMER_TCLR) = 0U;
  tick_clear_irq();

  /* Auto-reload mode, counter and reload value set for the tick period.*/
  *tmr_reg(TIMER_TCRR) = reload;
  *tmr_reg(TIMER_TLDR) = reload;
  *tmr_reg(TIMER_IRQSTATUS_SET) = TIMER_IRQ_OVF;

  vim_set_handler(AM67_TIMER0_IRQ, tick_irq_handler, NULL);
  vim_set_priority(AM67_TIMER0_IRQ, 0x8U);
  vim_enable_irq(AM67_TIMER0_IRQ);

  /* Start counting.*/
  *tmr_reg(TIMER_TCLR) = TIMER_TCLR_ST | TIMER_TCLR_AR;
}
