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
 * @file    main.c
 * @brief   ChibiOS/RT bring-up demo for the T3 Gemstone O1 Cortex-R5F.
 * @details Two threads increment counters, progress is periodically logged
 *          to the RemoteProc trace buffer. On the Linux host:
 *
 *          cat /sys/kernel/debug/remoteproc/remoteproc0/trace0
 */

#include "armcr5.h"
#include "ch.h"

#include "board.h"
#include "am67_vim.h"
#include "am67_tick.h"
#include "trace.h"

static volatile uint32_t thread_counter;
static volatile uint32_t main_counter;
#if CORTEX_USE_FPU == TRUE
static volatile uint32_t fpu_thread_counter;
static volatile uint32_t fpu_error_counter;
#endif

/*
 * RTOS example thread.
 */
static THD_WORKING_AREA(waThread1, 256);
static THD_FUNCTION(Thread1, arg) {

  (void)arg;

  chRegSetThreadName("counter");

  while (true) {
    chThdSleepMilliseconds(100);
    thread_counter++;
  }
}

#if CORTEX_USE_FPU == TRUE
/*
 * FPU context switching test thread, the d8 marker register must survive
 * every reschedule.
 */
static THD_WORKING_AREA(waThread2, 512);
static THD_FUNCTION(Thread2, arg) {
  register double marker asm ("d8");
  double expected;

  (void)arg;

  chRegSetThreadName("fpu");

  marker = 1000.0;
  expected = 1000.0;

  while (true) {
    __asm volatile ("" : "+w" (marker));
    chThdSleepMilliseconds(10);
    __asm volatile ("" : "+w" (marker));

    if (marker != expected) {
      fpu_error_counter++;
      marker = expected;
    }

    marker += 1.0;
    expected += 1.0;
    fpu_thread_counter++;
  }
}
#endif

/*
 * Application entry point.
 */
int main(void) {

  trace_init();
  trace_printf("ChibiOS/RT on T3 Gemstone O1 R5F\n");
  trace_printf("port: %s, core: %s\n",
               PORT_ARCHITECTURE_NAME, PORT_CORE_VARIANT_NAME);

  vim_init();
  am67_tick_init();

  /*
   * System initialization, the main() function becomes a thread and the
   * RTOS is active.
   */
  chSysInit();

  trace_printf("kernel started, tick at %u Hz\n",
               (uint32_t)CH_CFG_ST_FREQUENCY);

  (void) chThdCreateStatic(waThread1,
                           sizeof(waThread1),
                           NORMALPRIO,
                           Thread1,
                           NULL);

#if CORTEX_USE_FPU == TRUE
  (void) chThdCreateStatic(waThread2,
                           sizeof(waThread2),
                           NORMALPRIO,
                           Thread2,
                           NULL);
#endif

  while (true) {
    chThdSleepMilliseconds(1000);
    main_counter++;

#if CORTEX_USE_FPU == TRUE
    trace_printf("alive: main=%u thread=%u fpu=%u fpu_errors=%u\n",
                 main_counter, thread_counter,
                 fpu_thread_counter, fpu_error_counter);
#else
    trace_printf("alive: main=%u thread=%u\n", main_counter, thread_counter);
#endif
  }
}
