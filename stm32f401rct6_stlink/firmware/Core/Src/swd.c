/**
  ******************************************************************************
  * @file    swd.c
  * @brief   Low-level ADIv5 SWD driver for STM32F401RCT6 (ported from ESP32
  *          ESP_OTA_STM32 and extended for ARM CMSIS-DAP v1.10).
  *
  * Pin mapping (from main.h / CubeMX):
  *   - MCU_SWDIO    : PA4 (Bidirectional Push-Pull / Input with Pull-Up)
  *   - MCU_SWDCLK   : PB4 (Output Push-Pull)
  *   - MCU_SWDRESET : PB8 (Output Open-Drain, HIGH = released, LOW = reset)
  ******************************************************************************
  */

#include "swd.h"

/* Direct register access macros for PA4 (SWDIO), PB4 (SWCLK), PB8 (nRESET) */
#define SWDIO_PIN_POS      4U
#define SWDIO_MODER_MASK   (3UL << (SWDIO_PIN_POS * 2U))
#define SWDIO_MODER_OUT    (1UL << (SWDIO_PIN_POS * 2U))

#define DIO_HIGH()         (MCU_SWDIO_GPIO_Port->BSRR = MCU_SWDIO_Pin)
#define DIO_LOW()          (MCU_SWDIO_GPIO_Port->BSRR = ((uint32_t)MCU_SWDIO_Pin << 16U))
#define DIO_READ()         (((MCU_SWDIO_GPIO_Port->IDR & MCU_SWDIO_Pin) != 0U) ? 1U : 0U)
#define DIO_DRIVE()        do { \
                             MCU_SWDIO_GPIO_Port->MODER = (MCU_SWDIO_GPIO_Port->MODER & ~SWDIO_MODER_MASK) | SWDIO_MODER_OUT; \
                           } while (0)
#define DIO_FLOAT()        do { \
                             MCU_SWDIO_GPIO_Port->MODER &= ~SWDIO_MODER_MASK; \
                           } while (0)

#define CLK_HIGH()         (MCU_SWDCLK_GPIO_Port->BSRR = MCU_SWDCLK_Pin)
#define CLK_LOW()          (MCU_SWDCLK_GPIO_Port->BSRR = ((uint32_t)MCU_SWDCLK_Pin << 16U))
#define CLK_READ()         (((MCU_SWDCLK_GPIO_Port->IDR & MCU_SWDCLK_Pin) != 0U) ? 1U : 0U)

#define RST_LOW()          (MCU_SWDRESET_GPIO_Port->BSRR = ((uint32_t)MCU_SWDRESET_Pin << 16U))
#define RST_RELEASE()      (MCU_SWDRESET_GPIO_Port->BSRR = MCU_SWDRESET_Pin)
#define RST_READ()         (((MCU_SWDRESET_GPIO_Port->IDR & MCU_SWDRESET_Pin) != 0U) ? 1U : 0U)

/* CMSIS-DAP SWJ Pin bit definitions */
#define DAP_SWJ_SWCLK_TCK  (1U << 0)
#define DAP_SWJ_SWDIO_TMS  (1U << 1)
#define DAP_SWJ_TDI        (1U << 2)
#define DAP_SWJ_TDO        (1U << 3)
#define DAP_SWJ_nTRST      (1U << 5)
#define DAP_SWJ_nRESET     (1U << 7)

#define SWD_DP_TIMEOUT_MS  50U
#define SWD_AP_TIMEOUT_MS  1000U

static uint32_t s_halfPeriodCycles = 30U;   /* ~1 MHz default at 60 MHz HCLK */
static uint8_t  s_turnaround       = 1U;    /* 1..4 clocks */
static bool     s_dataPhase        = false; /* Generate data phase on WAIT/FAULT */
static uint8_t  s_idleCycles       = 2U;    /* Extra idle clocks after transfer */
static uint16_t s_waitRetry        = 100U;  /* Retries on ACK == WAIT */
static uint16_t s_matchRetry       = 0U;    /* Retries on Value Match */
static volatile bool s_abortFlag   = false;

static int s_lastError = 0;

/* -------------------------------------------------------------------------- */
/* Timing via Cortex-M4 DWT Cycle Counter                                     */
/* -------------------------------------------------------------------------- */

void swdTimeInit(void)
{
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0U;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
  swdSetClockHz(1000000U); /* Default 1 MHz SWCLK */
}

uint32_t swdGetTimestamp(void)
{
  return DWT->CYCCNT;
}

void swdDelayUs(uint32_t us)
{
  uint32_t start = DWT->CYCCNT;
  uint32_t ticks = us * (SystemCoreClock / 1000000U);
  while ((DWT->CYCCNT - start) < ticks)
  {
    __NOP();
  }
}

static inline void swdDelay(void)
{
  uint32_t n = s_halfPeriodCycles;
  while (n--)
  {
    __NOP();
  }
}

void swdSetClockHz(uint32_t hz)
{
  if (hz == 0U)
  {
    hz = 1000000U;
  }
  uint32_t halfCycles = SystemCoreClock / (hz * 2U);
  if (halfCycles <= 4U)
  {
    s_halfPeriodCycles = 1U;
  }
  else
  {
    s_halfPeriodCycles = (halfCycles - 4U) / 3U;
    if (s_halfPeriodCycles == 0U)
    {
      s_halfPeriodCycles = 1U;
    }
  }
}

void swdConfigure(uint8_t turnaround, bool dataPhase)
{
  if (turnaround < 1U) turnaround = 1U;
  if (turnaround > 4U) turnaround = 4U;
  s_turnaround = turnaround;
  s_dataPhase  = dataPhase;
}

void swdTransferConfigure(uint8_t idleCycles, uint16_t waitRetry, uint16_t matchRetry)
{
  s_idleCycles = idleCycles;
  s_waitRetry  = (waitRetry > 0U) ? waitRetry : 100U;
  s_matchRetry = matchRetry;
}

uint16_t swdGetWaitRetry(void)  { return s_waitRetry; }
uint16_t swdGetMatchRetry(void) { return s_matchRetry; }
void swdSetAbortFlag(bool abort) { s_abortFlag = abort; }
bool swdGetAbortFlag(void)       { return s_abortFlag; }

/* -------------------------------------------------------------------------- */
/* Low-level Bit-bang Primitives (ADIv5 B4.2)                                 */
/* -------------------------------------------------------------------------- */

static inline void clockPulse(void)
{
  CLK_HIGH();
  swdDelay();
  CLK_LOW();
  swdDelay();
}

static inline void writeBit(uint32_t bit)
{
  if (bit)
  {
    DIO_HIGH();
  }
  else
  {
    DIO_LOW();
  }
  swdDelay();
  clockPulse();
}

static inline uint32_t readBit(void)
{
  uint32_t bit = DIO_READ();
  clockPulse();
  return bit;
}

static void writeBits(uint32_t value, int count)
{
  for (int i = 0; i < count; i++)
  {
    writeBit((value >> i) & 1U);
  }
}

static uint32_t readBits(int count)
{
  uint32_t v = 0U;
  for (int i = 0; i < count; i++)
  {
    v |= (readBit() << i);
  }
  return v;
}

static void turnaroundCycles(uint8_t n)
{
  for (uint8_t i = 0U; i < n; i++)
  {
    clockPulse();
  }
}

static void idleCycles(uint8_t n)
{
  DIO_DRIVE();
  DIO_LOW();
  for (uint8_t i = 0U; i < n; i++)
  {
    clockPulse();
  }
}

/* -------------------------------------------------------------------------- */
/* GPIO Init / Release / Reset                                                */
/* -------------------------------------------------------------------------- */

void swdInit(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /* PA4: SWDIO Output Push-Pull with Pull-Up, Very High Speed */
  GPIO_InitStruct.Pin   = MCU_SWDIO_Pin;
  GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull  = GPIO_PULLUP;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  HAL_GPIO_Init(MCU_SWDIO_GPIO_Port, &GPIO_InitStruct);

  /* PB4: SWCLK Output Push-Pull, Very High Speed */
  GPIO_InitStruct.Pin   = MCU_SWDCLK_Pin;
  GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull  = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  HAL_GPIO_Init(MCU_SWDCLK_GPIO_Port, &GPIO_InitStruct);

  /* PB8: nRESET Output Open-Drain, default released (HIGH) */
  RST_RELEASE();
  GPIO_InitStruct.Pin   = MCU_SWDRESET_Pin;
  GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_OD;
  GPIO_InitStruct.Pull  = GPIO_PULLUP;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(MCU_SWDRESET_GPIO_Port, &GPIO_InitStruct);

  CLK_LOW();
  DIO_HIGH();
  DIO_DRIVE();
  s_lastError = 0;
}

void swdRelease(void)
{
  DIO_FLOAT();

  GPIO_InitTypeDef GPIO_InitStruct = {0};
  GPIO_InitStruct.Pin  = MCU_SWDCLK_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(MCU_SWDCLK_GPIO_Port, &GPIO_InitStruct);

  /* Ensure PB8 (nRESET) stays Open-Drain with internal Pull-Up and released (HIGH) */
  RST_RELEASE();
  GPIO_InitStruct.Pin   = MCU_SWDRESET_Pin;
  GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_OD;
  GPIO_InitStruct.Pull  = GPIO_PULLUP;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(MCU_SWDRESET_GPIO_Port, &GPIO_InitStruct);
}

void swdSetReset(bool asserted)
{
  if (asserted)
  {
    RST_LOW();
  }
  else
  {
    RST_RELEASE();
  }
}

bool swdGetResetState(void)
{
  return (RST_READ() != 0U);
}

void swdHardResetPulse(uint32_t lowMs, uint32_t settleMs)
{
  RST_LOW();
  HAL_Delay(lowMs);
  RST_RELEASE();
  HAL_Delay(settleMs);
}

uint8_t swdPinsControl(uint8_t pinOut, uint8_t pinSelect, uint32_t waitUs)
{
  if (pinSelect & DAP_SWJ_SWCLK_TCK)
  {
    if (pinOut & DAP_SWJ_SWCLK_TCK) CLK_HIGH();
    else                            CLK_LOW();
  }
  if (pinSelect & DAP_SWJ_SWDIO_TMS)
  {
    DIO_DRIVE();
    if (pinOut & DAP_SWJ_SWDIO_TMS) DIO_HIGH();
    else                            DIO_LOW();
  }
  if (pinSelect & DAP_SWJ_nRESET)
  {
    if (pinOut & DAP_SWJ_nRESET) RST_RELEASE();
    else                         RST_LOW();
  }

  if (waitUs > 0U)
  {
    if (waitUs > 3000000U)
    {
      waitUs = 3000000U;
    }
    uint32_t start = DWT->CYCCNT;
    uint32_t ticks = waitUs * (SystemCoreClock / 1000000U);
    uint8_t mask = pinSelect & (DAP_SWJ_SWCLK_TCK | DAP_SWJ_SWDIO_TMS | DAP_SWJ_nRESET);
    do
    {
      uint8_t cur = 0U;
      if (CLK_READ()) cur |= DAP_SWJ_SWCLK_TCK;
      if (DIO_READ()) cur |= DAP_SWJ_SWDIO_TMS;
      if (RST_READ()) cur |= DAP_SWJ_nRESET;
      if (((cur ^ pinOut) & mask) == 0U)
      {
        break;
      }
    } while ((DWT->CYCCNT - start) < ticks);
  }

  uint8_t state = 0U;
  if (CLK_READ()) state |= DAP_SWJ_SWCLK_TCK;
  if (DIO_READ()) state |= DAP_SWJ_SWDIO_TMS;
  if (RST_READ()) state |= DAP_SWJ_nRESET;
  return state;
}

/* -------------------------------------------------------------------------- */
/* Bit Sequences for CMSIS-DAP                                                */
/* -------------------------------------------------------------------------- */

void swdSwjSequence(uint32_t bitCount, const uint8_t *data)
{
  DIO_DRIVE();
  uint32_t byteIdx = 0U;
  uint8_t  curByte = 0U;
  for (uint32_t i = 0U; i < bitCount; i++)
  {
    if ((i & 7U) == 0U)
    {
      curByte = data[byteIdx++];
    }
    writeBit(curByte & 1U);
    curByte >>= 1U;
  }
}

void swdSequenceOut(uint32_t bitCount, const uint8_t *data)
{
  swdSwjSequence(bitCount, data);
}

void swdSequenceIn(uint32_t bitCount, uint8_t *data)
{
  DIO_FLOAT();
  uint32_t byteIdx = 0U;
  uint8_t  curByte = 0U;
  for (uint32_t i = 0U; i < bitCount; i++)
  {
    uint32_t bit = readBit();
    curByte |= (uint8_t)(bit << (i & 7U));
    if ((i & 7U) == 7U || (i + 1U) == bitCount)
    {
      data[byteIdx++] = curByte;
      curByte = 0U;
    }
  }
}

/* -------------------------------------------------------------------------- */
/* SWD Raw Transfer & CMSIS-DAP Transfer                                      */
/* -------------------------------------------------------------------------- */

static uint8_t buildRequest(bool ap, bool rd, uint8_t addr)
{
  uint8_t req = 0x81U; /* Start (bit 0) + Park (bit 7) */
  if (ap) req |= 0x02U;
  if (rd) req |= 0x04U;
  if (addr & 0x04U) req |= 0x08U;
  if (addr & 0x08U) req |= 0x10U;
  if (__builtin_popcount(req & 0x1EU) & 1) req |= 0x20U;
  return req;
}

/* Single raw SWD transaction (returns DAP_TRANSFER_OK / WAIT / FAULT / ERROR) */
static uint8_t rawTransferOnce(uint8_t reqPacket, uint32_t *data)
{
  const bool rd = (reqPacket & 0x04U) != 0U;

  DIO_DRIVE();
  writeBits(reqPacket, 8);

  DIO_FLOAT();
  turnaroundCycles(s_turnaround);
  uint8_t ack = (uint8_t)readBits(3);

  if (ack == SWD_ACK_OK)
  {
    if (rd)
    {
      uint32_t v   = readBits(32);
      uint32_t par = readBit();
      turnaroundCycles(s_turnaround);
      if (s_idleCycles > 0U)
      {
        idleCycles(s_idleCycles);
      }
      DIO_DRIVE();
      DIO_LOW();
      if ((uint32_t)(__builtin_popcount(v) & 1) != par)
      {
        s_lastError = SWD_ERR_PARITY;
        return (uint8_t)(DAP_TRANSFER_OK | DAP_TRANSFER_ERROR);
      }
      if (data != NULL)
      {
        *data = v;
      }
    }
    else
    {
      turnaroundCycles(s_turnaround);
      DIO_DRIVE();
      uint32_t v = (data != NULL) ? *data : 0U;
      writeBits(v, 32);
      writeBit((uint32_t)(__builtin_popcount(v) & 1));
      if (s_idleCycles > 0U)
      {
        idleCycles(s_idleCycles);
      }
      DIO_LOW();
    }
    s_lastError = 0;
    return DAP_TRANSFER_OK;
  }

  if (ack == SWD_ACK_WAIT || ack == SWD_ACK_FAULT)
  {
    if (s_dataPhase && rd)
    {
      for (int i = 0; i < 33; i++)
      {
        clockPulse();
      }
    }
    turnaroundCycles(s_turnaround);
    DIO_DRIVE();
    if (s_dataPhase && !rd)
    {
      DIO_LOW();
      for (int i = 0; i < 33; i++)
      {
        clockPulse();
      }
    }
    DIO_LOW();
    s_lastError = (int)ack;
    return ack;
  }

  /* Protocol error / No ACK: back off data phase + turnaround */
  for (uint8_t i = 0U; i < (uint8_t)(33U + s_turnaround); i++)
  {
    clockPulse();
  }
  DIO_DRIVE();
  DIO_LOW();
  s_lastError = SWD_ERR_NOACK;
  return DAP_TRANSFER_ERROR;
}

uint8_t swdDapTransfer(uint8_t reqCmd, uint32_t *data)
{
  const bool ap = (reqCmd & DAP_TRANSFER_APnDP) != 0U;
  const bool rd = (reqCmd & DAP_TRANSFER_RnW) != 0U;
  const uint8_t addr = (uint8_t)(reqCmd & (DAP_TRANSFER_A2 | DAP_TRANSFER_A3));
  const uint8_t pkt  = buildRequest(ap, rd, addr);

  uint16_t retry = s_waitRetry;
  uint8_t ack;
  do
  {
    ack = rawTransferOnce(pkt, data);
    if (ack != DAP_TRANSFER_WAIT)
    {
      break;
    }
  } while (retry-- > 0U && !s_abortFlag);

  return ack;
}

/* -------------------------------------------------------------------------- */
/* High-level ADIv5 SWD API (ESP_OTA_STM32 compatible)                        */
/* -------------------------------------------------------------------------- */

static void lineResetOnly(void)
{
  DIO_DRIVE();
  DIO_HIGH();
  for (int i = 0; i < 60; i++)
  {
    clockPulse();
  }
}

void swdLineReset(void)
{
  lineResetOnly();
  idleCycles(4U);
}

void swdJtagToSwd(void)
{
  DIO_DRIVE();
  writeBits(0xE79EU, 16);
}

void swdClearErrors(void)
{
  uint32_t v = ABORT_CLEAR;
  rawTransferOnce(buildRequest(false, false, DP_ABORT), &v);
}

static int swdTransferTimeout(bool ap, bool rd, uint8_t addr, uint32_t *data, uint32_t timeoutMs)
{
  const uint8_t pkt = buildRequest(ap, rd, addr);
  const uint32_t start = HAL_GetTick();

  for (;;)
  {
    uint8_t r = rawTransferOnce(pkt, data);
    if (r == DAP_TRANSFER_OK)
    {
      s_lastError = 0;
      return SWD_ACK_OK;
    }
    if (r == (uint8_t)(DAP_TRANSFER_OK | DAP_TRANSFER_ERROR))
    {
      s_lastError = SWD_ERR_PARITY;
      return SWD_ERR_PARITY;
    }
    if (r == DAP_TRANSFER_WAIT)
    {
      if ((HAL_GetTick() - start) > timeoutMs)
      {
        swdClearErrors();
        s_lastError = SWD_ERR_TIMEOUT;
        return SWD_ERR_TIMEOUT;
      }
      continue;
    }
    if (r == DAP_TRANSFER_FAULT)
    {
      swdClearErrors();
      s_lastError = SWD_ACK_FAULT;
      return SWD_ACK_FAULT;
    }
    s_lastError = SWD_ERR_NOACK;
    return SWD_ERR_NOACK;
  }
}

bool swdReadDP(uint8_t addr, uint32_t *out)
{
  return swdTransferTimeout(false, true, addr, out, SWD_DP_TIMEOUT_MS) == SWD_ACK_OK;
}

bool swdWriteDP(uint8_t addr, uint32_t val)
{
  return swdTransferTimeout(false, false, addr, &val, SWD_DP_TIMEOUT_MS) == SWD_ACK_OK;
}

bool swdReadAP(uint8_t addr, uint32_t *out)
{
  uint32_t dummy = 0U;
  if (swdTransferTimeout(true, true, addr, &dummy, SWD_AP_TIMEOUT_MS) != SWD_ACK_OK)
  {
    return false;
  }
  return swdReadDP(DP_RDBUFF, out);
}

bool swdWriteAP(uint8_t addr, uint32_t val)
{
  return swdTransferTimeout(true, false, addr, &val, SWD_AP_TIMEOUT_MS) == SWD_ACK_OK;
}

uint32_t swdConnect(void)
{
  for (int attempt = 0; attempt < 4; attempt++)
  {
    lineResetOnly();
    swdJtagToSwd();
    swdLineReset();
    uint32_t id = 0U;
    if (swdReadDP(DP_IDCODE, &id) && id != 0U && id != 0xFFFFFFFFUL)
    {
      swdClearErrors();
      swdWriteDP(DP_SELECT, 0U);
      return id;
    }
    HAL_Delay(2);
  }
  if (s_lastError == 0)
  {
    s_lastError = SWD_ERR_NOACK;
  }
  return 0U;
}

bool swdPowerUp(void)
{
  if (!swdWriteDP(DP_CTRL_STAT, 0x50000000UL))
  {
    return false;
  }
  const uint32_t start = HAL_GetTick();
  bool up = false;
  while ((HAL_GetTick() - start) < 200U)
  {
    uint32_t st = 0U;
    if (swdReadDP(DP_CTRL_STAT, &st) && (st & 0xA0000000UL) == 0xA0000000UL)
    {
      up = true;
      break;
    }
    HAL_Delay(1);
  }
  if (!up)
  {
    return false;
  }
  return swdWriteAP(AP_CSW, CSW_32BIT_INC);
}

bool memRead32(uint32_t addr, uint32_t *out)
{
  return swdWriteAP(AP_TAR, addr) && swdReadAP(AP_DRW, out);
}

bool memWrite32(uint32_t addr, uint32_t val)
{
  if (!swdWriteAP(AP_TAR, addr) || !swdWriteAP(AP_DRW, val))
  {
    return false;
  }
  uint32_t dummy = 0U;
  return swdReadDP(DP_RDBUFF, &dummy);
}

bool memWrite16(uint32_t addr, uint16_t val)
{
  if (!swdWriteAP(AP_CSW, CSW_16BIT_INC))
  {
    return false;
  }
  /* In ADIv5 byte-lane packing, 16-bit write at addr uses lane (addr & 2) */
  uint32_t packed = ((addr & 2U) != 0U) ? ((uint32_t)val << 16U) : (uint32_t)val;
  bool ok = swdWriteAP(AP_TAR, addr) && swdWriteAP(AP_DRW, packed);
  uint32_t dummy = 0U;
  if (ok)
  {
    ok = swdReadDP(DP_RDBUFF, &dummy);
  }
  swdWriteAP(AP_CSW, CSW_32BIT_INC);
  return ok;
}

static size_t wordsToBoundary(uint32_t addr, size_t count)
{
  size_t room = (0x400U - (addr & 0x3FFU)) / 4U;
  return (count < room) ? count : room;
}

bool memWriteBlock(uint32_t addr, const uint32_t *words, size_t count)
{
  while (count > 0U)
  {
    size_t n = wordsToBoundary(addr, count);
    if (!swdWriteAP(AP_TAR, addr))
    {
      return false;
    }
    for (size_t i = 0U; i < n; i++)
    {
      if (!swdWriteAP(AP_DRW, words[i]))
      {
        return false;
      }
    }
    uint32_t dummy = 0U;
    if (!swdReadDP(DP_RDBUFF, &dummy))
    {
      return false;
    }
    addr  += (uint32_t)(n * 4U);
    words += n;
    count -= n;
  }
  return true;
}

bool memReadBlock(uint32_t addr, uint32_t *words, size_t count)
{
  while (count > 0U)
  {
    size_t n = wordsToBoundary(addr, count);
    if (!swdWriteAP(AP_TAR, addr))
    {
      return false;
    }
    uint32_t v = 0U;
    if (swdTransferTimeout(true, true, AP_DRW, &v, SWD_AP_TIMEOUT_MS) != SWD_ACK_OK)
    {
      return false;
    }
    for (size_t i = 1U; i < n; i++)
    {
      if (swdTransferTimeout(true, true, AP_DRW, &words[i - 1U], SWD_AP_TIMEOUT_MS) != SWD_ACK_OK)
      {
        return false;
      }
    }
    if (!swdReadDP(DP_RDBUFF, &words[n - 1U]))
    {
      return false;
    }
    addr  += (uint32_t)(n * 4U);
    words += n;
    count -= n;
  }
  return true;
}

int swdLastError(void)
{
  return s_lastError;
}

const char *swdErrorStr(int err)
{
  switch (err)
  {
    case 0:               return "OK";
    case SWD_ACK_WAIT:    return "WAIT";
    case SWD_ACK_FAULT:   return "FAULT";
    case SWD_ERR_PARITY:  return "parity error";
    case SWD_ERR_NOACK:   return "no ACK (target not connected?)";
    case SWD_ERR_TIMEOUT: return "WAIT timeout";
    default:              return "unknown";
  }
}
