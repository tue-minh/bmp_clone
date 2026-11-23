/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : usb_device.c
  * @version        : v1.0_Cube
  * @brief          : This file implements the USB Device
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/

#include "usb_device.h"
#include "usbd_core.h"
#include "usbd_desc.h"
#include "usbd_customhid.h"
#include "usbd_custom_hid_if.h"
#include "usbd_cdc.h"
#include "usbd_cdc_if.h"
#include "usbd_composite_builder.h"
#include "dap.h"

/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* USER CODE BEGIN PV */
/* Private variables ---------------------------------------------------------*/

/* USER CODE END PV */

/* USER CODE BEGIN PFP */
/* Private function prototypes -----------------------------------------------*/

/* USER CODE END PFP */

/* USB Device Core handle declaration. */
USBD_HandleTypeDef hUsbDeviceFS;

/*
 * -- Insert your variables declaration here --
 */
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/*
 * -- Insert your external function declaration here --
 */
/* USER CODE BEGIN 1 */

/* USER CODE END 1 */

/**
  * Init USB device Library, add supported class and start the library
  * @retval None
  */
void MX_USB_DEVICE_Init(void)
{
  /* USER CODE BEGIN USB_DEVICE_Init_PreTreatment */

  /* USER CODE END USB_DEVICE_Init_PreTreatment */

  static uint8_t s_hidEpAdd[2] = { CUSTOM_HID_EPIN_ADDR, CUSTOM_HID_EPOUT_ADDR };
  static uint8_t s_cdcEpAdd[3] = { CDC_IN_EP, CDC_OUT_EP, CDC_CMD_EP };

  /* Init Device Library, add composite classes (CMSIS-DAP HID + CDC VCP) and start. */
  if (USBD_Init(&hUsbDeviceFS, &FS_Desc, DEVICE_FS) != USBD_OK)
  {
    Error_Handler();
  }
  if (USBD_RegisterClassComposite(&hUsbDeviceFS, &USBD_CUSTOM_HID, CLASS_TYPE_CHID, s_hidEpAdd) != USBD_OK)
  {
    Error_Handler();
  }
  if (USBD_RegisterClassComposite(&hUsbDeviceFS, &USBD_CDC, CLASS_TYPE_CDC, s_cdcEpAdd) != USBD_OK)
  {
    Error_Handler();
  }

  uint8_t hidClassId = (uint8_t)USBD_CMPSIT_GetClassID(&hUsbDeviceFS, CLASS_TYPE_CHID, 0U);
  uint8_t cdcClassId = (uint8_t)USBD_CMPSIT_GetClassID(&hUsbDeviceFS, CLASS_TYPE_CDC, 0U);

  DAP_SetUsbClassId(hidClassId);
  hUsbDeviceFS.classId = hidClassId;
  if (USBD_CUSTOM_HID_RegisterInterface(&hUsbDeviceFS, &USBD_CustomHID_fops_FS) != USBD_OK)
  {
    Error_Handler();
  }

  CDC_SetUsbClassId(cdcClassId);
  hUsbDeviceFS.classId = cdcClassId;
  if (USBD_CDC_RegisterInterface(&hUsbDeviceFS, &USBD_Interface_fops_FS) != USBD_OK)
  {
    Error_Handler();
  }

  if (USBD_Start(&hUsbDeviceFS) != USBD_OK)
  {
    Error_Handler();
  }

  /* USER CODE BEGIN USB_DEVICE_Init_PostTreatment */

  /* USER CODE END USB_DEVICE_Init_PostTreatment */
}

/**
  * @}
  */

/**
  * @}
  */

