#include "led_status.h"
#include "main.h"

static LedState_t s_baseState          = LED_STATE_IDLE;
static uint32_t   s_errorUntilTick     = 0U;
static uint32_t   s_activityUntilTick  = 0U;
static uint32_t   s_debugCmdUntilTick  = 0U;

static inline void ledSetRaw(bool on)
{
  /* Active LOW: RESET = ON, SET = OFF */
  HAL_GPIO_WritePin(MCU_LED_GPIO_Port, MCU_LED_Pin, on ? GPIO_PIN_RESET : GPIO_PIN_SET);
}

void ledStatusInit(void)
{
  s_baseState = LED_STATE_IDLE;
  s_errorUntilTick = 0U;
  s_activityUntilTick = 0U;
  s_debugCmdUntilTick = 0U;
  ledSetRaw(false);
}

void ledStatusSetState(LedState_t state)
{
  s_baseState = state;
}

LedState_t ledStatusGetState(void)
{
  return s_baseState;
}

void ledStatusDebugCommandPulse(void)
{
  /* Nháy 3 lần liên tục (3 chu kỳ 120ms = 360ms: 60ms Tắt, 60ms Sáng) rồi lại Solid ON */
  s_debugCmdUntilTick = HAL_GetTick() + 360U;
}

void ledStatusActivityPulse(void)
{
  /* Kích hoạt nháy liên tục trong ít nhất 100ms khi có transfer dữ liệu */
  s_activityUntilTick = HAL_GetTick() + 100U;
}

void ledStatusSetErrorTemporary(uint32_t durationMs)
{
  s_errorUntilTick = HAL_GetTick() + durationMs;
}

void ledStatusUpdate(void)
{
  uint32_t now = HAL_GetTick();

  /* 1. Ưu tiên cao nhất: Báo lỗi tạm thời */
  if (now < s_errorUntilTick || s_baseState == LED_STATE_ERROR)
  {
    uint32_t t = now % 800U;
    bool on = (t < 60U) ||
              (t >= 120U && t < 180U) ||
              (t >= 240U && t < 300U);
    ledSetRaw(on);
    return;
  }

  /* 2. Đang có tác vụ Flash / Erase / Verify tác động tới chip (nháy liên tục cho đến khi hoàn thành) */
  if (s_baseState == LED_STATE_BUSY_FLASHING || now < s_activityUntilTick)
  {
    /* Flicker nhanh 16.7Hz (chu kỳ 60ms: 30ms Sáng, 30ms Tắt) */
    uint32_t t = now % 60U;
    ledSetRaw(t < 30U);
    return;
  }

  /* 3. Các trạng thái nền */
  switch (s_baseState)
  {
    case LED_STATE_DEBUG:
    {
      /* Debug mode: LED sáng liên tục.
       * Khi ra lệnh lúc đang debug: nháy 3 lần liên tục rồi lại sáng liên tục */
      if (now < s_debugCmdUntilTick)
      {
        uint32_t t = (s_debugCmdUntilTick - now) % 120U;
        /* Chớp 60ms TẮT / 60ms SÁNG */
        ledSetRaw(t < 60U);
      }
      else
      {
        ledSetRaw(true); /* Sáng liên tục */
      }
      break;
    }

    case LED_STATE_CONNECTED:
    {
      /* Khi kết nối được tới 1 thiết bị: Blink LED double (nháy kép 2 lần) */
      uint32_t t = now % 1200U;
      bool on = (t < 70U) || (t >= 200U && t < 270U);
      ledSetRaw(on);
      break;
    }

    case LED_STATE_IDLE:
    default:
    {
      /* Khi không kết nối: Blink LED bình thường (blink 1 lần / chu kỳ 1s) */
      uint32_t t = now % 1000U;
      bool on = (t < 80U);
      ledSetRaw(on);
      break;
    }
  }
}
