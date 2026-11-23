#pragma once

#include <Arduino.h>

// ADIv5 DP register addresses (A[3:2] as byte offsets).
#define DP_IDCODE     0x00   // read
#define DP_ABORT      0x00   // write
#define DP_CTRL_STAT  0x04
#define DP_SELECT     0x08
#define DP_RDBUFF     0x0C

// MEM-AP registers (AP bank 0).
#define AP_CSW        0x00
#define AP_TAR        0x04
#define AP_DRW        0x0C

#define CSW_32BIT_INC 0x23000012UL   // 32-bit, single auto-increment
#define ABORT_CLEAR   0x1EUL         // clear STKCMP/STKERR/WDERR/ORUNERR

// ACK values / error codes.
#define SWD_ACK_OK     1
#define SWD_ACK_WAIT   2
#define SWD_ACK_FAULT  4
#define SWD_ERR_PARITY (-1)
#define SWD_ERR_NOACK  (-2)
#define SWD_ERR_TIMEOUT (-3)

void swdInit();          // configure pins (call before every session)
void swdRelease();       // float both pins (leave target alone)
void swdLineReset();     // >=50 clocks SWDIO high + 2 idle
void swdJtagToSwd();     // 0xE79E switch sequence

// Line reset + JTAG->SWD + IDCODE read. Returns IDCODE, 0 on failure.
uint32_t swdConnect();

bool swdReadDP(uint8_t addr, uint32_t* out);
bool swdWriteDP(uint8_t addr, uint32_t val);
bool swdReadAP(uint8_t addr, uint32_t* out);   // includes RDBUFF fetch
bool swdWriteAP(uint8_t addr, uint32_t val);
void swdClearErrors();

// CTRL/STAT power-up + MEM-AP CSW setup.
bool swdPowerUp();

bool memRead32(uint32_t addr, uint32_t* out);
bool memWrite32(uint32_t addr, uint32_t val);

// Block transfers using TAR auto-increment (handles the 1 KB TAR wrap).
// addr must be word aligned.
bool memWriteBlock(uint32_t addr, const uint32_t* words, size_t count);
bool memReadBlock(uint32_t addr, uint32_t* words, size_t count);

int swdLastError();
const char* swdErrorStr(int err);
