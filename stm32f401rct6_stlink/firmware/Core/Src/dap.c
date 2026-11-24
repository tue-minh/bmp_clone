/**
  ******************************************************************************
  * @file    dap.c
  * @brief   ARM CMSIS-DAP v1.10 Protocol Implementation over USB Custom HID
  *          Supports OpenOCD, pyOCD, Probe-rs, Keil MDK, STM32CubeIDE, etc.
  *          for flashing and debugging STM32F1, F4, G4, L4, H7 and any Cortex-M.
  ******************************************************************************
  */

#include "dap.h"
#include "swd.h"
#include "main.h"
#include "led_status.h"
#include "usb_device.h"
#include "usbd_customhid.h"
#include <string.h>

extern USBD_HandleTypeDef hUsbDeviceFS;

/* CMSIS-DAP Info IDs */
#define DAP_ID_VENDOR             0x01U
#define DAP_ID_PRODUCT            0x02U
#define DAP_ID_SER_NUM            0x03U
#define DAP_ID_FW_VER             0x04U
#define DAP_ID_DEVICE_VENDOR      0x05U
#define DAP_ID_DEVICE_NAME        0x06U
#define DAP_ID_CAPABILITIES       0xF0U
#define DAP_ID_TIMESTAMP_CLOCK    0xF1U
#define DAP_ID_SWO_BUFFER_SIZE    0xFDU
#define DAP_ID_PACKET_COUNT       0xFEU
#define DAP_ID_PACKET_SIZE        0xFFU

static const char kVendorStr[]  = "TUE";
static const char kProductStr[] = "TUE CMSIS-DAP";
static const char kFwVerStr[]   = "1.10";

static uint8_t  s_debugPort = DAP_PORT_DISABLED;
static uint32_t s_matchMask = 0x00000000UL;
static uint8_t  s_hidClassId = 0U;

/* Ring buffer for 64-byte HID packets between USB ISR and main loop */
static uint8_t           s_rxQueue[DAP_PACKET_COUNT][DAP_PACKET_SIZE];
static volatile uint8_t  s_rxWriteIdx = 0U;
static volatile uint8_t  s_rxReadIdx  = 0U;
static volatile uint8_t  s_rxCount    = 0U;
static volatile bool     s_rxPaused   = false;
static __ALIGN_BEGIN uint8_t s_txBuf[DAP_PACKET_SIZE] __ALIGN_END;

/* -------------------------------------------------------------------------- */
/* Helpers                                                                    */
/* -------------------------------------------------------------------------- */

static inline uint16_t readLE16(const uint8_t *p)
{
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8U);
}

static inline uint32_t readLE32(const uint8_t *p)
{
  return (uint32_t)p[0] |
         ((uint32_t)p[1] << 8U) |
         ((uint32_t)p[2] << 16U) |
         ((uint32_t)p[3] << 24U);
}

static inline void writeLE16(uint8_t *p, uint16_t v)
{
  p[0] = (uint8_t)(v & 0xFFU);
  p[1] = (uint8_t)((v >> 8U) & 0xFFU);
}

static inline void writeLE32(uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)(v & 0xFFU);
  p[1] = (uint8_t)((v >> 8U) & 0xFFU);
  p[2] = (uint8_t)((v >> 16U) & 0xFFU);
  p[3] = (uint8_t)((v >> 24U) & 0xFFU);
}

static void formatSerialUid(char *out, size_t maxLen)
{
  static const char kHex[] = "0123456789ABCDEF";
  const uint8_t *uid = (const uint8_t *)UID_BASE;
  size_t pos = 0U;
  for (size_t i = 0U; i < 12U && (pos + 2U) < maxLen; i++)
  {
    out[pos++] = kHex[(uid[i] >> 4U) & 0x0FU];
    out[pos++] = kHex[uid[i] & 0x0FU];
  }
  out[pos] = '\0';
}

/* -------------------------------------------------------------------------- */
/* Individual CMSIS-DAP Command Handlers                                      */
/* -------------------------------------------------------------------------- */

static uint32_t dapInfo(const uint8_t *req, uint8_t *resp)
{
  uint8_t id = req[0];
  uint8_t len = 0U;

  switch (id)
  {
    case DAP_ID_VENDOR:
      len = (uint8_t)sizeof(kVendorStr);
      memcpy(&resp[1], kVendorStr, len);
      break;

    case DAP_ID_PRODUCT:
      len = (uint8_t)sizeof(kProductStr);
      memcpy(&resp[1], kProductStr, len);
      break;

    case DAP_ID_SER_NUM:
    {
      char ser[26];
      formatSerialUid(ser, sizeof(ser));
      len = (uint8_t)(strlen(ser) + 1U);
      memcpy(&resp[1], ser, len);
      break;
    }

    case DAP_ID_FW_VER:
      len = (uint8_t)sizeof(kFwVerStr);
      memcpy(&resp[1], kFwVerStr, len);
      break;

    case DAP_ID_DEVICE_VENDOR:
    case DAP_ID_DEVICE_NAME:
      len = 0U;
      break;

    case DAP_ID_CAPABILITIES:
      /* Bit 0: SWD supported, Bit 4: Atomic Commands supported */
      resp[1] = 0x11U;
      len = 1U;
      break;

    case DAP_ID_TIMESTAMP_CLOCK:
      writeLE32(&resp[1], SystemCoreClock);
      len = 4U;
      break;

    case DAP_ID_PACKET_COUNT:
      resp[1] = DAP_PACKET_COUNT;
      len = 1U;
      break;

    case DAP_ID_PACKET_SIZE:
      writeLE16(&resp[1], DAP_PACKET_SIZE);
      len = 2U;
      break;

    default:
      len = 0U;
      break;
  }

  resp[0] = len;
  return (1U << 16U) | (1U + (uint32_t)len);
}

static uint32_t dapHostStatus(const uint8_t *req, uint8_t *resp)
{
  uint8_t type   = req[0];
  uint8_t status = req[1];
  if (type == 0U)
  {
    /* Type 0: Connect LED - Host debugger is connected/active */
    ledStatusSetState((status != 0U) ? LED_STATE_CONNECTED : LED_STATE_IDLE);
  }
  else if (type == 1U)
  {
    /* Type 1: Running LED - Target CPU is Running (1) or Halted (0) */
    ledStatusSetState((status != 0U) ? LED_STATE_CONNECTED : LED_STATE_DEBUG_HALTED);
  }
  resp[0] = DAP_OK;
  return (2U << 16U) | 1U;
}

static uint32_t dapConnect(const uint8_t *req, uint8_t *resp)
{
  uint8_t port = req[0];
  if (port == DAP_PORT_AUTODETECT)
  {
    port = DAP_PORT_SWD;
  }

  if (port == DAP_PORT_SWD)
  {
    s_debugPort = DAP_PORT_SWD;
    swdInit();
    ledStatusSetState(LED_STATE_CONNECTED);
    resp[0] = DAP_PORT_SWD;
  }
  else
  {
    resp[0] = DAP_PORT_DISABLED;
  }
  return (1U << 16U) | 1U;
}

static uint32_t dapDisconnect(uint8_t *resp)
{
  s_debugPort = DAP_PORT_DISABLED;
  swdRelease();
  ledStatusSetState(LED_STATE_IDLE);
  resp[0] = DAP_OK;
  return (0U << 16U) | 1U;
}

static uint32_t dapTransferConfigure(const uint8_t *req, uint8_t *resp)
{
  uint8_t  idleCycles = req[0];
  uint16_t waitRetry  = readLE16(&req[1]);
  uint16_t matchRetry = readLE16(&req[3]);
  swdTransferConfigure(idleCycles, waitRetry, matchRetry);
  resp[0] = DAP_OK;
  return (5U << 16U) | 1U;
}

static uint32_t dapTransfer(const uint8_t *req, uint8_t *resp)
{
  const uint8_t *reqPtr  = req;
  uint8_t       *respPtr = resp;

  uint8_t dapIdx      = *reqPtr++;
  (void)dapIdx;
  uint8_t reqCount    = *reqPtr++;
  uint8_t respCount   = 0U;
  uint8_t respVal     = 0U;

  uint8_t *respBody   = respPtr + 2U;
  bool     postRead   = false;
  bool     checkWrite = false;
  uint32_t data       = 0U;

  swdSetAbortFlag(false);

  if (s_debugPort != DAP_PORT_SWD)
  {
    respPtr[0] = 0U;
    respPtr[1] = DAP_TRANSFER_ERROR;
    return ((uint32_t)(reqPtr - req) << 16U) | 2U;
  }

  for (; reqCount > 0U; reqCount--)
  {
    uint8_t reqByte = *reqPtr++;
    if ((reqByte & DAP_TRANSFER_RnW) != 0U)
    {
      /* Read register */
      if (postRead)
      {
        if ((reqByte & (DAP_TRANSFER_APnDP | DAP_TRANSFER_MATCH_VALUE)) == DAP_TRANSFER_APnDP)
        {
          /* Next AP read fetches previous posted AP read data */
          respVal = swdDapTransfer(reqByte, &data);
        }
        else
        {
          /* Fetch previous posted AP read from RDBUFF first */
          respVal = swdDapTransfer(DP_RDBUFF | DAP_TRANSFER_RnW, &data);
          postRead = false;
        }
        if (respVal != DAP_TRANSFER_OK)
        {
          break;
        }
        writeLE32(respBody, data);
        respBody += 4U;
      }

      if ((reqByte & DAP_TRANSFER_MATCH_VALUE) != 0U)
      {
        uint32_t matchVal = readLE32(reqPtr);
        reqPtr += 4U;
        uint16_t matchRetry = swdGetMatchRetry();

        if ((reqByte & DAP_TRANSFER_APnDP) != 0U)
        {
          respVal = swdDapTransfer(reqByte, NULL);
          if (respVal != DAP_TRANSFER_OK)
          {
            break;
          }
        }
        do
        {
          respVal = swdDapTransfer(reqByte, &data);
          if (respVal != DAP_TRANSFER_OK)
          {
            break;
          }
        } while (((data & s_matchMask) != matchVal) && (matchRetry-- > 0U) && !swdGetAbortFlag());

        if ((data & s_matchMask) != matchVal)
        {
          respVal |= DAP_TRANSFER_MISMATCH;
        }
        if (respVal != DAP_TRANSFER_OK)
        {
          break;
        }
      }
      else
      {
        if (!postRead)
        {
          if ((reqByte & DAP_TRANSFER_APnDP) != 0U)
          {
            if ((reqByte & DAP_TRANSFER_TIMESTAMP) != 0U)
            {
              writeLE32(respBody, swdGetTimestamp());
              respBody += 4U;
            }
            respVal = swdDapTransfer(reqByte, NULL);
            if (respVal != DAP_TRANSFER_OK)
            {
              break;
            }
            postRead = true;
          }
          else
          {
            respVal = swdDapTransfer(reqByte, &data);
            if (respVal != DAP_TRANSFER_OK)
            {
              break;
            }
            if ((reqByte & DAP_TRANSFER_TIMESTAMP) != 0U)
            {
              writeLE32(respBody, swdGetTimestamp());
              respBody += 4U;
            }
            writeLE32(respBody, data);
            respBody += 4U;
          }
        }
      }
      checkWrite = false;
    }
    else
    {
      /* Write register */
      if (postRead)
      {
        respVal = swdDapTransfer(DP_RDBUFF | DAP_TRANSFER_RnW, &data);
        if (respVal != DAP_TRANSFER_OK)
        {
          break;
        }
        writeLE32(respBody, data);
        respBody += 4U;
        postRead = false;
      }

      data = readLE32(reqPtr);
      reqPtr += 4U;

      if ((reqByte & DAP_TRANSFER_MATCH_MASK) != 0U)
      {
        s_matchMask = data;
        respVal = DAP_TRANSFER_OK;
      }
      else
      {
        respVal = swdDapTransfer(reqByte, &data);
        if (respVal != DAP_TRANSFER_OK)
        {
          break;
        }
        if ((reqByte & DAP_TRANSFER_TIMESTAMP) != 0U)
        {
          writeLE32(respBody, swdGetTimestamp());
          respBody += 4U;
        }
        checkWrite = true;
      }
    }

    respCount++;
    if (swdGetAbortFlag())
    {
      break;
    }
  }

  /* Consume any remaining request bytes if we aborted early */
  for (; reqCount > 0U; reqCount--)
  {
    uint8_t reqByte = *reqPtr++;
    if ((reqByte & DAP_TRANSFER_RnW) != 0U)
    {
      if ((reqByte & DAP_TRANSFER_MATCH_VALUE) != 0U)
      {
        reqPtr += 4U;
      }
    }
    else
    {
      reqPtr += 4U;
    }
  }

  if (respVal == DAP_TRANSFER_OK)
  {
    if (postRead)
    {
      respVal = swdDapTransfer(DP_RDBUFF | DAP_TRANSFER_RnW, &data);
      if (respVal != DAP_TRANSFER_OK)
      {
        respCount--;
      }
      else
      {
        writeLE32(respBody, data);
        respBody += 4U;
      }
    }
    else if (checkWrite)
    {
      respVal = swdDapTransfer(DP_RDBUFF | DAP_TRANSFER_RnW, NULL);
      if (respVal != DAP_TRANSFER_OK)
      {
        respCount--;
      }
    }
  }

  respPtr[0] = respCount;
  respPtr[1] = respVal;

  if (respVal != DAP_TRANSFER_OK)
  {
    ledStatusSetErrorTemporary(1000U);
  }
  else
  {
    ledStatusActivityPulse();
  }

  return ((uint32_t)(reqPtr - req) << 16U) | (uint32_t)(respBody - respPtr);
}

static uint32_t dapTransferBlock(const uint8_t *req, uint8_t *resp)
{
  const uint8_t *reqPtr  = req;
  uint8_t       *respPtr = resp;

  uint8_t  dapIdx    = *reqPtr++;
  (void)dapIdx;
  uint16_t reqCount  = readLE16(reqPtr);
  reqPtr += 2U;
  uint8_t  reqByte   = *reqPtr++;

  uint16_t respCount = 0U;
  uint8_t  respVal   = 0U;
  uint8_t *respBody  = respPtr + 3U;
  uint32_t data      = 0U;

  swdSetAbortFlag(false);

  if (s_debugPort != DAP_PORT_SWD)
  {
    writeLE16(&respPtr[0], 0U);
    respPtr[2] = DAP_TRANSFER_ERROR;
    return ((uint32_t)(reqPtr - req) << 16U) | 3U;
  }

  if (reqCount == 0U)
  {
    writeLE16(&respPtr[0], 0U);
    respPtr[2] = 0U;
    return ((uint32_t)(reqPtr - req) << 16U) | 3U;
  }

  if ((reqByte & DAP_TRANSFER_RnW) != 0U)
  {
    /* Read Block */
    if ((reqByte & DAP_TRANSFER_APnDP) != 0U)
    {
      respVal = swdDapTransfer(reqByte, NULL);
      if (respVal != DAP_TRANSFER_OK)
      {
        goto end_block;
      }
    }

    while (reqCount > 0U)
    {
      reqCount--;
      if ((reqCount == 0U) && ((reqByte & DAP_TRANSFER_APnDP) != 0U))
      {
        reqByte = DP_RDBUFF | DAP_TRANSFER_RnW;
      }
      respVal = swdDapTransfer(reqByte, &data);
      if (respVal != DAP_TRANSFER_OK)
      {
        goto end_block;
      }
      writeLE32(respBody, data);
      respBody += 4U;
      respCount++;
      if (swdGetAbortFlag())
      {
        break;
      }
    }
  }
  else
  {
    /* Write Block */
    while (reqCount > 0U)
    {
      data = readLE32(reqPtr);
      reqPtr += 4U;
      reqCount--;
      respVal = swdDapTransfer(reqByte, &data);
      if (respVal != DAP_TRANSFER_OK)
      {
        reqPtr += (uint32_t)reqCount * 4U;
        goto end_block;
      }
      respCount++;
      if (swdGetAbortFlag())
      {
        reqPtr += (uint32_t)reqCount * 4U;
        break;
      }
    }

    if (respVal == DAP_TRANSFER_OK)
    {
      respVal = swdDapTransfer(DP_RDBUFF | DAP_TRANSFER_RnW, NULL);
      if (respVal != DAP_TRANSFER_OK)
      {
        respCount--;
      }
    }
  }

end_block:
  writeLE16(&respPtr[0], respCount);
  respPtr[2] = respVal;

  if (respVal != DAP_TRANSFER_OK)
  {
    ledStatusSetErrorTemporary(1000U);
  }
  else
  {
    ledStatusActivityPulse();
  }

  return ((uint32_t)(reqPtr - req) << 16U) | (uint32_t)(respBody - respPtr);
}

static uint32_t dapWriteAbort(const uint8_t *req, uint8_t *resp)
{
  uint32_t abortVal = readLE32(&req[1]);
  if (s_debugPort == DAP_PORT_SWD)
  {
    uint8_t ack = swdDapTransfer(DP_ABORT, &abortVal);
    resp[0] = (ack == DAP_TRANSFER_OK) ? DAP_OK : DAP_ERROR;
  }
  else
  {
    resp[0] = DAP_ERROR;
  }
  return (5U << 16U) | 1U;
}

static uint32_t dapDelay(const uint8_t *req, uint8_t *resp)
{
  uint16_t delayUs = readLE16(&req[0]);
  swdDelayUs(delayUs);
  resp[0] = DAP_OK;
  return (2U << 16U) | 1U;
}

static uint32_t dapResetTarget(uint8_t *resp)
{
  swdHardResetPulse(20U, 10U);
  resp[0] = DAP_OK;
  resp[1] = 0x01U; /* Target specific reset executed */
  return (0U << 16U) | 2U;
}

static uint32_t dapSwjPins(const uint8_t *req, uint8_t *resp)
{
  uint8_t  pinOut    = req[0];
  uint8_t  pinSelect = req[1];
  uint32_t waitUs    = readLE32(&req[2]);
  resp[0] = swdPinsControl(pinOut, pinSelect, waitUs);
  return (6U << 16U) | 1U;
}

static uint32_t dapSwjClock(const uint8_t *req, uint8_t *resp)
{
  uint32_t clockHz = readLE32(&req[0]);
  if (clockHz == 0U)
  {
    resp[0] = DAP_ERROR;
  }
  else
  {
    swdSetClockHz(clockHz);
    resp[0] = DAP_OK;
  }
  return (4U << 16U) | 1U;
}

static uint32_t dapSwjSequence(const uint8_t *req, uint8_t *resp)
{
  uint32_t bitCount = req[0];
  if (bitCount == 0U)
  {
    bitCount = 256U;
  }
  swdSwjSequence(bitCount, &req[1]);
  resp[0] = DAP_OK;
  uint32_t reqBytes = 1U + ((bitCount + 7U) / 8U);
  return (reqBytes << 16U) | 1U;
}

static uint32_t dapSwdConfigure(const uint8_t *req, uint8_t *resp)
{
  uint8_t cfg        = req[0];
  uint8_t turnaround = (uint8_t)((cfg & 0x03U) + 1U);
  bool    dataPhase  = (cfg & 0x04U) != 0U;
  swdConfigure(turnaround, dataPhase);
  resp[0] = DAP_OK;
  return (1U << 16U) | 1U;
}

static uint32_t dapSwdSequence(const uint8_t *req, uint8_t *resp)
{
  const uint8_t *reqPtr  = req;
  uint8_t       *respPtr = resp;

  *respPtr++ = DAP_OK;

  uint8_t seqCount = *reqPtr++;
  while (seqCount-- > 0U)
  {
    uint8_t  info     = *reqPtr++;
    uint32_t bitCount = info & 0x3FU;
    if (bitCount == 0U)
    {
      bitCount = 64U;
    }
    uint32_t byteCount = (bitCount + 7U) / 8U;

    if ((info & 0x80U) != 0U)
    {
      /* Input sequence */
      swdSequenceIn(bitCount, respPtr);
      respPtr += byteCount;
    }
    else
    {
      /* Output sequence */
      swdSequenceOut(bitCount, reqPtr);
      reqPtr += byteCount;
    }
  }

  return ((uint32_t)(reqPtr - req) << 16U) | (uint32_t)(respPtr - resp);
}

/* -------------------------------------------------------------------------- */
/* Command Dispatcher                                                         */
/* -------------------------------------------------------------------------- */

static uint32_t executeSingleCommand(const uint8_t *request, uint8_t *response)
{
  uint32_t num;

  response[0] = request[0];
  switch (request[0])
  {
    case ID_DAP_Info:
      num = dapInfo(&request[1], &response[1]);
      break;

    case ID_DAP_HostStatus:
      num = dapHostStatus(&request[1], &response[1]);
      break;

    case ID_DAP_Connect:
      num = dapConnect(&request[1], &response[1]);
      break;

    case ID_DAP_Disconnect:
      num = dapDisconnect(&response[1]);
      break;

    case ID_DAP_TransferConfigure:
      num = dapTransferConfigure(&request[1], &response[1]);
      break;

    case ID_DAP_Transfer:
      num = dapTransfer(&request[1], &response[1]);
      break;

    case ID_DAP_TransferBlock:
      num = dapTransferBlock(&request[1], &response[1]);
      break;

    case ID_DAP_TransferAbort:
      swdSetAbortFlag(true);
      response[1] = DAP_OK;
      num = (0U << 16U) | 1U;
      break;

    case ID_DAP_WriteABORT:
      num = dapWriteAbort(&request[1], &response[1]);
      break;

    case ID_DAP_Delay:
      num = dapDelay(&request[1], &response[1]);
      break;

    case ID_DAP_ResetTarget:
      num = dapResetTarget(&response[1]);
      break;

    case ID_DAP_SWJ_Pins:
      num = dapSwjPins(&request[1], &response[1]);
      break;

    case ID_DAP_SWJ_Clock:
      num = dapSwjClock(&request[1], &response[1]);
      break;

    case ID_DAP_SWJ_Sequence:
      num = dapSwjSequence(&request[1], &response[1]);
      break;

    case ID_DAP_SWD_Configure:
      num = dapSwdConfigure(&request[1], &response[1]);
      break;

    case ID_DAP_SWD_Sequence:
      num = dapSwdSequence(&request[1], &response[1]);
      break;

    default:
      response[0] = ID_DAP_Invalid;
      return (1U << 16U) | 1U;
  }

  return ((1U << 16U) | 1U) + num;
}

uint32_t DAP_ProcessCommand(const uint8_t *request, uint8_t *response)
{
  memset(response, 0, DAP_PACKET_SIZE);

  if (request[0] == ID_DAP_ExecuteCommands || request[0] == ID_DAP_QueueCommands)
  {
    response[0] = (request[0] == ID_DAP_QueueCommands) ? ID_DAP_QueueCommands : ID_DAP_ExecuteCommands;
    uint8_t cmdCount = request[1];
    response[1] = cmdCount;

    const uint8_t *reqPtr  = &request[2];
    uint8_t       *respPtr = &response[2];

    while (cmdCount-- > 0U)
    {
      uint32_t n = executeSingleCommand(reqPtr, respPtr);
      reqPtr  += (n >> 16U);
      respPtr += (n & 0xFFFFU);
      if ((uint32_t)(reqPtr - request) >= DAP_PACKET_SIZE ||
          (uint32_t)(respPtr - response) >= DAP_PACKET_SIZE)
      {
        break;
      }
    }
    return ((uint32_t)(reqPtr - request) << 16U) | (uint32_t)(respPtr - response);
  }

  return executeSingleCommand(request, response);
}

/* -------------------------------------------------------------------------- */
/* USB Custom HID Integration                                                 */
/* -------------------------------------------------------------------------- */

void DAP_SetUsbClassId(uint8_t classId)
{
  s_hidClassId = classId;
}

void DAP_Init(void)
{
  swdTimeInit();
  swdRelease();
  s_debugPort  = DAP_PORT_DISABLED;
  s_rxWriteIdx = 0U;
  s_rxReadIdx  = 0U;
  s_rxCount    = 0U;
  s_rxPaused   = false;
  ledStatusInit();
}

bool DAP_CanAcceptPacket(void)
{
  return (s_rxCount < DAP_PACKET_COUNT);
}

bool DAP_IsConnected(void)
{
  return (s_debugPort != DAP_PORT_DISABLED);
}

void DAP_USB_OutCallback(const uint8_t *reportBuf)
{
  if (reportBuf[0] == ID_DAP_TransferAbort)
  {
    swdSetAbortFlag(true);
    return;
  }

  if (s_rxCount < DAP_PACKET_COUNT)
  {
    memcpy(s_rxQueue[s_rxWriteIdx], reportBuf, DAP_PACKET_SIZE);
    s_rxWriteIdx = (uint8_t)((s_rxWriteIdx + 1U) % DAP_PACKET_COUNT);
    s_rxCount++;
  }
  if (s_rxCount >= DAP_PACKET_COUNT)
  {
    s_rxPaused = true;
  }
}

void DAP_Task(void)
{
  if (s_rxCount == 0U)
  {
    return;
  }

  USBD_CUSTOM_HID_HandleTypeDef *hhid =
      (USBD_CUSTOM_HID_HandleTypeDef *)hUsbDeviceFS.pClassDataCmsit[s_hidClassId];
  if (hhid != NULL && hhid->state != CUSTOM_HID_IDLE)
  {
    /* Wait until previous USB IN report finishes transmitting */
    return;
  }

  uint8_t reqBuf[DAP_PACKET_SIZE];
  bool resumeOut = false;
  __disable_irq();
  memcpy(reqBuf, s_rxQueue[s_rxReadIdx], DAP_PACKET_SIZE);
  s_rxReadIdx = (uint8_t)((s_rxReadIdx + 1U) % DAP_PACKET_COUNT);
  s_rxCount--;
  if (s_rxPaused && s_rxCount < DAP_PACKET_COUNT)
  {
    s_rxPaused = false;
    resumeOut = true;
  }
  __enable_irq();

  if (resumeOut)
  {
    hUsbDeviceFS.classId = s_hidClassId;
    USBD_CUSTOM_HID_ReceivePacket(&hUsbDeviceFS);
  }

  DAP_ProcessCommand(reqBuf, s_txBuf);
  USBD_CUSTOM_HID_SendReport(&hUsbDeviceFS, s_txBuf, DAP_PACKET_SIZE, s_hidClassId);
}
