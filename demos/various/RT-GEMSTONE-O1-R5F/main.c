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
 * The SPI and I2C tests run on demand: pressing 's' or 'i' on the serial
 * console signals the matching semaphore, so a test can be started after
 * the Linux side has been prepared (its driver for the shared peripheral
 * unbound).
 */
static BSEMAPHORE_DECL(spi_trigger_bsem, true);
static BSEMAPHORE_DECL(i2c_trigger_bsem, true);

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
      if ((char)c == 's') {
        chBSemSignal(&spi_trigger_bsem);
      }
      if ((char)c == 'i') {
        chBSemSignal(&i2c_trigger_bsem);
      }
      chnPutTimeout(&SD1, (uint8_t)c, TIME_INFINITE);
    }
  }
}

/*
 * On-demand SPI smoke test (SPID1 = MCU_MCSPI0): an interrupt-driven
 * loopback exchange. With MOSI (D0) jumpered to MISO (D1) every
 * transmitted byte is received back; without the jumper the exchange
 * still completes, which alone validates the RX0_FULL interrupt path
 * through the VIM.
 *
 * The controller is shared with Linux (4b00000.spi): unless the device
 * tree reserves it for the R5F, unbind the Linux driver first or the
 * transfer stalls (its handler clears IRQENABLE on interrupts it
 * considers spurious):
 *
 *   echo 4b00000.spi | sudo tee /sys/bus/platform/drivers/omap2_mcspi/unbind
 */
static THD_WORKING_AREA(waSpiTestThread, 1024);
static THD_FUNCTION(SpiTestThread, arg) {
  static const SPIConfig spicfg = {
    .end_cb   = NULL,
    .speed    = 1000000U,   /* 1 MHz SCLK.*/
    .mode     = 0U          /* CPOL=0 CPHA=0.*/
  };
  static const uint8_t spi_tx[8] =
    {0xA5U, 0x5AU, 0xDEU, 0xADU, 0xBEU, 0xEFU, 0x12U, 0x34U};
  uint8_t spi_rx[8];

  (void)arg;

  chRegSetThreadName("spi-test");

  while (true) {
    sd1_puts("press 's' to run the SPI test\r\n");
    (void) chBSemWait(&spi_trigger_bsem);

    spiStart(&SPID1, &spicfg);
    if (!SPID1.ready) {
      trace_printf("spi: module did not leave reset (clock gated?)\n");
      sd1_puts("SPI module not ready (clock gated?)\r\n");
      continue;
    }

    spiSelect(&SPID1);
    spiExchange(&SPID1, sizeof spi_tx, spi_tx, spi_rx);
    spiUnselect(&SPID1);

    if (memcmp(spi_tx, spi_rx, sizeof spi_tx) == 0) {
      trace_printf("spi: loopback OK\n");
      sd1_puts("SPI loopback OK\r\n");
    }
    else {
      trace_printf("spi: loopback mismatch, rx: %x %x %x %x %x %x %x %x\n",
                   spi_rx[0], spi_rx[1], spi_rx[2], spi_rx[3],
                   spi_rx[4], spi_rx[5], spi_rx[6], spi_rx[7]);
      sd1_puts("SPI loopback MISMATCH (jumper D0-D1 missing?)\r\n");
    }
  }
}

/*
 * On-demand I2C bus scan (I2CD1 = MCU_I2C0, header pins 3/5): a 1-byte
 * read is attempted at every 7-bit address, a device that ACKs its
 * address is reported. With nothing attached every address NACKs, which
 * still exercises the full START/address/NACK/STOP interrupt path.
 *
 * The controller is shared with Linux (4900000.i2c): unbind its driver
 * before scanning or the two masters fight over the interrupt line:
 *
 *   echo 4900000.i2c | sudo tee /sys/bus/platform/drivers/omap_i2c/unbind
 */
static THD_WORKING_AREA(waI2cTestThread, 1024);
static THD_FUNCTION(I2cTestThread, arg) {
  static const I2CConfig i2ccfg = {
    .frequency = 100000U    /* Standard mode, 100 kHz.*/
  };

  (void)arg;

  chRegSetThreadName("i2c-test");

  while (true) {
    unsigned addr, found;

    sd1_puts("press 'i' to run the I2C bus scan\r\n");
    (void) chBSemWait(&i2c_trigger_bsem);

    i2cStart(&I2CD1, &i2ccfg);
    if (!I2CD1.ready) {
      trace_printf("i2c: module did not leave reset (clock gated?)\n");
      sd1_puts("I2C module not ready (clock gated?)\r\n");
      continue;
    }

    trace_printf("i2c: scanning\n");
    found = 0U;
    for (addr = 0x08U; addr <= 0x77U; addr++) {
      uint8_t dummy;
      msg_t msg;

      i2cAcquireBus(&I2CD1);
      msg = i2cMasterReceiveTimeout(&I2CD1, (i2caddr_t)addr, &dummy, 1U,
                                    TIME_MS2I(50));
      i2cReleaseBus(&I2CD1);

      if (msg == MSG_OK) {
        trace_printf("i2c: device at %x\n", addr);
        found++;
      }
      else if (msg == MSG_TIMEOUT) {
        /* A timeout leaves the driver in I2C_LOCKED, only i2cStart()
           recovers it, so the scan cannot usefully continue.*/
        trace_printf("i2c: timeout at %x, aborting scan\n", addr);
        sd1_puts("I2C scan timed out (Linux driver still bound?)\r\n");
        break;
      }
      /* MSG_RESET is the normal NACK of an empty address.*/
    }
    trace_printf("i2c: scan done, %u device(s)\n", found);
    sd1_puts("I2C scan done\r\n");
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
   * SPI test runs in its own thread so that a stuck transfer suspends
   * only that thread, the alive messages keep flowing either way.
   */
  (void) chThdCreateStatic(waSpiTestThread,
                           sizeof(waSpiTestThread),
                           NORMALPRIO,
                           SpiTestThread,
                           NULL);

  (void) chThdCreateStatic(waI2cTestThread,
                           sizeof(waI2cTestThread),
                           NORMALPRIO,
                           I2cTestThread,
                           NULL);

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

    /* Health beacon goes to trace0 only: the interactive serial console
       is for typed commands and their echo/results, a message every
       second there would bury both under a constant flood.*/
#if CORTEX_USE_FPU == TRUE
    trace_printf("alive: main=%u thread=%u fpu=%u fpu_errors=%u\n",
                 main_counter, thread_counter,
                 fpu_thread_counter, fpu_error_counter);
#else
    trace_printf("alive: main=%u thread=%u\n", main_counter, thread_counter);
#endif
  }
}
