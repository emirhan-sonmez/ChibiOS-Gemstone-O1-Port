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
 * The SPI test runs on demand: pressing 's' on the serial console
 * signals this semaphore, so the test can be started after the Linux
 * side has been prepared (e.g. its McSPI driver unbound).
 */
static BSEMAPHORE_DECL(spi_trigger_bsem, true);

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
      chnPutTimeout(&SD1, (uint8_t)c, TIME_INFINITE);
    }
  }
}

/*
 * One-shot SPI test thread (SPID1 = MCU_MCSPI0): SYST-mode pin continuity
 * check (D0 driven, D1 read back), then an interrupt-driven loopback
 * exchange. With MOSI (D0) jumpered to MISO (D1) every transmitted byte
 * is received back; without the jumper the exchange still completes,
 * which alone validates the RX0_FULL interrupt path through the VIM.
 */
#define SPI_DBG_REG(off) (*(volatile uint32_t *)(AM67_MCSPI0_BASE + (off)))

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
    extern volatile uint32_t am67_spi_isr_count, am67_spi_frames,
                             am67_spi_ien_exit[8];
    unsigned k;

    /* Wait for the 's' key on the serial console before each run, so the
       Linux side can be prepared first (unbind its McSPI driver).*/
    sd1_puts("press 's' to run the SPI test\r\n");
    (void) chBSemWait(&spi_trigger_bsem);

    am67_spi_isr_count = 0U;
    am67_spi_frames = 0U;
    for (k = 0U; k < 8U; k++) {
      am67_spi_ien_exit[k] = 0U;
    }

    trace_printf("spi: starting SPID1\n");
    spiStart(&SPID1, &spicfg);
    trace_printf("spi: started, ready=%u\n", (uint32_t)SPID1.ready);
    trace_printf("spi: pads CS0=%x CLK=%x D0=%x D1=%x "
                 "(expect 10000/50000/50000/50000)\n",
                 *(volatile uint32_t *)0x04084000U,
                 *(volatile uint32_t *)0x04084008U,
                 *(volatile uint32_t *)0x0408400CU,
                 *(volatile uint32_t *)0x04084010U);

    if (SPID1.ready) {
    unsigned i;
    uint32_t lo, hi;

    /* Physical continuity test via McSPI system test mode: D0 becomes a
       manually driven output, D1 a readable input. With the D0-D1 jumper
       in place D1 must follow D0, no SPI transfer involved.*/
    SPI_DBG_REG(MCSPI_MODULCTRL_OFFSET) |= MCSPI_MODULCTRL_SYSTEM_TEST;
    SPI_DBG_REG(MCSPI_SYST_OFFSET) = MCSPI_SYST_SPIDATDIR1;     /* D0 = 0 */
    for (i = 0U; i < 1000U; i++) {
      __asm volatile ("");
    }
    lo = (SPI_DBG_REG(MCSPI_SYST_OFFSET) >> 5) & 1U;
    SPI_DBG_REG(MCSPI_SYST_OFFSET) = MCSPI_SYST_SPIDATDIR1 |
                                     MCSPI_SYST_SPIDAT_0;       /* D0 = 1 */
    for (i = 0U; i < 1000U; i++) {
      __asm volatile ("");
    }
    hi = (SPI_DBG_REG(MCSPI_SYST_OFFSET) >> 5) & 1U;
    SPI_DBG_REG(MCSPI_MODULCTRL_OFFSET) &= ~MCSPI_MODULCTRL_SYSTEM_TEST;
    trace_printf("spi: pin test D1 reads %u with D0 low, %u with D0 high "
                 "(jumper OK = 0 then 1)\n", lo, hi);

    /* Interrupt-driven exchange: this thread suspends until the RX0_FULL
       interrupt chain completes the transfer (VIM line 207).*/
    spiSelect(&SPID1);
    spiExchange(&SPID1, sizeof spi_tx, spi_tx, spi_rx);
    trace_printf("spi: irq exchange completed\n");
    spiUnselect(&SPID1);

    if (memcmp(spi_tx, spi_rx, sizeof spi_tx) == 0) {
      trace_printf("spi: loopback OK\n");
      sd1_puts("SPI loopback OK\r\n");
    }
    else {
      trace_printf("spi: loopback mismatch, rx: "
                   "%x %x %x %x %x %x %x %x\n",
                   spi_rx[0], spi_rx[1], spi_rx[2], spi_rx[3],
                   spi_rx[4], spi_rx[5], spi_rx[6], spi_rx[7]);
      sd1_puts("SPI loopback MISMATCH (jumper D0-D1 missing?)\r\n");
    }
    }
    else {
      trace_printf("spi: module did not leave reset (clock gated?)\n");
      sd1_puts("SPI module not ready (clock gated?)\r\n");
    }

    trace_printf("spi: run done, isr_count=%u frames=%u\n",
                 am67_spi_isr_count, am67_spi_frames);
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

    /* One-shot diagnostic while the SPI test thread is presumably stuck:
       the McSPI interrupt state and every asserted VIM line (raw status,
       independent of enables). IRQ 207 = group 6 bit 15.*/
    if (main_counter == 3U) {
      extern volatile uint32_t am67_spi_isr_count, am67_spi_frames,
                               am67_spi_ien_rb, am67_spi_ien_exit[8];
      uint32_t g;

      trace_printf("spi-dbg: IRQENABLE=%x IRQSTATUS=%x CHSTAT0=%x\n",
                   SPI_DBG_REG(MCSPI_IRQENABLE_OFFSET),
                   SPI_DBG_REG(MCSPI_IRQSTATUS_OFFSET),
                   SPI_DBG_REG(MCSPI_CHSTAT0_OFFSET));
      trace_printf("spi-dbg: isr_count=%u frames=%u ien_readback=%x "
                   "state=%u remaining=%u\n",
                   am67_spi_isr_count, am67_spi_frames, am67_spi_ien_rb,
                   (uint32_t)SPID1.state, (uint32_t)SPID1.remaining);
      trace_printf("spi-dbg: ien at isr exits: %x %x %x %x %x %x %x %x\n",
                   am67_spi_ien_exit[0], am67_spi_ien_exit[1],
                   am67_spi_ien_exit[2], am67_spi_ien_exit[3],
                   am67_spi_ien_exit[4], am67_spi_ien_exit[5],
                   am67_spi_ien_exit[6], am67_spi_ien_exit[7]);
      for (g = 0U; g < 16U; g += 4U) {
        trace_printf("vim-raw[%u..%u]: %x %x %x %x\n", g * 32U,
                     (g + 4U) * 32U - 1U,
                     *(volatile uint32_t *)(0x2FFF0400U + ((g + 0U) * 0x20U)),
                     *(volatile uint32_t *)(0x2FFF0400U + ((g + 1U) * 0x20U)),
                     *(volatile uint32_t *)(0x2FFF0400U + ((g + 2U) * 0x20U)),
                     *(volatile uint32_t *)(0x2FFF0400U + ((g + 3U) * 0x20U)));
      }
    }

    /* Recovery experiment: while the exchange is stuck, re-arm the RX
       interrupt. If each re-arm advances the transfer by one frame the
       delivery path is healthy and only the enable bit is being killed
       by an external agent (Linux still owns this controller).*/
    if ((main_counter >= 4U) && (SPID1.state == SPI_ACTIVE)) {
      trace_printf("spi-dbg: re-arming, remaining=%u\n",
                   (uint32_t)SPID1.remaining);
      SPI_DBG_REG(MCSPI_IRQENABLE_OFFSET) = MCSPI_IRQ_RX0_FULL;
    }

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
