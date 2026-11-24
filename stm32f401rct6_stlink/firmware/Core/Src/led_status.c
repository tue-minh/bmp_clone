#include "led_status.h"
#include "main.h"

static LedState_t s_baseState        = LED_STATE_IDLE;
static uint32_t   s_errorUntilTick   = 0U;
static uint32_t   s_activityUntilTick= 0U;

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

void ledStatusActivityPulse(void)
{
  /* Kích hoạt xung nhấp nháy bận rộn trong ít nhất 80ms */
  s_activityUntilTick = HAL_GetTick() + 80U;
}

void ledStatusSetErrorTemporary(uint32_t durationMs)
{
  s_errorUntilTick = HAL_GetTick() + durationMs;
}

void ledStatusUpdate(void)
{
  uint32_t now = HAL_GetTick();

  /* 1. Ưu tiên cao nhất: Trạng thái Lỗi tạm thời */
  if (now < s_errorUntilTick)
  {
    uint32_t t = now % 800U;
    bool on = (t < 60U) ||
              (t >= 120U && t < 180U) ||
              (t >= 240U && t < 300U);
    ledSetRaw(on);
    return;
  }

  /* 2. Ưu tiên kế tiếp: Đang có xung truyền dữ liệu / Flash activity */
  if (now < s_activityUntilTick || s_baseState == LED_STATE_BUSY_FLASHING)
  {
    /* Flicker nhanh 16.7Hz (chu kỳ 60ms: 30ms ON, 30ms OFF) */
    uint32_t t = now % 60U;
    ledSetRaw(t < 30U);
    return;
  }

  /* 3. Các trạng thái nền */
  switch (s_baseState)
  {
    case LED_STATE_CONNECTED:
      /* Khi kết nối session SWD / Target Link: Sáng liên tục */
      ledSetRaw(true);
      break;

    case LED_STATE_DEBUG_HALTED:
    {
      /* Double blink (chu kỳ 1000ms: chớp 2 lần ngắn báo Halt) */
      uint32_t t = now % 1000U;
      bool on = (t < 90U) || (t >= 220U && t < 310U);
      ledSetRaw(on);
      break;
    }

    case LED_STATE_ERROR:
    {
      /* 3 chớp dồn dập (chu kỳ 800ms) */
      uint32_t t = now % 800U;
      bool on = (t < 60U) ||
                (t >= 120U && t < 180U) ||
                (t >= 240U && t < 300U);
      ledSetRaw(on);
      break;
    }

    case LED_STATE_IDLE:
    default:
    {
      /* Heartbeat nhịp tim kép (chu kỳ 1200ms: thump-thump ... thump-thump) */
      uint32_t t = now % 1200U;
      bool on = (t < 40U) || (t >= 180U && t < 220U);
      ledSetRaw(on);
      break;
    }
  }
}
