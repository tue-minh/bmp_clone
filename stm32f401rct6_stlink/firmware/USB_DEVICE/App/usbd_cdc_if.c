/**
  ******************************************************************************
  * @file    usbd_cdc_if.c
  * @brief   USB CDC (Virtual COM Port) <-> USART1 (PA9 TX / PB7 RX) bridge
  *          for TUE CMSIS-DAP + VCP Composite Probe.
  ******************************************************************************
  */

#include "usbd_cdc_if.h"
#include "usart.h"
#include <stdbool.h>
#include <string.h>

extern USBD_HandleTypeDef hUsbDeviceFS;

#define CDC_RX_BUF_SIZE   256U
#define UART_BUF_SIZE     1024U

static uint8_t s_cdcClassId = 1U;
static uint8_t s_usbRxBuf[CDC_RX_BUF_SIZE];
static uint8_t s_usbTxChunk[64];

/* Ring buffer: USB OUT -> USART1 TX (PA9) */
static uint8_t           s_uTxRing[UART_BUF_SIZE];
static volatile uint16_t s_uTxHead = 0U;
static volatile uint16_t s_uTxTail = 0U;
static volatile bool     s_usbRxPaused = false;

/* Ring buffer: USART1 RX (PB7) -> USB IN */
static uint8_t           s_uRxRing[UART_BUF_SIZE];
static volatile uint16_t s_uRxHead = 0U;
static volatile uint16_t s_uRxTail = 0U;

static USBD_CDC_LineCodingTypeDef s_lineCoding =
{
  115200U, /* baud rate */
  0x00U,   /* 1 stop bit */
  0x00U,   /* no parity */
  0x08U    /* 8 data bits */
};

static int8_t CDC_Init_FS(void);
static int8_t CDC_DeInit_FS(void);
static int8_t CDC_Control_FS(uint8_t cmd, uint8_t *pbuf, uint16_t length);
static int8_t CDC_Receive_FS(uint8_t *Buf, uint32_t *Len);
static int8_t CDC_TransmitCplt_FS(uint8_t *Buf, uint32_t *Len, uint8_t epnum);

USBD_CDC_ItfTypeDef USBD_Interface_fops_FS =
{
  CDC_Init_FS,
  CDC_DeInit_FS,
  CDC_Control_FS,
  CDC_Receive_FS,
  CDC_TransmitCplt_FS
};

void CDC_SetUsbClassId(uint8_t classId)
{
  s_cdcClassId = classId;
}

static uint16_t uTxFreeSpace(void)
{
  uint16_t head = s_uTxHead;
  uint16_t tail = s_uTxTail;
  if (head >= tail)
  {
    return (uint16_t)((UART_BUF_SIZE - 1U) - (head - tail));
  }
  return (uint16_t)(tail - head - 1U);
}

static void applyUartLineCoding(void)
{
  uint32_t baud = s_lineCoding.bitrate;
  if (baud < 1200U || baud > 4000000U)
  {
    baud = 115200U;
  }

  huart1.Init.BaudRate = baud;
  huart1.Init.StopBits = (s_lineCoding.format == 2U) ? UART_STOPBITS_2 : UART_STOPBITS_1;

  if (s_lineCoding.paritytype == 1U)
  {
    huart1.Init.Parity     = UART_PARITY_ODD;
    huart1.Init.WordLength = UART_WORDLENGTH_9B;
  }
  else if (s_lineCoding.paritytype == 2U)
  {
    huart1.Init.Parity     = UART_PARITY_EVEN;
    huart1.Init.WordLength = UART_WORDLENGTH_9B;
  }
  else
  {
    huart1.Init.Parity     = UART_PARITY_NONE;
    huart1.Init.WordLength = UART_WORDLENGTH_8B;
  }

  HAL_UART_Init(&huart1);
}

static int8_t CDC_Init_FS(void)
{
  s_uTxHead = 0U;
  s_uTxTail = 0U;
  s_uRxHead = 0U;
  s_uRxTail = 0U;
  s_usbRxPaused = false;

  hUsbDeviceFS.classId = s_cdcClassId;
  USBD_CDC_SetTxBuffer(&hUsbDeviceFS, s_usbTxChunk, 0U, s_cdcClassId);
  USBD_CDC_SetRxBuffer(&hUsbDeviceFS, s_usbRxBuf);
  USBD_CDC_ReceivePacket(&hUsbDeviceFS);
  return (int8_t)USBD_OK;
}

static int8_t CDC_DeInit_FS(void)
{
  return (int8_t)USBD_OK;
}

static int8_t CDC_Control_FS(uint8_t cmd, uint8_t *pbuf, uint16_t length)
{
  UNUSED(length);

  switch (cmd)
  {
    case CDC_SET_LINE_CODING:
      s_lineCoding.bitrate = (uint32_t)pbuf[0] |
                             ((uint32_t)pbuf[1] << 8U) |
                             ((uint32_t)pbuf[2] << 16U) |
                             ((uint32_t)pbuf[3] << 24U);
      s_lineCoding.format     = pbuf[4];
      s_lineCoding.paritytype = pbuf[5];
      s_lineCoding.datatype   = pbuf[6];
      applyUartLineCoding();
      break;

    case CDC_GET_LINE_CODING:
      pbuf[0] = (uint8_t)(s_lineCoding.bitrate & 0xFFU);
      pbuf[1] = (uint8_t)((s_lineCoding.bitrate >> 8U) & 0xFFU);
      pbuf[2] = (uint8_t)((s_lineCoding.bitrate >> 16U) & 0xFFU);
      pbuf[3] = (uint8_t)((s_lineCoding.bitrate >> 24U) & 0xFFU);
      pbuf[4] = s_lineCoding.format;
      pbuf[5] = s_lineCoding.paritytype;
      pbuf[6] = s_lineCoding.datatype;
      break;

    default:
      break;
  }

  return (int8_t)USBD_OK;
}

static int8_t CDC_Receive_FS(uint8_t *Buf, uint32_t *Len)
{
  uint32_t n = *Len;
  for (uint32_t i = 0U; i < n; i++)
  {
    uint16_t nextHead = (uint16_t)((s_uTxHead + 1U) % UART_BUF_SIZE);
    if (nextHead != s_uTxTail)
    {
      s_uTxRing[s_uTxHead] = Buf[i];
      s_uTxHead = nextHead;
    }
  }

  if (uTxFreeSpace() >= 64U)
  {
    hUsbDeviceFS.classId = s_cdcClassId;
    USBD_CDC_SetRxBuffer(&hUsbDeviceFS, s_usbRxBuf);
    USBD_CDC_ReceivePacket(&hUsbDeviceFS);
  }
  else
  {
    s_usbRxPaused = true;
  }

  return (int8_t)USBD_OK;
}

static int8_t CDC_TransmitCplt_FS(uint8_t *Buf, uint32_t *Len, uint8_t epnum)
{
  UNUSED(Buf);
  UNUSED(Len);
  UNUSED(epnum);
  return (int8_t)USBD_OK;
}

void CDC_UART_Task(void)
{
  /* 1. Clear overrun error if any & drain USART1 RX (PB7) into ring buffer */
  if (__HAL_UART_GET_FLAG(&huart1, UART_FLAG_ORE) != RESET)
  {
    __HAL_UART_CLEAR_OREFLAG(&huart1);
  }
  while (__HAL_UART_GET_FLAG(&huart1, UART_FLAG_RXNE) != RESET)
  {
    uint8_t b = (uint8_t)(huart1.Instance->DR & 0xFFU);
    uint16_t nextHead = (uint16_t)((s_uRxHead + 1U) % UART_BUF_SIZE);
    if (nextHead != s_uRxTail)
    {
      s_uRxRing[s_uRxHead] = b;
      s_uRxHead = nextHead;
    }
  }

  /* 2. Feed USART1 TX (PA9) from USB-to-UART ring buffer */
  while (s_uTxTail != s_uTxHead && __HAL_UART_GET_FLAG(&huart1, UART_FLAG_TXE) != RESET)
  {
    huart1.Instance->DR = s_uTxRing[s_uTxTail];
    s_uTxTail = (uint16_t)((s_uTxTail + 1U) % UART_BUF_SIZE);
  }

  /* Resume USB CDC OUT endpoint once buffer has room */
  if (s_usbRxPaused && uTxFreeSpace() >= 64U)
  {
    s_usbRxPaused = false;
    hUsbDeviceFS.classId = s_cdcClassId;
    USBD_CDC_SetRxBuffer(&hUsbDeviceFS, s_usbRxBuf);
    USBD_CDC_ReceivePacket(&hUsbDeviceFS);
  }

  /* 3. Forward buffered USART1 RX bytes to USB CDC IN endpoint */
  if (hUsbDeviceFS.dev_state == USBD_STATE_CONFIGURED && s_uRxTail != s_uRxHead)
  {
    USBD_CDC_HandleTypeDef *hcdc =
        (USBD_CDC_HandleTypeDef *)hUsbDeviceFS.pClassDataCmsit[s_cdcClassId];
    if (hcdc != NULL && hcdc->TxState == 0U)
    {
      uint16_t cnt = 0U;
      while (s_uRxTail != s_uRxHead && cnt < sizeof(s_usbTxChunk))
      {
        s_usbTxChunk[cnt++] = s_uRxRing[s_uRxTail];
        s_uRxTail = (uint16_t)((s_uRxTail + 1U) % UART_BUF_SIZE);
      }
      if (cnt > 0U)
      {
        USBD_CDC_SetTxBuffer(&hUsbDeviceFS, s_usbTxChunk, cnt, s_cdcClassId);
        USBD_CDC_TransmitPacket(&hUsbDeviceFS, s_cdcClassId);
      }
    }
  }
}
