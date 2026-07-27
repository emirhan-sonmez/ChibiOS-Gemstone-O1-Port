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
 *          cat /sys/kernel/debug/remoteproc/remoteproc2/trace0
 */

#include <string.h>

#include "ch.h"
#include "hal.h"

#include "trace.h"
#include "am67_epwm.h"

static volatile uint32_t thread_counter;
static volatile uint32_t main_counter;
#if CORTEX_USE_FPU == TRUE
static volatile uint32_t fpu_thread_counter;
static volatile uint32_t fpu_error_counter;
#endif

/* Several threads share the console, the mutex keeps their messages from
   interleaving mid-line.*/
static MUTEX_DECL(sd1_mtx);

static void sd1_puts(const char *s) {

  chMtxLock(&sd1_mtx);
  chnWrite(&SD1, (const uint8_t *)s, strlen(s));
  chMtxUnlock(&sd1_mtx);
}

/* Prints a hundredths-of-a-unit fixed point value as "-12.34". Avoids
   pulling floating point and printf into the demo for two numbers.*/
static void sd1_putcenti(int32_t centi) {
  char out[16];
  uint32_t whole, frac, u;
  unsigned i = 0U, j;

  if (centi < 0) {
    out[i++] = '-';
    u = (uint32_t)(-centi);
  }
  else {
    u = (uint32_t)centi;
  }
  whole = u / 100U;
  frac  = u % 100U;

  j = i;
  if (whole == 0U) {
    out[i++] = '0';
  }
  else {
    char rev[10];
    unsigned n = 0U;

    while (whole > 0U) {
      rev[n++] = (char)('0' + (whole % 10U));
      whole /= 10U;
    }
    while (n > 0U) {
      out[i++] = rev[--n];
    }
  }
  (void)j;
  out[i++] = '.';
  out[i++] = (char)('0' + (frac / 10U));
  out[i++] = (char)('0' + (frac % 10U));

  chMtxLock(&sd1_mtx);
  chnWrite(&SD1, (const uint8_t *)out, i);
  chMtxUnlock(&sd1_mtx);
}

/* Prints a byte as two hexadecimal digits.*/
static void sd1_puthex(uint8_t value) {
  static const char digits[] = "0123456789abcdef";
  char out[2];

  out[0] = digits[(value >> 4) & 0x0FU];
  out[1] = digits[value & 0x0FU];

  chMtxLock(&sd1_mtx);
  chnWrite(&SD1, (const uint8_t *)out, sizeof out);
  chMtxUnlock(&sd1_mtx);
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

/* Address the 'r' command talks to, the one the bus scan reports.*/
#define I2C_TEST_ADDR           0x70U

/* Compile time of this binary, see the build stamp in main().*/
#define BUILD_DATE              __DATE__
#define BUILD_TIME              __TIME__

/* Which operation the console asked the I2C thread to run.*/
#define I2C_CMD_SCAN            0U
#define I2C_CMD_READ            1U
static volatile uint32_t i2c_command = I2C_CMD_SCAN;

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
        i2c_command = I2C_CMD_SCAN;
        chBSemSignal(&i2c_trigger_bsem);
      }
      if ((char)c == 'r') {
        i2c_command = I2C_CMD_READ;
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

    /* The echo of the trigger key sits on the current line.*/
    sd1_puts("\r\n");

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

static const char *i2c_init_error_text(uint32_t reason) {

  switch (reason) {
  case I2C_INIT_RESET_TIMEOUT:
    return "module never left reset (clock gated?)";
  case I2C_INIT_TIMING_LOST:
    return "SCL timing wiped by a late reset";
  case I2C_INIT_BAD_FREQUENCY:
    return "requested SCL rate unreachable";
  case I2C_INIT_BUS_STUCK:
    return "bus still held after a STOP (slave stretching?)";
  default:
    return "unknown";
  }
}

/*
 * On-demand I2C bus scan (I2CD1 = MCU_I2C0, header pins 3/5): a 1-byte
 * read is attempted at every 7-bit address, a device that ACKs its
 * address is reported. With nothing attached every address NACKs, which
 * still exercises the full START/address/NACK/STOP interrupt path.
 *
 * If 4900000.i2c is bound to Linux's omap_i2c the two masters fight over
 * the controller, so unbind it first. It is normally NOT bound on this
 * board, and "No such device" from the unbind means there is nothing to
 * do rather than something to fix:
 *
 *   echo 4900000.i2c | sudo tee /sys/bus/platform/drivers/omap_i2c/unbind
 */
/*
 * Reports why a transfer timed out. The controller snapshot separates a
 * bus held by a slave (BB, bit 12 of raw) from a module with no clock
 * configured (scll 0) from an interrupt that was raised but never
 * dispatched (irqen and vim both sane).
 */
static void i2c_report_timeout(void) {

  trace_printf("i2c: timeout con=%x raw=%x scll=%u irqen=%x vim=%x\n",
               I2CD1.dbg.con, I2CD1.dbg.raw, I2CD1.dbg.scll,
               I2CD1.dbg.irqen, I2CD1.dbg.vim);
  sd1_puts("I2C timeout: con=0x");
  sd1_puthex((uint8_t)(I2CD1.dbg.con >> 8));
  sd1_puthex((uint8_t)I2CD1.dbg.con);
  sd1_puts(" raw=0x");
  sd1_puthex((uint8_t)(I2CD1.dbg.raw >> 8));
  sd1_puthex((uint8_t)I2CD1.dbg.raw);
  sd1_puts(" irqen=0x");
  sd1_puthex((uint8_t)(I2CD1.dbg.irqen >> 8));
  sd1_puthex((uint8_t)I2CD1.dbg.irqen);
  sd1_puts(" vim=0x");
  sd1_puthex((uint8_t)I2CD1.dbg.vim);
  sd1_puts("\r\n");
}

/*
 * Sensirion SHTC3 temperature and humidity sensor, the part that answers
 * at 0x70. It takes 16-bit commands rather than a register index, and
 * every reply is 16 bits of data followed by a CRC-8.
 *
 * Talking to it exercises the two-segment transfer path, which the bus
 * scan never touches: the scan is address-only, so it never moves a data
 * byte, never takes the transmit-to-receive phase change on a repeated
 * START and never fills the RX FIFO.
 */
#define SHTC3_CMD_SLEEP         0xB098U
#define SHTC3_CMD_WAKEUP        0x3517U
#define SHTC3_CMD_READ_ID       0xEFC8U
/* Normal mode, temperature first, clock stretching disabled: the result
   is collected after a fixed delay instead of relying on the slave
   holding SCL, which keeps the driver out of clock-stretch handling.*/
#define SHTC3_CMD_MEASURE       0x7866U
#define SHTC3_MEAS_DELAY_MS     20U     /* Datasheet max is 12.1 ms.      */
/* The part number is encoded in bits 11 and 5:0 of the ID word.*/
#define SHTC3_ID_MASK           0x083FU
#define SHTC3_ID_VALUE          0x0807U

static uint8_t shtc3_crc(const uint8_t *data, size_t len) {
  uint8_t crc = 0xFFU;
  size_t i;
  unsigned bit;

  for (i = 0U; i < len; i++) {
    crc ^= data[i];
    for (bit = 0U; bit < 8U; bit++) {
      if ((crc & 0x80U) != 0U) {
        crc = (uint8_t)((crc << 1) ^ 0x31U);
      }
      else {
        crc = (uint8_t)(crc << 1);
      }
    }
  }
  return crc;
}

/* Sends a 16-bit command, optionally reading a reply after a repeated
   START. rxlen 0 sends the command on its own.*/
static msg_t shtc3_command(uint16_t cmd, uint8_t *rxbuf, size_t rxlen) {
  uint8_t tx[2];
  msg_t msg;

  tx[0] = (uint8_t)(cmd >> 8);
  tx[1] = (uint8_t)cmd;

  i2cAcquireBus(&I2CD1);
  msg = i2cMasterTransmitTimeout(&I2CD1, (i2caddr_t)I2C_TEST_ADDR,
                                 tx, sizeof tx, rxbuf, rxlen,
                                 TIME_MS2I(50));
  i2cReleaseBus(&I2CD1);
  return msg;
}

/* Reports a failed step and says which one it was.*/
static void shtc3_report_error(const char *step, msg_t msg) {

  if (msg == MSG_TIMEOUT) {
    sd1_puts("  timeout during ");
    sd1_puts(step);
    sd1_puts("\r\n");
    i2c_report_timeout();
  }
  else {
    sd1_puts("  NACK during ");
    sd1_puts(step);
    sd1_puts("\r\n");
    trace_printf("i2c: shtc3 NACK during %s\n", step);
  }
}

static void i2c_do_read(void) {
  uint8_t rx[6];
  uint16_t id, raw_t, raw_rh;
  int32_t temp_centi, rh_centi;
  msg_t msg;

  /* The sensor powers up asleep and is put back to sleep at the end of
     this function, so every run starts by waking it.*/
  msg = shtc3_command(SHTC3_CMD_WAKEUP, NULL, 0U);
  if (msg != MSG_OK) {
    shtc3_report_error("wakeup", msg);
    return;
  }
  chThdSleepMilliseconds(1);

  msg = shtc3_command(SHTC3_CMD_READ_ID, rx, 3U);
  if (msg != MSG_OK) {
    shtc3_report_error("read id", msg);
    return;
  }
  id = (uint16_t)(((uint16_t)rx[0] << 8) | rx[1]);

  sd1_puts("  id=0x");
  sd1_puthex(rx[0]);
  sd1_puthex(rx[1]);
  if (shtc3_crc(rx, 2U) != rx[2]) {
    sd1_puts(" CRC BAD (bus noise or wrong part)\r\n");
    trace_printf("i2c: shtc3 id crc bad, id %x\n", id);
    return;
  }
  if ((id & SHTC3_ID_MASK) != SHTC3_ID_VALUE) {
    sd1_puts(" not an SHTC3\r\n");
    trace_printf("i2c: unexpected id %x\n", id);
    return;
  }
  sd1_puts(" SHTC3 confirmed\r\n");

  msg = shtc3_command(SHTC3_CMD_MEASURE, NULL, 0U);
  if (msg != MSG_OK) {
    shtc3_report_error("measure", msg);
    return;
  }
  chThdSleepMilliseconds(SHTC3_MEAS_DELAY_MS);

  /* The conversion result is read back as a plain 6-byte read.*/
  i2cAcquireBus(&I2CD1);
  msg = i2cMasterReceiveTimeout(&I2CD1, (i2caddr_t)I2C_TEST_ADDR,
                                rx, sizeof rx, TIME_MS2I(50));
  i2cReleaseBus(&I2CD1);
  if (msg != MSG_OK) {
    shtc3_report_error("result read", msg);
    return;
  }

  if ((shtc3_crc(&rx[0], 2U) != rx[2]) || (shtc3_crc(&rx[3], 2U) != rx[5])) {
    sd1_puts("  measurement CRC BAD\r\n");
    trace_printf("i2c: shtc3 measurement crc bad\n");
    return;
  }

  raw_t  = (uint16_t)(((uint16_t)rx[0] << 8) | rx[1]);
  raw_rh = (uint16_t)(((uint16_t)rx[3] << 8) | rx[4]);

  /* Datasheet transfer functions, in hundredths to stay in integers:
     T[C] = -45 + 175 * raw / 65536, RH[%] = 100 * raw / 65536.*/
  temp_centi = -4500 + (int32_t)((17500U * (uint32_t)raw_t) / 65536U);
  rh_centi   = (int32_t)((10000U * (uint32_t)raw_rh) / 65536U);

  sd1_puts("  T=");
  sd1_putcenti(temp_centi);
  sd1_puts(" C  RH=");
  sd1_putcenti(rh_centi);
  sd1_puts(" %\r\n");
  trace_printf("i2c: shtc3 t=%d centiC rh=%d centi%%\n",
               (int)temp_centi, (int)rh_centi);

  (void)shtc3_command(SHTC3_CMD_SLEEP, NULL, 0U);
}

static THD_WORKING_AREA(waI2cTestThread, 1024);
static THD_FUNCTION(I2cTestThread, arg) {
  static const I2CConfig i2ccfg = {
    .frequency = 100000U    /* Standard mode, 100 kHz.*/
  };

  (void)arg;

  chRegSetThreadName("i2c-test");

  while (true) {
    unsigned addr, found;

    sd1_puts("press 'i' to scan the I2C bus, 'r' to read from 0x");
    sd1_puthex((uint8_t)I2C_TEST_ADDR);
    sd1_puts("\r\n");
    (void) chBSemWait(&i2c_trigger_bsem);

    /* The echo of the trigger key sits on the current line.*/
    sd1_puts("\r\n");

    i2cStart(&I2CD1, &i2ccfg);
    if (!I2CD1.ready) {
      trace_printf("i2c: init failed, reason %u\n", I2CD1.init_error);
      sd1_puts("I2C init failed: ");
      sd1_puts(i2c_init_error_text(I2CD1.init_error));
      sd1_puts("\r\n");
      continue;
    }

    if (i2c_command == I2C_CMD_READ) {
      sd1_puts("I2C reading...\r\n");
      i2c_do_read();
      continue;
    }

    sd1_puts("I2C scanning...\r\n");
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
        sd1_puts("  device at 0x");
        sd1_puthex((uint8_t)addr);
        sd1_puts("\r\n");
        found++;
      }
      else if (msg == MSG_TIMEOUT) {
        /* A timeout leaves the driver in I2C_LOCKED, only i2cStart()
           recovers it, so the scan cannot usefully continue.*/
        trace_printf("i2c: timeout at address %x\n", addr);
        i2c_report_timeout();
        break;
      }
      /* MSG_RESET is the normal NACK of an empty address.*/
    }
    trace_printf("i2c: scan done, %u device(s)\n", found);
    if (found == 0U) {
      sd1_puts("I2C scan done, no devices found\r\n");
    }
    else {
      sd1_puts("I2C scan done\r\n");
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
 * EPWM0_A bring-up (M4). Drives EHRPWM0_A on the 40-pin header (pin 29,
 * GPIO5 pad muxed by the Linux DT overlay) at a 50 Hz servo/ESC frame,
 * stepping the pulse width through 1000/1500/2000 us every 3 s so the
 * waveform can be checked on a scope. SCOPE ONLY -- do NOT connect an ESC
 * or motor until AM67_EPWM0_CLK_HZ has been calibrated against the measured
 * period, because the 100 MHz clock is still provisional.
 */
static THD_WORKING_AREA(waPwmThread, 256);
static THD_FUNCTION(PwmThread, arg) {
  static const uint32_t pulses_us[] = { 1000U, 1500U, 2000U };
  unsigned i = 0U;

  (void)arg;
  chRegSetThreadName("pwm");

  epwm0a_init();          /* Pin forced LOW until the frame is started. */
  epwm0a_start(50U);      /* 50 Hz -> 20000 us frame. */

  /* Clocking self-check: TBPRD must read back as the programmed period
     (62500 at the provisional 100 MHz). A 0 here means EPWM0 is not clocked
     by the Linux host -> fix the DT/clock arrangement before scoping. */
  trace_printf("pwm: EPWM0 TBPRD readback=%u (expect 62500 if clocked)\n",
               (uint32_t)epwm0a_read_tbprd());

  while (true) {
    epwm0a_set_pulse_us(pulses_us[i]);
    trace_printf("pwm: EPWM0_A frame=20000us pulse=%u us\n", pulses_us[i]);
    chThdSleepMilliseconds(3000);
    i = (i + 1U) % 3U;
  }
}

/*
 * Application entry point.
 */
int main(void) {

  trace_init();
  trace_printf("ChibiOS/RT on %s\n", BOARD_NAME);
  trace_printf("port: %s, core: %s\n",
               PORT_ARCHITECTURE_NAME, PORT_CORE_VARIANT_NAME);
  /* Build stamp: remoteproc cannot stop this core without a mailbox
     handshake we do not implement, so "cp + start" silently keeps the
     OLD image running. Printing when this binary was compiled makes a
     stale firmware obvious instead of costing a debugging round.*/
  trace_printf("build: %s %s\n", BUILD_DATE, BUILD_TIME);

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

  (void) chThdCreateStatic(waPwmThread,
                           sizeof(waPwmThread),
                           NORMALPRIO,
                           PwmThread,
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
