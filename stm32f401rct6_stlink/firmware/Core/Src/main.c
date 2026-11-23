/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
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
#include "main.h"
#include "usart.h"
#include "usb_device.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "swd.h"
#include "dap.h"
#include "stm32_target.h"
#include "usbd_cdc_if.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
static char     s_cliLine[96];
static uint32_t s_cliPos = 0U;

int _write(int file, char *ptr, int len)
{
  (void)file;
  HAL_UART_Transmit(&huart1, (uint8_t *)ptr, (uint16_t)len, 100U);
  return len;
}

static void printTargetProbe(void)
{
  if (DAP_IsConnected())
  {
    printf("[WARN] USB CMSIS-DAP session active; disconnect IDE before standalone probe\r\n");
    return;
  }

  Stm32TargetInfo_t info;
  char err[128] = {0};
  if (stm32TargetProbe(&info, err, sizeof(err)))
  {
    printf("[SWD] CONNECTED: %s\r\n", info.name);
    printf("      DP_IDCODE=0x%08lX DBGMCU_IDCODE=0x%08lX (DEV_ID=0x%03X REV=0x%04X)\r\n",
           (unsigned long)info.dpIdcode, (unsigned long)info.dbgIdcode,
           info.devId, info.revId);
    printf("      Flash=%lu KB | Page/Sector=%lu B | RDP=0x%02X (%s)\r\n",
           (unsigned long)info.flashSizeKB, (unsigned long)info.pageOrSectorSize,
           info.rdpLevel, (info.rdpLevel == 0xAAU) ? "Level 0" : "Protected");
  }
  else
  {
    printf("[SWD] DISCONNECTED: %s\r\n", err);
  }
}

static void processCliCommand(char *cmd)
{
  char err[128] = {0};

  if (strcmp(cmd, "check") == 0 || strcmp(cmd, "c") == 0 || strcmp(cmd, "probe") == 0)
  {
    printTargetProbe();
  }
  else if (strcmp(cmd, "link") == 0)
  {
    uint32_t dpId = 0U;
    if (stm32TargetCheckLink(&dpId))
      printf("[SWD] Link OK: DP_IDCODE=0x%08lX\r\n", (unsigned long)dpId);
    else
      printf("[SWD] No target ACK\r\n");
  }
  else if (strcmp(cmd, "reset") == 0 || strcmp(cmd, "r") == 0)
  {
    if (stm32TargetReset(err, sizeof(err)))
      printf("[SWD] Target reset OK\r\n");
    else
      printf("[SWD] Target reset FAILED: %s\r\n", err);
  }
  else if (strcmp(cmd, "halt") == 0)
  {
    if (stm32TargetHalt(err, sizeof(err)))
      printf("[SWD] Target CPU halted\r\n");
    else
      printf("[SWD] Halt FAILED: %s\r\n", err);
  }
  else if (strcmp(cmd, "run") == 0)
  {
    if (stm32TargetResume(err, sizeof(err)))
      printf("[SWD] Target CPU resumed\r\n");
    else
      printf("[SWD] Resume FAILED: %s\r\n", err);
  }
  else if (strncmp(cmd, "read ", 5) == 0)
  {
    uint32_t addr = (uint32_t)strtoul(&cmd[5], NULL, 0);
    uint32_t val = 0U;
    swdInit();
    if (swdConnect() != 0U && swdPowerUp() && memRead32(addr, &val))
      printf("[SWD] [0x%08lX] = 0x%08lX\r\n", (unsigned long)addr, (unsigned long)val);
    else
      printf("[SWD] Read 0x%08lX failed (%s)\r\n", (unsigned long)addr, swdErrorStr(swdLastError()));
    swdRelease();
  }
  else if (strcmp(cmd, "status") == 0 || strcmp(cmd, "s") == 0)
  {
    printf("[INFO] USB_DAP=%s | HCLK=%lu MHz | NRST_PIN=%s\r\n",
           DAP_IsConnected() ? "CONNECTED" : "IDLE",
           (unsigned long)(SystemCoreClock / 1000000UL),
           swdGetResetState() ? "HIGH (Released)" : "LOW (Asserted)");
  }
  else
  {
    printf("[HELP] Commands: probe (c), link, reset (r), halt, run, read <0xADDR>, status (s)\r\n");
  }
}

static void serviceButtonAndCli(void)
{
  /* PA0 (MCU_BUTTON, active LOW with internal pull-up): short press -> toggle LED */
  static uint8_t  lastBtn = GPIO_PIN_SET;
  static uint32_t lastDebounceMs = 0U;
  uint8_t rawBtn = (uint8_t)HAL_GPIO_ReadPin(MCU_BUTTON_GPIO_Port, MCU_BUTTON_Pin);
  uint32_t now = HAL_GetTick();

  if (rawBtn != lastBtn && (now - lastDebounceMs) > 50U)
  {
    lastDebounceMs = now;
    lastBtn = rawBtn;
    if (rawBtn == GPIO_PIN_RESET)
    {
      HAL_GPIO_TogglePin(MCU_LED_GPIO_Port, MCU_LED_Pin);
    }
  }
  (void)s_cliLine;
  (void)s_cliPos;
  (void)processCliCommand;
}
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_USART1_UART_Init();
  MX_USB_DEVICE_Init();
  /* USER CODE BEGIN 2 */
  DAP_Init();
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    DAP_Task();
    CDC_UART_Task();
    serviceButtonAndCli();
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE2);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 15;
  RCC_OscInitStruct.PLL.PLLN = 144;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV4;
  RCC_OscInitStruct.PLL.PLLQ = 5;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_1) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
