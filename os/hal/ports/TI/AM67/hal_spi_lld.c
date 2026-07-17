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
 * @file    TI/AM67/hal_spi_lld.c
 * @brief   AM67 (J722S) SPI subsystem low level driver source.
 * @details Interrupt-driven driver for the TI McSPI in single-channel
 *          master mode, one frame in flight at a time (RX0_FULL paced).
 *          Controller init sequence and channel configuration derived from
 *          NuttX arch/arm/src/am67/am67_mcspi.c (Apache-2.0). The SPI0
 *          pads are muxed here (mode 0) in case the Linux device tree
 *          leaves them unconfigured (the write is idempotent otherwise).
 *
 * @addtogroup SPI
 * @{
 */

#include "hal.h"

#if (HAL_USE_SPI == TRUE) || defined(__DOXYGEN__)

/*===========================================================================*/
/* Driver local definitions.                                                 */
/*===========================================================================*/

/* MCU-domain PADCFG control module: pad registers are write-protected
   until BOTH lock regions are unlocked, and each lock region has TWO
   adjacent KICK registers that both must be written (KICK0 at +0x?008,
   KICK1 at +0x?00C). Writing only one register of a pair leaves the lock
   closed and pad writes are silently ignored.*/
#define AM67_PADCFG_CTRL_BASE       0x04080000U
#define AM67_PADCFG_LOCK0_KICK0     (AM67_PADCFG_CTRL_BASE + 0x1008U)
#define AM67_PADCFG_LOCK0_KICK1     (AM67_PADCFG_CTRL_BASE + 0x100CU)
#define AM67_PADCFG_LOCK1_KICK0     (AM67_PADCFG_CTRL_BASE + 0x5008U)
#define AM67_PADCFG_LOCK1_KICK1     (AM67_PADCFG_CTRL_BASE + 0x500CU)
#define AM67_KICK0_UNLOCK           0x68EF3490U
#define AM67_KICK1_UNLOCK           0xD172BC5AU

#define AM67_PADCFG_BASE            (AM67_PADCFG_CTRL_BASE + 0x4000U)
#define AM67_PAD_SPI0_CS0           (AM67_PADCFG_BASE + 0x0000U)
#define AM67_PAD_SPI0_CLK           (AM67_PADCFG_BASE + 0x0008U)
#define AM67_PAD_SPI0_D0            (AM67_PADCFG_BASE + 0x000CU)
#define AM67_PAD_SPI0_D1            (AM67_PADCFG_BASE + 0x0010U)

#define AM67_PIN_MODE(m)            ((uint32_t)(m))
#define AM67_PIN_PULL_DISABLE       (1U << 16)
#define AM67_PIN_INPUT_ENABLE       (1U << 18)

/* Bound for busy-wait loops on CHSTAT, avoids a silent hard hang if the
   module clock is not running.*/
#define MCSPI_WAIT_LOOPS            1000000U

/*===========================================================================*/
/* Driver exported variables.                                                */
/*===========================================================================*/

/** @brief Main-domain MCSPI0 SPI driver identifier.*/
#if (AM67_SPI_USE_MCSPI0 == TRUE) || defined(__DOXYGEN__)
SPIDriver SPID1;
#endif

/*===========================================================================*/
/* Driver local functions.                                                   */
/*===========================================================================*/

static inline uint32_t spi_getreg(SPIDriver *spip, uint32_t offset) {

  return *(volatile uint32_t *)(spip->base + offset);
}

static inline void spi_putreg(SPIDriver *spip, uint32_t offset,
                              uint32_t value) {

  *(volatile uint32_t *)(spip->base + offset) = value;
}

static inline void reg_write(uint32_t address, uint32_t value) {

  *(volatile uint32_t *)address = value;
}

/*
 * Routes SPI0 CLK/D0/D1/CS0 pads to the McSPI (mux mode 0). CLK and both
 * data pads get the receiver enabled, D0 input is what makes the
 * MOSI-MISO jumper loopback test possible (matches the NuttX pad setup).
 */
static void spi0_pinmux(void) {

  reg_write(AM67_PADCFG_LOCK0_KICK0, AM67_KICK0_UNLOCK);
  reg_write(AM67_PADCFG_LOCK0_KICK1, AM67_KICK1_UNLOCK);
  reg_write(AM67_PADCFG_LOCK1_KICK0, AM67_KICK0_UNLOCK);
  reg_write(AM67_PADCFG_LOCK1_KICK1, AM67_KICK1_UNLOCK);

  reg_write(AM67_PAD_SPI0_CLK,
            AM67_PIN_MODE(0) | AM67_PIN_INPUT_ENABLE | AM67_PIN_PULL_DISABLE);
  reg_write(AM67_PAD_SPI0_D0,
            AM67_PIN_MODE(0) | AM67_PIN_INPUT_ENABLE | AM67_PIN_PULL_DISABLE);
  reg_write(AM67_PAD_SPI0_D1,
            AM67_PIN_MODE(0) | AM67_PIN_INPUT_ENABLE | AM67_PIN_PULL_DISABLE);
  reg_write(AM67_PAD_SPI0_CS0,
            AM67_PIN_MODE(0) | AM67_PIN_PULL_DISABLE);
}

/**
 * @brief   Waits for a CHSTAT flag with a bounded loop.
 *
 * @param[in] spip      pointer to the @p SPIDriver object
 * @param[in] flag      CHSTAT flag to wait for
 * @return              True if the flag was seen, false on timeout.
 */
static bool spi_wait_chstat(SPIDriver *spip, uint32_t flag) {
  uint32_t i;

  for (i = 0U; i < MCSPI_WAIT_LOOPS; i++) {
    if ((spi_getreg(spip, MCSPI_CHSTAT0_OFFSET) & flag) != 0U) {
      return true;
    }
  }
  return false;
}

/**
 * @brief   Controller and channel 0 configuration.
 * @details Sequence from NuttX am67_mcspi_controller_init(): keep the OCP
 *          clock alive, soft reset, then single-channel master mode with
 *          the channel configured from the driver configuration.
 *
 * @param[in] spip      pointer to the @p SPIDriver object
 */
static void mcspi_init(SPIDriver *spip) {
  const SPIConfig *config = spip->config;
  uint32_t chconf, chctrl, div, i;

  spip->ready = false;

  /* No-idle so the interconnect does not gate the functional clock while
     CHSTAT is polled (K3 HL wrapper).*/
  spi_putreg(spip, MCSPI_HL_SYSCONFIG_OFFSET, MCSPI_HL_SYSCONFIG_NOIDLE);

  /* Module soft reset. The wait is bounded: if the module clock is gated
     RESETDONE never rises and an unbounded loop here would freeze the
     whole system, spi_lld_start() runs in a lock zone.*/
  spi_putreg(spip, MCSPI_SYSCONFIG_OFFSET,
             spi_getreg(spip, MCSPI_SYSCONFIG_OFFSET) |
             MCSPI_SYSCONFIG_SOFTRESET);
  for (i = 0U; i < MCSPI_WAIT_LOOPS; i++) {
    if ((spi_getreg(spip, MCSPI_SYSSTATUS_OFFSET) &
         MCSPI_SYSSTATUS_RESETDONE) != 0U) {
      break;
    }
  }
  if (i >= MCSPI_WAIT_LOOPS) {
    return;
  }

  spi_putreg(spip, MCSPI_SYSCONFIG_OFFSET,
             MCSPI_SYSCONFIG_CLKACT_BOTH | MCSPI_SYSCONFIG_SIDLEMODE_NO);

  /* Master, single channel mode, CS controlled by the FORCE bit.*/
  spi_putreg(spip, MCSPI_MODULCTRL_OFFSET, MCSPI_MODULCTRL_SINGLE);

  /* Granular clock divider: SCLK = FCLK / div.*/
  div = (spip->clock + config->speed - 1U) / config->speed;
  if (div < 1U) {
    div = 1U;
  }
  if (div > 4096U) {
    div = 4096U;
  }

  /* Channel 0: RX from D1 (MISO), TX on D0 (MOSI), CS active low,
     8-bit frames, POL/PHA from the standard SPI mode number.*/
  chconf = MCSPI_CHCONF_CLKG | MCSPI_CHCONF_IS | MCSPI_CHCONF_DPE1 |
           MCSPI_CHCONF_EPOL |
           (7U << MCSPI_CHCONF_WL_SHIFT) |
           (((div - 1U) & 0x0FU) << MCSPI_CHCONF_CLKD_SHIFT);
  if ((config->mode & 2U) != 0U) {
    chconf |= MCSPI_CHCONF_POL;
  }
  if ((config->mode & 1U) != 0U) {
    chconf |= MCSPI_CHCONF_PHA;
  }
  spi_putreg(spip, MCSPI_CHCONF0_OFFSET, chconf);

  chctrl = (((div - 1U) >> 4) << MCSPI_CHCTRL_EXTCLK_SHIFT) &
           MCSPI_CHCTRL_EXTCLK_MASK;
  spi_putreg(spip, MCSPI_CHCTRL0_OFFSET, chctrl);

  /* All interrupts off and pending flags cleared, they are enabled per
     transfer.*/
  spi_putreg(spip, MCSPI_IRQENABLE_OFFSET, 0U);
  spi_putreg(spip, MCSPI_IRQSTATUS_OFFSET, 0xFFFFFFFFU);

  /* Channel enabled, idle until FORCE asserts the CS.*/
  spi_putreg(spip, MCSPI_CHCTRL0_OFFSET, chctrl | MCSPI_CHCTRL_EN);

  spip->ready = true;
}

/**
 * @brief   Starts an interrupt-paced transfer.
 * @details Writes the first frame, every RX0_FULL interrupt then reads one
 *          frame back and feeds the next one until done.
 *
 * @param[in] spip      pointer to the @p SPIDriver object
 * @param[in] n         number of frames
 * @param[in] txbuf     transmit buffer or @p NULL for idle frames
 * @param[in] rxbuf     receive buffer or @p NULL to discard
 */
static void spi_start_transfer(SPIDriver *spip, size_t n,
                               const void *txbuf, void *rxbuf) {
  uint32_t first;

  spip->txptr     = (const uint8_t *)txbuf;
  spip->rxptr     = (uint8_t *)rxbuf;
  spip->remaining = n;

  spi_putreg(spip, MCSPI_IRQSTATUS_OFFSET, 0xFFFFFFFFU);
  spi_putreg(spip, MCSPI_IRQENABLE_OFFSET, MCSPI_IRQ_RX0_FULL);

  first = 0xFFU;
  if (spip->txptr != NULL) {
    first = *spip->txptr++;
  }
  spi_putreg(spip, MCSPI_TX0_OFFSET, first);
}

/**
 * @brief   Shared interrupt service.
 *
 * @param[in] spip      pointer to the @p SPIDriver object
 */
static void spi_serve_interrupt(SPIDriver *spip) {

  while ((spi_getreg(spip, MCSPI_IRQSTATUS_OFFSET) &
          MCSPI_IRQ_RX0_FULL) != 0U) {
    uint32_t frame = spi_getreg(spip, MCSPI_RX0_OFFSET);

    spi_putreg(spip, MCSPI_IRQSTATUS_OFFSET, MCSPI_IRQ_RX0_FULL);

    if (spip->rxptr != NULL) {
      *spip->rxptr++ = (uint8_t)frame;
    }

    spip->remaining--;
    if (spip->remaining == 0U) {
      spi_putreg(spip, MCSPI_IRQENABLE_OFFSET, 0U);
      _spi_isr_code(spip);
      return;
    }

    if (spip->txptr != NULL) {
      spi_putreg(spip, MCSPI_TX0_OFFSET, *spip->txptr++);
    }
    else {
      spi_putreg(spip, MCSPI_TX0_OFFSET, 0xFFU);
    }
  }
}

/*===========================================================================*/
/* Driver interrupt handlers.                                                */
/*===========================================================================*/

#if (AM67_SPI_USE_MCSPI0 == TRUE) || defined(__DOXYGEN__)
static bool mcspi0_irq_handler(void *arg) {
  bool preemption_required;

  (void)arg;

  spi_serve_interrupt(&SPID1);

  chSysLockFromISR();
  preemption_required = chSchIsPreemptionRequired();
  chSysUnlockFromISR();

  return preemption_required;
}
#endif

/*===========================================================================*/
/* Driver exported functions.                                                */
/*===========================================================================*/

/**
 * @brief   Low level SPI driver initialization.
 *
 * @notapi
 */
void spi_lld_init(void) {

#if AM67_SPI_USE_MCSPI0 == TRUE
  spiObjectInit(&SPID1);
  SPID1.base  = AM67_MCSPI0_BASE;
  SPID1.clock = AM67_MCSPI0_CLOCK;
  vim_set_handler(AM67_MCSPI0_IRQ, mcspi0_irq_handler, NULL);
  vim_set_priority(AM67_MCSPI0_IRQ, AM67_SPI_MCSPI0_IRQ_PRIORITY);
#endif
}

/**
 * @brief   Configures and activates the SPI peripheral.
 *
 * @param[in] spip      pointer to the @p SPIDriver object
 *
 * @notapi
 */
void spi_lld_start(SPIDriver *spip) {

#if AM67_SPI_USE_MCSPI0 == TRUE
  if (spip == &SPID1) {
    spi0_pinmux();
    mcspi_init(spip);
    vim_enable_irq(AM67_MCSPI0_IRQ);
  }
#endif
}

/**
 * @brief   Deactivates the SPI peripheral.
 *
 * @param[in] spip      pointer to the @p SPIDriver object
 *
 * @notapi
 */
void spi_lld_stop(SPIDriver *spip) {

  if (spip->state == SPI_READY) {
#if AM67_SPI_USE_MCSPI0 == TRUE
    if (spip == &SPID1) {
      vim_disable_irq(AM67_MCSPI0_IRQ);
    }
#endif
    spi_putreg(spip, MCSPI_IRQENABLE_OFFSET, 0U);
    spi_putreg(spip, MCSPI_CHCTRL0_OFFSET,
               spi_getreg(spip, MCSPI_CHCTRL0_OFFSET) & ~MCSPI_CHCTRL_EN);
  }
}

#if (SPI_SELECT_MODE == SPI_SELECT_MODE_LLD) || defined(__DOXYGEN__)
/**
 * @brief   Asserts the slave select signal and prepares for transfers.
 * @details The FORCE bit drives the CS0 pad low (EPOL selects active low).
 *
 * @param[in] spip      pointer to the @p SPIDriver object
 *
 * @notapi
 */
void spi_lld_select(SPIDriver *spip) {

  spi_putreg(spip, MCSPI_CHCONF0_OFFSET,
             spi_getreg(spip, MCSPI_CHCONF0_OFFSET) | MCSPI_CHCONF_FORCE);
}

/**
 * @brief   Deasserts the slave select signal.
 *
 * @param[in] spip      pointer to the @p SPIDriver object
 *
 * @notapi
 */
void spi_lld_unselect(SPIDriver *spip) {

  spi_putreg(spip, MCSPI_CHCONF0_OFFSET,
             spi_getreg(spip, MCSPI_CHCONF0_OFFSET) & ~MCSPI_CHCONF_FORCE);
}
#endif

/**
 * @brief   Ignores data on the SPI bus.
 * @details This asynchronous function starts the transmission of a series
 *          of idle words on the SPI bus and ignores the received data.
 *
 * @param[in] spip      pointer to the @p SPIDriver object
 * @param[in] n         number of words to be ignored
 *
 * @notapi
 */
void spi_lld_ignore(SPIDriver *spip, size_t n) {

  spi_start_transfer(spip, n, NULL, NULL);
}

/**
 * @brief   Exchanges data on the SPI bus.
 * @details This asynchronous function starts a simultaneous
 *          transmit/receive operation.
 *
 * @param[in] spip      pointer to the @p SPIDriver object
 * @param[in] n         number of words to be exchanged
 * @param[in] txbuf     the pointer to the transmit buffer
 * @param[out] rxbuf    the pointer to the receive buffer
 *
 * @notapi
 */
void spi_lld_exchange(SPIDriver *spip, size_t n,
                      const void *txbuf, void *rxbuf) {

  spi_start_transfer(spip, n, txbuf, rxbuf);
}

/**
 * @brief   Sends data over the SPI bus.
 *
 * @param[in] spip      pointer to the @p SPIDriver object
 * @param[in] n         number of words to send
 * @param[in] txbuf     the pointer to the transmit buffer
 *
 * @notapi
 */
void spi_lld_send(SPIDriver *spip, size_t n, const void *txbuf) {

  spi_start_transfer(spip, n, txbuf, NULL);
}

/**
 * @brief   Receives data from the SPI bus.
 *
 * @param[in] spip      pointer to the @p SPIDriver object
 * @param[in] n         number of words to receive
 * @param[out] rxbuf    the pointer to the receive buffer
 *
 * @notapi
 */
void spi_lld_receive(SPIDriver *spip, size_t n, void *rxbuf) {

  spi_start_transfer(spip, n, NULL, rxbuf);
}

/**
 * @brief   Aborts the ongoing SPI operation, if any.
 *
 * @param[in] spip      pointer to the @p SPIDriver object
 *
 * @notapi
 */
void spi_lld_abort(SPIDriver *spip) {

  spi_putreg(spip, MCSPI_IRQENABLE_OFFSET, 0U);
  spi_putreg(spip, MCSPI_IRQSTATUS_OFFSET, 0xFFFFFFFFU);
  spip->remaining = 0U;
}

/**
 * @brief   Exchanges one frame using a polled synchronous wait.
 * @details Same sequence as NuttX am67_mcspi_transfer_word(): wait for TX
 *          register empty, write, wait for RX register full, read.
 *
 * @param[in] spip      pointer to the @p SPIDriver object
 * @param[in] frame     the data frame to send
 * @return              The received data frame.
 *
 * @notapi
 */
uint16_t spi_lld_polled_exchange(SPIDriver *spip, uint16_t frame) {

  if (!spi_wait_chstat(spip, MCSPI_CHSTAT_TXS)) {
    return 0U;
  }
  spi_putreg(spip, MCSPI_TX0_OFFSET, (uint32_t)frame);

  if (!spi_wait_chstat(spip, MCSPI_CHSTAT_RXS)) {
    return 0U;
  }
  return (uint16_t)spi_getreg(spip, MCSPI_RX0_OFFSET);
}

#endif /* HAL_USE_SPI == TRUE */

/** @} */
