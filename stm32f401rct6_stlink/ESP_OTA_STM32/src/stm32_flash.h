#pragma once

#include <Arduino.h>

struct Stm32Info {
  uint32_t dpIdcode;
  uint32_t dbgIdcode;
  uint16_t devId;
  uint16_t revId;
  uint32_t flashSizeKB;
  uint8_t  rdpLevel;     // 0xAA = level 0 (unprotected)
};

typedef void (*Stm32ProgressFn)(const char* stage, uint32_t done, uint32_t total);

// Lightweight SWD link check (reads DP IDCODE only, does not halt core).
bool stm32CheckLink(uint32_t* dpIdOut);

// Full SWD probe: connect, read DP/DBGMCU/flash size/RDP, then release core
// running. Fills info and returns true if target is an accessible STM32F401.
bool stm32Probe(Stm32Info* info, char* err, size_t errLen);

// Connect over SWD, power up, halt the core (connect-under-reset fallback if
// NRST is wired), identify the chip, check RDP and freeze watchdogs.
bool stm32Connect(Stm32Info* info, char* err, size_t errLen);

// Full job: erase needed sectors, program from LittleFS, verify, reset & run.
bool stm32FlashFile(const char* path, Stm32ProgressFn progress, char* err, size_t errLen);

// Reset the STM32: SWD AIRCR.SYSRESETREQ, falling back to NRST pulse.
bool stm32Reset(char* err, size_t errLen);

// Pulse NRST (if STM32_NRST_PIN >= 0).
void stm32HardReset();
