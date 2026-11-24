#include "led_status.h"
#include "main.h"

static LedState_t s_baseState            = LED_STATE_IDLE;
static uint32_t   s_errorUntilTick       = 0U;
static uint32_t   s_activityUntilTick    = 0U;
static uint32_t   s_debugCmdStartTick    = 0U;
static bool       s_debugCmdPulseActive  = false;

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
  s_debugCmdStartTick = 0U;
  s_debugCmdPulseActive = false;
  ledSetRaw(false);
}

void ledStatusSetState(LedState_t state)
{
  if (s_baseState != state)
  {
    s_baseState = state;
    s_debugCmdPulseActive = false;
    s_activityUntilTick = 0U;
  }
}

LedState_t ledStatusGetState(void)
{
  return s_baseState;
}

void ledStatusDebugCommandPulse(void)
{
  /* Nháy 3 lần liên tục (3 chu kỳ 180ms = 540ms: 90ms Tắt, 90ms Sáng) rồi lại Solid ON.
   * Không reset bộ đếm nếu chuỗi 3 nháy đang chạy để tránh méo nhịp khi 1 lệnh gửi nhiều gói USB. */
  if (!s_debugCmdPulseActive)
  {
    s_debugCmdPulseActive = true;
    s_debugCmdStartTick = HAL_GetTick();
  }
}

void ledStatusActivityPulse(void)
{
  /* Kích hoạt nháy liên tục trong ít nhất 120ms khi có transfer dữ liệu */
  s_activityUntilTick = HAL_GetTick() + 120U;
}

void ledStatusSetErrorTemporary(uint32_t durationMs)
{
  s_errorUntilTick = HAL_GetTick() + durationMs;
}

void ledStatusClearError(void)
{
  s_errorUntilTick = 0U;
}

void ledStatusUpdate(void)
{
  uint32_t now = HAL_GetTick();

  /* 1. Ưu tiên cao nhất: Báo lỗi tạm thời */
  if ((s_errorUntilTick != 0U && (int32_t)(s_errorUntilTick - now) > 0) || s_baseState == LED_STATE_ERROR)
  {
    uint32_t t = now % 800U;
    bool on = (t < 60U) ||
              (t >= 120U && t < 180U) ||
              (t >= 240U && t < 300U);
    ledSetRaw(on);
    return;
  }
  s_errorUntilTick = 0U;

  /* 2. Đang có tác vụ Flash / Erase / Verify / thao tác chip không phải debug: nháy liên tục đến khi xong */
  if (s_baseState == LED_STATE_BUSY_FLASHING ||
      (s_baseState != LED_STATE_DEBUG && s_activityUntilTick != 0U && (int32_t)(s_activityUntilTick - now) > 0))
  {
    /* Flicker nhanh 12.5Hz (chu kỳ 80ms: 40ms Sáng, 40ms Tắt) */
    uint32_t t = now % 80U;
    ledSetRaw(t < 40U);
    return;
  }

  /* 3. Các trạng thái nền */
  switch (s_baseState)
  {
    case LED_STATE_DEBUG:
    {
      /* Debug mode: LED sáng liên tục.
       * Khi ra lệnh lúc đang debug: nháy 3 lần liên tục rồi lại sáng liên tục */
      if (s_debugCmdPulseActive)
      {
        uint32_t elapsed = now - s_debugCmdStartTick;
        if (elapsed < 540U)
        {
          uint32_t t = elapsed % 180U;
          /* Chớp 90ms TẮT / 90ms SÁNG x 3 nhịp */
          ledSetRaw(t >= 90U);
        }
        else
        {
          s_debugCmdPulseActive = false;
          ledSetRaw(true); /* Quay lại sáng liên tục */
        }
      }
      else
      {
        ledSetRaw(true); /* Sáng liên tục */
      }
      break;
    }

    case LED_STATE_CONNECTED:
    {
      /* Khi kết nối được tới 1 thiết bị: Blink LED double (nháy kép 2 lần)
       * Chu kỳ 1200ms: Sáng 80ms, Tắt 120ms, Sáng 80ms, Tắt 920ms */
      uint32_t t = now % 1200U;
      bool on = (t < 80U) || (t >= 200U && t < 280U);
      ledSetRaw(on);
      break;
    }

    case LED_STATE_IDLE:
    default:
    {
      /* Khi không kết nối: Blink LED bình thường (blink 1 lần / chu kỳ 1s)
       * Chu kỳ 1000ms: Sáng 100ms, Tắt 900ms */
      uint32_t t = now % 1000U;
      bool on = (t < 100U);
      ledSetRaw(on);
      break;
    }
  }
}
