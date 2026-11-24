#ifndef __LED_STATUS_H__
#define __LED_STATUS_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

/**
 * @brief Các trạng thái LED hiển thị chuẩn công nghiệp cho mạch nạp
 */
typedef enum {
  LED_STATE_IDLE = 0,       /* Chờ kết nối (Standby): Heartbeat nhịp tim kép thanh lịch */
  LED_STATE_CONNECTED,      /* Đã kết nối Host / Target SWD link active: Sáng liên tục (Solid ON) */
  LED_STATE_BUSY_FLASHING,  /* Đang nạp code / Erase / Flash programming: Flicker cực nhanh */
  LED_STATE_DEBUG_HALTED,   /* CPU Target đang bị Halted / Breakpoint: Nháy kép (Double blink) */
  LED_STATE_ERROR,          /* Báo lỗi kết nối / SWD fault / Verify mismatch: 3 chớp dồn dập */
} LedState_t;

/**
 * @brief Khởi tạo module LED Status
 */
void ledStatusInit(void);

/**
 * @brief Đặt trạng thái LED hiển thị nền
 * @param state Trạng thái mong muốn (LED_STATE_IDLE, LED_STATE_CONNECTED, ...)
 */
void ledStatusSetState(LedState_t state);

/**
 * @brief Lấy trạng thái LED hiện tại
 */
LedState_t ledStatusGetState(void);

/**
 * @brief Kích hoạt xung nhấp nháy Activity (chớp cực nhanh) khi có packet SWD / Flash transfer
 *        Hàm này không block, tự phục hồi về trạng thái nền sau khi hết transfer.
 */
void ledStatusActivityPulse(void);

/**
 * @brief Báo lỗi tạm thời trong một khoảng thời gian (ms), sau đó tự trở về trạng thái trước
 * @param durationMs Thời gian báo lỗi (vd: 2000ms)
 */
void ledStatusSetErrorTemporary(uint32_t durationMs);

/**
 * @brief Hàm cập nhật LED State Machine - non-blocking, gọi liên tục trong main while(1) loop
 */
void ledStatusUpdate(void);

#ifdef __cplusplus
}
#endif

#endif /* __LED_STATUS_H__ */
