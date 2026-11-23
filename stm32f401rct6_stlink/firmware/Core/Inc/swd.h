#ifndef __SWD_H__
#define __SWD_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "main.h"

/* ADIv5 DP register addresses (A[3:2] as byte offsets) */
#define DP_IDCODE       0x00U   /* Read */
#define DP_ABORT        0x00U   /* Write */
#define DP_CTRL_STAT    0x04U
#define DP_SELECT       0x08U
#define DP_RDBUFF       0x0CU

/* MEM-AP registers (AP bank 0) */
#define AP_CSW          0x00U
#define AP_TAR          0x04U
#define AP_DRW          0x0CU
#define AP_IDR          0xFCU

#define CSW_8BIT_INC    0x23000010UL   /* 8-bit, single auto-increment */
#define CSW_16BIT_INC   0x23000011UL   /* 16-bit, single auto-increment */
#define CSW_32BIT_INC   0x23000012UL   /* 32-bit, single auto-increment */
#define CSW_32BIT_NOINC 0x23000002UL   /* 32-bit, no auto-increment */
#define ABORT_CLEAR     0x1EUL         /* Clear STKCMP/STKERR/WDERR/ORUNERR */

/* ACK values / error codes (compatible with ESP_OTA_STM32 & CMSIS-DAP) */
#define SWD_ACK_OK      0x01
#define SWD_ACK_WAIT    0x02
#define SWD_ACK_FAULT   0x04
#define SWD_ERR_PARITY  (-1)
#define SWD_ERR_NOACK   (-2)
#define SWD_ERR_TIMEOUT (-3)

/* CMSIS-DAP Transfer request bit masks */
#define DAP_TRANSFER_APnDP        (1U << 0)
#define DAP_TRANSFER_RnW          (1U << 1)
#define DAP_TRANSFER_A2           (1U << 2)
#define DAP_TRANSFER_A3           (1U << 3)
#define DAP_TRANSFER_MATCH_VALUE  (1U << 4)
#define DAP_TRANSFER_MATCH_MASK   (1U << 5)
#define DAP_TRANSFER_TIMESTAMP    (1U << 7)

#define DAP_TRANSFER_OK           0x01U
#define DAP_TRANSFER_WAIT         0x02U
#define DAP_TRANSFER_FAULT        0x04U
#define DAP_TRANSFER_ERROR        0x07U
#define DAP_TRANSFER_MISMATCH     0x10U

/* Initialize DWT cycle counter and SWD GPIO pins */
void swdTimeInit(void);
void swdDelayUs(uint32_t us);
uint32_t swdGetTimestamp(void);

void swdInit(void);
void swdRelease(void);

/* Configure SWD clock, turnaround (1..4 cycles),data_phase, idle_cycles, wait_retry */
void swdSetClockHz(uint32_t hz);
void swdConfigure(uint8_t turnaround, bool dataPhase);
void swdTransferConfigure(uint8_t idleCycles, uint16_t waitRetry, uint16_t matchRetry);
uint16_t swdGetWaitRetry(void);
uint16_t swdGetMatchRetry(void);
void swdSetAbortFlag(bool abort);
bool swdGetAbortFlag(void);

/* Target nRESET (PB8 Open-Drain) control: asserted=true pulls NRST LOW */
void swdSetReset(bool asserted);
bool swdGetResetState(void);
void swdHardResetPulse(uint32_t lowMs, uint32_t settleMs);

/* Direct pin access for CMSIS-DAP DAP_SWJ_Pins */
uint8_t swdPinsControl(uint8_t pinOut, uint8_t pinSelect, uint32_t waitUs);

/* Bit sequences for CMSIS-DAP DAP_SWJ_Sequence and DAP_SWD_Sequence */
void swdSwjSequence(uint32_t bitCount, const uint8_t *data);
void swdSequenceOut(uint32_t bitCount, const uint8_t *data);
void swdSequenceIn(uint32_t bitCount, uint8_t *data);

/* Single CMSIS-DAP SWD transfer with wait_retry & idle_cycles (returns DAP_TRANSFER_*) */
uint8_t swdDapTransfer(uint8_t reqCmd, uint32_t *data);

/* High-level ADIv5 SWD API (ported from ESP_OTA_STM32) */
void swdLineReset(void);
void swdJtagToSwd(void);
uint32_t swdConnect(void);

bool swdReadDP(uint8_t addr, uint32_t *out);
bool swdWriteDP(uint8_t addr, uint32_t val);
bool swdReadAP(uint8_t addr, uint32_t *out);
bool swdWriteAP(uint8_t addr, uint32_t val);
void swdClearErrors(void);
bool swdPowerUp(void);

bool memRead32(uint32_t addr, uint32_t *out);
bool memWrite32(uint32_t addr, uint32_t val);
bool memWrite16(uint32_t addr, uint16_t val);
bool memReadBlock(uint32_t addr, uint32_t *words, size_t count);
bool memWriteBlock(uint32_t addr, const uint32_t *words, size_t count);

int swdLastError(void);
const char *swdErrorStr(int err);

#ifdef __cplusplus
}
#endif

#endif /* __SWD_H__ */
