#ifndef __STM32_TARGET_H__
#define __STM32_TARGET_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Supported STM32 Flash Controller Families */
typedef enum
{
  STM32_FAMILY_UNKNOWN = 0,
  STM32_FAMILY_F1_F3,   /* Page erase (1KB/2KB), 16-bit half-word write (0x40022000) */
  STM32_FAMILY_F2_F4,   /* Sector erase (16K/64K/128K), 32-bit word write (0x40023C00) */
  STM32_FAMILY_G4_L4,   /* Page erase (2KB/4KB), 64-bit double-word write (0x40022000) */
  STM32_FAMILY_F0_G0    /* Detected for probe / core control */
} Stm32Family_t;

typedef struct
{
  uint32_t      dpIdcode;
  uint32_t      dbgIdcode;
  uint16_t      devId;
  uint16_t      revId;
  uint32_t      flashSizeKB;
  uint32_t      pageOrSectorSize;
  uint8_t       rdpLevel;
  Stm32Family_t family;
  const char   *name;
} Stm32TargetInfo_t;

typedef void (*Stm32TargetProgressFn)(const char *stage, uint32_t done, uint32_t total);

/* Lightweight SWD link check (reads DP IDCODE only, does not halt core) */
bool stm32TargetCheckLink(uint32_t *dpIdOut);

/* Connect over SWD, halt core (with connect-under-reset fallback on PB8),
   auto-detect STM32F1 / F3 / F4 / G4 / L4 / F0 / G0, freeze watchdogs */
bool stm32TargetConnect(Stm32TargetInfo_t *info, char *err, size_t errLen);

/* Full SWD probe: connect, identify chip, then release core running */
bool stm32TargetProbe(Stm32TargetInfo_t *info, char *err, size_t errLen);

/* Halt or resume target CPU */
bool stm32TargetHalt(char *err, size_t errLen);
bool stm32TargetResume(char *err, size_t errLen);

/* Reset target via SWD AIRCR.SYSRESETREQ or PB8 NRST pin */
bool stm32TargetReset(char *err, size_t errLen);

/* Multi-family Flash Erase, Program, and Verify (supports F1/F3, F2/F4, G4/L4) */
bool stm32TargetFlashBuffer(uint32_t flashAddr,
                            const uint8_t *data,
                            uint32_t length,
                            Stm32TargetProgressFn progress,
                            char *err,
                            size_t errLen);

#ifdef __cplusplus
}
#endif

#endif /* __STM32_TARGET_H__ */
