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
 *          to the RemoteProc trace buffer and to the SD1 serial port (UART1,
 *          40-pin header pins 8/10, 115200 8N1). Characters received on SD1
 *          are echoed back. On the Linux host:
 *
 *          cat /sys/kernel/debug/remoteproc/remoteproc3/trace0
 */

#include <string.h>

#include "ch.h"
#include "hal.h"

#include "trace.h"

static volatile uint32_t thread_counter;
static volatile uint32_t main_counter;
#if CORTEX_USE_FPU == TRUE
static volatile uint32_t fpu_thread_counter;
static volatile uint32_t fpu_error_counter;
#endif

static void sd1_puts(const char *s) {

  chnWrite(&SD1, (const uint8_t *)s, strlen(s));
}

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

/*
 * UART RX echo thread: any character received on SD1 is sent back. The
 * thread sleeps inside chnGetTimeout() until the UART interrupt pushes a
 * received byte into the input queue, no polling involved.
 */
static THD_WORKING_AREA(waEchoThread, 256);
static THD_FUNCTION(EchoThread, arg) {

  (void)arg;

  chRegSetThreadName("uart-echo");

  while (true) {
    msg_t c = chnGetTimeout(&SD1, TIME_INFINITE);
    if (c >= MSG_OK) {
      chnPutTimeout(&SD1, (uint8_t)c, TIME_INFINITE);
    }
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
  trace_printf("ChibiOS/RT on %s\n", BOARD_NAME);
  trace_printf("port: %s, core: %s\n",
               PORT_ARCHITECTURE_NAME, PORT_CORE_VARIANT_NAME);

  /*
   * HAL initialization: platform (VIM), drivers (SD1 object), board hook
   * and the ST tick timer, in that order.
   */
  halInit();

  /*
   * System initialization, the main() function becomes a thread and the
   * RTOS is active.
   */
  chSysInit();

  /*
   * Activates SD1 (UART1) with the default configuration (115200 8N1).
   */
  trace_printf("uart: starting SD1\n");
  sdStart(&SD1, NULL);
  sd1_puts("SD1 started from the ChibiOS HAL\r\n");
  trace_printf("uart: first message sent\n");

  /*
   * SPI loopback test on SPID1 (MCU_MCSPI0): with MOSI (MCU_SPI0_D0)
   * jumpered to MISO (MCU_SPI0_D1) every transmitted byte is received
   * back. Without the jumper the received bytes read 0x00 or 0xFF.
   */
  {
    static const SPIConfig spicfg = {
      .end_cb   = NULL,
      .speed    = 1000000U,   /* 1 MHz SCLK.*/
      .mode     = 0U          /* CPOL=0 CPHA=0.*/
    };
    static const uint8_t spi_tx[8] =
      {0xA5U, 0x5AU, 0xDEU, 0xADU, 0xBEU, 0xEFU, 0x12U, 0x34U};
    uint8_t spi_rx[8];

    trace_printf("spi: starting SPID1\n");
    spiStart(&SPID1, &spicfg);
    spiSelect(&SPID1);
    spiExchange(&SPID1, sizeof spi_tx, spi_tx, spi_rx);
    spiUnselect(&SPID1);

    if (memcmp(spi_tx, spi_rx, sizeof spi_tx) == 0) {
      trace_printf("spi: loopback OK\n");
      sd1_puts("SPI loopback OK\r\n");
    }
    else {
      trace_printf("spi: loopback mismatch, rx: "
                   "%02x %02x %02x %02x %02x %02x %02x %02x\n",
                   spi_rx[0], spi_rx[1], spi_rx[2], spi_rx[3],
                   spi_rx[4], spi_rx[5], spi_rx[6], spi_rx[7]);
      sd1_puts("SPI loopback MISMATCH (jumper D0-D1 missing?)\r\n");
    }
  }

  trace_printf("kernel started, tick at %u Hz\n",
               (uint32_t)CH_CFG_ST_FREQUENCY);

  (void) chThdCreateStatic(waThread1,
                           sizeof(waThread1),
                           NORMALPRIO,
                           Thread1,
                           NULL);

  (void) chThdCreateStatic(waEchoThread,
                           sizeof(waEchoThread),
                           NORMALPRIO,
                           EchoThread,
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

    sd1_puts("SD1 alive from ChibiOS\r\n");

#if CORTEX_USE_FPU == TRUE
    trace_printf("alive: main=%u thread=%u fpu=%u fpu_errors=%u\n",
                 main_counter, thread_counter,
                 fpu_thread_counter, fpu_error_counter);
#else
    trace_printf("alive: main=%u thread=%u\n", main_counter, thread_counter);
#endif
  }
}
