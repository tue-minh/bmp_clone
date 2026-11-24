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
  LED_STATE_IDLE = 0,       /* Chưa kết nối (Standby): Chớp đơn bình thường (1 lần / chu kỳ) */
  LED_STATE_CONNECTED,      /* Đã kết nối tới chip: Chớp kép (Double blink 2 lần / chu kỳ) */
  LED_STATE_DEBUG,          /* Debug mode: Sáng liên tục (Solid ON) */
  LED_STATE_BUSY_FLASHING,  /* Đang nạp / Erase / Verify: Nháy liên tục đến khi hoàn thành */
  LED_STATE_ERROR,          /* Báo lỗi kết nối / SWD fault / Verify mismatch: 3 chớp dồn dập */
} LedState_t;

/**
 * @brief Khởi tạo module LED Status
 */
void ledStatusInit(void);

/**
 * @brief Đặt trạng thái LED hiển thị nền
 */
void ledStatusSetState(LedState_t state);

/**
 * @brief Lấy trạng thái LED hiện tại
 */
LedState_t ledStatusGetState(void);

/**
 * @brief Khi đang debug mà có lệnh tác động (Step into/out, Halt, Run, Read Regs):
 *        Nháy 3 lần liên tục rồi tự động quay lại sáng liên tục
 */
void ledStatusDebugCommandPulse(void);

/**
 * @brief Kích hoạt xung nhấp nháy liên tục khi có nạp code / verify / flash transfer
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
