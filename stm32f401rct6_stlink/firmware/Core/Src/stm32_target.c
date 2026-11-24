/**
  ******************************************************************************
  * @file    stm32_target.c
  * @brief   Multi-family STM32 target probe, core control, and flash programmer
  *          over SWD (ported from ESP_OTA_STM32 and generalized to support
  *          STM32F1, STM32F3, STM32F4, STM32G4, STM32L4, etc.).
  ******************************************************************************
  */

#include "stm32_target.h"
#include "swd.h"
#include "led_status.h"
#include <stdio.h>
#include <string.h>

/* Cortex-M Debug Registers */
#define DHCSR                  0xE000EDF0UL
#define DEMCR                  0xE000EDFCUL
#define AIRCR                  0xE000ED0CUL
#define DHCSR_HALT             0xA05F0003UL   /* DBGKEY | C_HALT | C_DEBUGEN */
#define DHCSR_RUN              0xA05F0001UL   /* DBGKEY | C_DEBUGEN */
#define DHCSR_S_HALT           (1UL << 17)
#define DEMCR_VC_CORERESET     (1UL << 0)
#define AIRCR_SYSRESET         0x05FA0004UL

/* STM32 DBGMCU IDCODE addresses */
#define DBGMCU_IDCODE_M3_M4    0xE0042000UL   /* STM32F1, F2, F3, F4, G4, L4 */
#define DBGMCU_IDCODE_M0       0x40015800UL   /* STM32F0, G0 */
#define DBGMCU_APB1_FZ_M3_M4   0xE0042008UL
#define DBG_WWDG_STOP          (1UL << 11)
#define DBG_IWDG_STOP          (1UL << 12)

/* Flash Size Registers by family */
#define FSIZE_REG_F1_F3        0x1FFFF7E0UL
#define FSIZE_REG_F4           0x1FFF7A20UL
#define FSIZE_REG_G4_L4        0x1FFF75E0UL

#define STM32_FLASH_BASE       0x08000000UL

/* Flash unlock keys (common across F1, F3, F4, G4, L4) */
#ifndef FLASH_KEY1
#define FLASH_KEY1             0x45670123UL
#endif
#ifndef FLASH_KEY2
#define FLASH_KEY2             0xCDEF89ABUL
#endif

/* --- 1. STM32F1 / F3 Flash Controller (0x40022000) --- */
#define F1_FLASH_BASE          0x40022000UL
#define F1_FLASH_KEYR          (F1_FLASH_BASE + 0x04UL)
#define F1_FLASH_SR            (F1_FLASH_BASE + 0x0CUL)
#define F1_FLASH_CR            (F1_FLASH_BASE + 0x10UL)
#define F1_FLASH_AR            (F1_FLASH_BASE + 0x14UL)
#define F1_FLASH_OBR           (F1_FLASH_BASE + 0x1CUL)
#define F1_SR_BSY              (1UL << 0)
#define F1_SR_PGERR            (1UL << 2)
#define F1_SR_WRPRTERR         (1UL << 4)
#define F1_SR_EOP              (1UL << 5)
#define F1_SR_ERRORS           (F1_SR_PGERR | F1_SR_WRPRTERR)
#define F1_CR_PG               (1UL << 0)
#define F1_CR_PER              (1UL << 1)
#define F1_CR_STRT             (1UL << 6)
#define F1_CR_LOCK             (1UL << 7)

/* --- 2. STM32F2 / F4 Flash Controller (0x40023C00) --- */
#define F4_FLASH_BASE          0x40023C00UL
#define F4_FLASH_KEYR          (F4_FLASH_BASE + 0x04UL)
#define F4_FLASH_SR            (F4_FLASH_BASE + 0x0CUL)
#define F4_FLASH_CR            (F4_FLASH_BASE + 0x10UL)
#define F4_FLASH_OPTCR         (F4_FLASH_BASE + 0x14UL)
#define F4_SR_EOP              (1UL << 0)
#define F4_SR_OPERR            (1UL << 1)
#define F4_SR_WRPERR           (1UL << 4)
#define F4_SR_PGAERR           (1UL << 5)
#define F4_SR_PGPERR           (1UL << 6)
#define F4_SR_PGSERR           (1UL << 7)
#define F4_SR_RDERR            (1UL << 8)
#define F4_SR_BSY              (1UL << 16)
#define F4_SR_ERRORS           (F4_SR_OPERR | F4_SR_WRPERR | F4_SR_PGAERR | F4_SR_PGPERR | F4_SR_PGSERR | F4_SR_RDERR)
#define F4_CR_PG               (1UL << 0)
#define F4_CR_SER              (1UL << 1)
#define F4_CR_SNB(n)           ((uint32_t)(n) << 3)
#define F4_CR_PSIZE_X32        (2UL << 8)
#define F4_CR_STRT             (1UL << 16)
#define F4_CR_LOCK             (1UL << 31)

/* --- 3. STM32G4 / L4 Flash Controller (0x40022000) --- */
#define G4_FLASH_BASE          0x40022000UL
#define G4_FLASH_KEYR          (G4_FLASH_BASE + 0x08UL)
#define G4_FLASH_SR            (G4_FLASH_BASE + 0x10UL)
#define G4_FLASH_CR            (G4_FLASH_BASE + 0x14UL)
#define G4_FLASH_OPTR          (G4_FLASH_BASE + 0x20UL)
#define G4_SR_EOP              (1UL << 0)
#define G4_SR_OPERR            (1UL << 1)
#define G4_SR_PROGERR          (1UL << 3)
#define G4_SR_WRPERR           (1UL << 4)
#define G4_SR_PGAERR           (1UL << 5)
#define G4_SR_SIZERR           (1UL << 6)
#define G4_SR_PGSERR           (1UL << 7)
#define G4_SR_MISERR           (1UL << 8)
#define G4_SR_FASTERR          (1UL << 9)
#define G4_SR_BSY              (1UL << 16)
#define G4_SR_ERRORS           (G4_SR_OPERR | G4_SR_PROGERR | G4_SR_WRPERR | G4_SR_PGAERR | G4_SR_SIZERR | G4_SR_PGSERR | G4_SR_MISERR | G4_SR_FASTERR)
#define G4_CR_PG               (1UL << 0)
#define G4_CR_PER              (1UL << 1)
#define G4_CR_PNB(p)           (((uint32_t)(p) & 0x7FUL) << 3)
#define G4_CR_STRT             (1UL << 16)
#define G4_CR_LOCK             (1UL << 31)

#define SETERR(...) do { if (err != NULL && errLen > 0U) snprintf(err, errLen, __VA_ARGS__); } while (0)

/* -------------------------------------------------------------------------- */
/* Multi-family Device Table (F1, F3, F4, G4, L4, F0, G0)                     */
/* -------------------------------------------------------------------------- */

typedef struct
{
  uint16_t      devId;
  Stm32Family_t family;
  uint32_t      pageOrSectorSize;
  const char   *name;
} Stm32DeviceEntry_t;

static const Stm32DeviceEntry_t kDeviceTable[] =
{
  /* STM32F1 series */
  { 0x412U, STM32_FAMILY_F1_F3, 1024U, "STM32F10x Low-density" },
  { 0x410U, STM32_FAMILY_F1_F3, 1024U, "STM32F10x Medium-density (F103C8/RB)" },
  { 0x414U, STM32_FAMILY_F1_F3, 2048U, "STM32F10x High-density (F103RC/ZE)" },
  { 0x418U, STM32_FAMILY_F1_F3, 2048U, "STM32F105/F107 Connectivity" },
  { 0x420U, STM32_FAMILY_F1_F3, 1024U, "STM32F100 Value Line" },
  { 0x428U, STM32_FAMILY_F1_F3, 2048U, "STM32F100 High-density Value Line" },
  { 0x430U, STM32_FAMILY_F1_F3, 2048U, "STM32F10x XL-density" },

  /* STM32F3 series (shares F1 flash controller) */
  { 0x422U, STM32_FAMILY_F1_F3, 2048U, "STM32F302/F303xB/C" },
  { 0x432U, STM32_FAMILY_F1_F3, 2048U, "STM32F373" },
  { 0x438U, STM32_FAMILY_F1_F3, 2048U, "STM32F303x6/8 / F334" },
  { 0x439U, STM32_FAMILY_F1_F3, 2048U, "STM32F301/F302x6/8" },
  { 0x446U, STM32_FAMILY_F1_F3, 2048U, "STM32F302/F303xD/E" },

  /* STM32F2 / STM32F4 series */
  { 0x411U, STM32_FAMILY_F2_F4, 16384U, "STM32F20x/F21x" },
  { 0x413U, STM32_FAMILY_F2_F4, 16384U, "STM32F405/F407/F415/F417" },
  { 0x419U, STM32_FAMILY_F2_F4, 16384U, "STM32F427/F429/F437/F439" },
  { 0x421U, STM32_FAMILY_F2_F4, 16384U, "STM32F446" },
  { 0x423U, STM32_FAMILY_F2_F4, 16384U, "STM32F401xB/C" },
  { 0x431U, STM32_FAMILY_F2_F4, 16384U, "STM32F411xC/E" },
  { 0x433U, STM32_FAMILY_F2_F4, 16384U, "STM32F401xD/E" },
  { 0x434U, STM32_FAMILY_F2_F4, 16384U, "STM32F469/F479" },
  { 0x441U, STM32_FAMILY_F2_F4, 16384U, "STM32F412" },
  { 0x458U, STM32_FAMILY_F2_F4, 16384U, "STM32F410" },
  { 0x463U, STM32_FAMILY_F2_F4, 16384U, "STM32F413/F423" },

  /* STM32G4 series */
  { 0x468U, STM32_FAMILY_G4_L4, 2048U, "STM32G431/G441 (Cat.2)" },
  { 0x469U, STM32_FAMILY_G4_L4, 2048U, "STM32G473/G474/G484 (Cat.3)" },
  { 0x479U, STM32_FAMILY_G4_L4, 2048U, "STM32G491/G4A1 (Cat.4)" },

  /* STM32L4 series (shares G4 double-word flash controller) */
  { 0x415U, STM32_FAMILY_G4_L4, 2048U, "STM32L475/L476/L486" },
  { 0x435U, STM32_FAMILY_G4_L4, 2048U, "STM32L43x/L44x" },
  { 0x462U, STM32_FAMILY_G4_L4, 2048U, "STM32L45x/L46x" },
  { 0x464U, STM32_FAMILY_G4_L4, 2048U, "STM32L41x/L42x" },

  /* STM32F0 / G0 series */
  { 0x440U, STM32_FAMILY_F0_G0, 1024U, "STM32F05x/F030x8" },
  { 0x444U, STM32_FAMILY_F0_G0, 1024U, "STM32F03x/F04x" },
  { 0x445U, STM32_FAMILY_F0_G0, 2048U, "STM32F07x" },
  { 0x448U, STM32_FAMILY_F0_G0, 2048U, "STM32F09x/F030xC" },
  { 0x460U, STM32_FAMILY_F0_G0, 2048U, "STM32G07x/G08x" },
  { 0x466U, STM32_FAMILY_F0_G0, 2048U, "STM32G03x/G04x" },
  { 0x467U, STM32_FAMILY_F0_G0, 2048U, "STM32G0Bx/G0Cx" },
};

static const Stm32DeviceEntry_t *lookupDevice(uint16_t devId)
{
  for (size_t i = 0U; i < (sizeof(kDeviceTable) / sizeof(kDeviceTable[0])); i++)
  {
    if (kDeviceTable[i].devId == devId)
    {
      return &kDeviceTable[i];
    }
  }
  return NULL;
}

/* -------------------------------------------------------------------------- */
/* Core Control Helpers                                                       */
/* -------------------------------------------------------------------------- */

static bool waitHalted(uint32_t timeoutMs)
{
  uint32_t start = HAL_GetTick();
  do
  {
    uint32_t v = 0U;
    if (memRead32(DHCSR, &v) && ((v & DHCSR_S_HALT) != 0U))
    {
      return true;
    }
    HAL_Delay(1);
  } while ((HAL_GetTick() - start) < timeoutMs);
  return false;
}

static bool attachAndHalt(uint32_t *dpId)
{
  swdInit();
  *dpId = swdConnect();
  if (*dpId == 0U)
  {
    return false;
  }
  if (!swdPowerUp())
  {
    return false;
  }
  if (!memWrite32(DHCSR, DHCSR_HALT))
  {
    return false;
  }
  return waitHalted(100U);
}

static bool connectUnderReset(uint32_t *dpId)
{
  swdInit();
  swdSetReset(true);
  HAL_Delay(10);

  bool ok = false;
  *dpId = swdConnect();
  if (*dpId != 0U && swdPowerUp())
  {
    memWrite32(DHCSR, DHCSR_HALT);
    memWrite32(DEMCR, DEMCR_VC_CORERESET);
    ok = true;
  }
  swdSetReset(false);
  HAL_Delay(20);
  if (!ok)
  {
    return false;
  }

  bool halted = waitHalted(200U);
  memWrite32(DEMCR, 0U);
  return halted;
}

static void releaseTarget(bool run)
{
  if (run)
  {
    memWrite32(DEMCR, 0U);
    memWrite32(DHCSR, DHCSR_RUN);
  }
  swdWriteDP(DP_CTRL_STAT, 0U);
  swdRelease();
}

bool stm32TargetCheckLink(uint32_t *dpIdOut)
{
  swdInit();
  uint32_t dpId = swdConnect();
  swdRelease();
  if (dpIdOut != NULL)
  {
    *dpIdOut = dpId;
  }
  return (dpId != 0U);
}

bool stm32TargetConnect(Stm32TargetInfo_t *info, char *err, size_t errLen)
{
  memset(info, 0, sizeof(*info));
  info->name = "Unknown Cortex-M";

  uint32_t dpId = 0U;
  bool halted = attachAndHalt(&dpId);
  if (!halted)
  {
    halted = connectUnderReset(&dpId);
  }
  info->dpIdcode = dpId;

  if (dpId == 0U)
  {
    SETERR("No SWD target (%s)", swdErrorStr(swdLastError()));
    swdRelease();
    return false;
  }
  if (!halted)
  {
    SETERR("Core halt failed (DP=0x%08lX)", (unsigned long)dpId);
    swdRelease();
    return false;
  }

  /* Read DBGMCU_IDCODE (Cortex-M3/M4/M7 first, fallback to Cortex-M0/M0+) */
  uint32_t dbgId = 0U;
  if (!memRead32(DBGMCU_IDCODE_M3_M4, &dbgId) || (dbgId & 0xFFFU) == 0U)
  {
    memRead32(DBGMCU_IDCODE_M0, &dbgId);
  }
  info->dbgIdcode = dbgId;
  info->devId     = (uint16_t)(dbgId & 0xFFFU);
  info->revId     = (uint16_t)(dbgId >> 16U);

  const Stm32DeviceEntry_t *entry = lookupDevice(info->devId);
  if (entry != NULL)
  {
    info->family           = entry->family;
    info->pageOrSectorSize = entry->pageOrSectorSize;
    info->name             = entry->name;
  }

  /* Read family-specific Flash size and RDP level */
  uint32_t rawFsz = 0U;
  uint32_t optReg = 0U;
  switch (info->family)
  {
    case STM32_FAMILY_F1_F3:
      if (memRead32(FSIZE_REG_F1_F3, &rawFsz))
      {
        info->flashSizeKB = rawFsz & 0xFFFFUL;
      }
      if (memRead32(F1_FLASH_OBR, &optReg))
      {
        info->rdpLevel = ((optReg & (1UL << 1)) != 0U) ? 0xBBU : 0xAAU;
      }
      break;

    case STM32_FAMILY_F2_F4:
      if (memRead32(FSIZE_REG_F4, &rawFsz))
      {
        info->flashSizeKB = (rawFsz >> 16U) & 0xFFFFUL;
      }
      if (memRead32(F4_FLASH_OPTCR, &optReg))
      {
        info->rdpLevel = (uint8_t)((optReg >> 8U) & 0xFFU);
      }
      break;

    case STM32_FAMILY_G4_L4:
      if (memRead32(FSIZE_REG_G4_L4, &rawFsz))
      {
        info->flashSizeKB = rawFsz & 0xFFFFUL;
      }
      if (memRead32(G4_FLASH_OPTR, &optReg))
      {
        info->rdpLevel = (uint8_t)(optReg & 0xFFU);
      }
      break;

    default:
      info->rdpLevel = 0xAAU;
      break;
  }

  if (info->flashSizeKB == 0xFFFFUL)
  {
    info->flashSizeKB = 0U;
  }

  /* Freeze IWDG & WWDG on M3/M4 targets while halted */
  if (info->family != STM32_FAMILY_F0_G0)
  {
    uint32_t fz = 0U;
    if (memRead32(DBGMCU_APB1_FZ_M3_M4, &fz))
    {
      memWrite32(DBGMCU_APB1_FZ_M3_M4, fz | DBG_IWDG_STOP | DBG_WWDG_STOP);
    }
  }

  return true;
}

bool stm32TargetProbe(Stm32TargetInfo_t *info, char *err, size_t errLen)
{
  if (!stm32TargetConnect(info, err, errLen))
  {
    ledStatusSetErrorTemporary(2000U);
    return false;
  }
  releaseTarget(true);
  ledStatusSetState(LED_STATE_CONNECTED);
  return true;
}

bool stm32TargetHalt(char *err, size_t errLen)
{
  uint32_t dpId = 0U;
  if (!attachAndHalt(&dpId) && !connectUnderReset(&dpId))
  {
    SETERR("Failed to halt target (%s)", swdErrorStr(swdLastError()));
    swdRelease();
    ledStatusSetErrorTemporary(2000U);
    return false;
  }
  ledStatusSetState(LED_STATE_DEBUG_HALTED);
  return true;
}

bool stm32TargetResume(char *err, size_t errLen)
{
  swdInit();
  if (!swdConnect() || !swdPowerUp())
  {
    SETERR("SWD connect failed (%s)", swdErrorStr(swdLastError()));
    swdRelease();
    ledStatusSetErrorTemporary(2000U);
    return false;
  }
  releaseTarget(true);
  ledStatusSetState(LED_STATE_CONNECTED);
  return true;
}

bool stm32TargetReset(char *err, size_t errLen)
{
  ledStatusActivityPulse();
  swdInit();
  if (swdConnect() && swdPowerUp())
  {
    memWrite32(DEMCR, 0U);
    memWrite32(DHCSR, DHCSR_RUN);
    memWrite32(AIRCR, AIRCR_SYSRESET);
    HAL_Delay(20);
    swdWriteDP(DP_CTRL_STAT, 0U);
    swdRelease();
    return true;
  }
  swdRelease();
  swdHardResetPulse(25U, 15U);
  (void)err;
  (void)errLen;
  return true;
}

/* -------------------------------------------------------------------------- */
/* Multi-Family Flash Programming (STM32F1/F3, STM32F2/F4, STM32G4/L4)        */
/* -------------------------------------------------------------------------- */

static bool waitFlashReady(uint32_t srAddr, uint32_t bsyMask, uint32_t errMask,
                           uint32_t eopMask, uint32_t timeoutMs, char *err, size_t errLen)
{
  uint32_t start = HAL_GetTick();
  for (;;)
  {
    uint32_t sr = 0U;
    if (!memRead32(srAddr, &sr))
    {
      SETERR("SWD lost polling FLASH_SR (%s)", swdErrorStr(swdLastError()));
      return false;
    }
    if ((sr & errMask) != 0U)
    {
      SETERR("Flash error SR=0x%08lX", (unsigned long)sr);
      memWrite32(srAddr, errMask | eopMask);
      return false;
    }
    if ((sr & bsyMask) == 0U)
    {
      return true;
    }
    if ((HAL_GetTick() - start) > timeoutMs)
    {
      SETERR("Flash timeout SR=0x%08lX", (unsigned long)sr);
      return false;
    }
  }
}

/* --- Family 1: STM32F1 / STM32F3 (Page Erase + 16-bit Half-Word Program) --- */
static bool flashF1F3(uint32_t flashAddr, const uint8_t *data, uint32_t length,
                      uint32_t pageSize, Stm32TargetProgressFn progress,
                      char *err, size_t errLen)
{
  uint32_t cr = 0U;
  if (!memRead32(F1_FLASH_CR, &cr)) { SETERR("Cannot read F1 FLASH_CR"); return false; }
  if ((cr & F1_CR_LOCK) != 0U)
  {
    memWrite32(F1_FLASH_KEYR, FLASH_KEY1);
    memWrite32(F1_FLASH_KEYR, FLASH_KEY2);
    if (!memRead32(F1_FLASH_CR, &cr) || ((cr & F1_CR_LOCK) != 0U))
    {
      SETERR("F1/F3 Flash unlock failed");
      return false;
    }
  }

  uint32_t numPages = (length + pageSize - 1U) / pageSize;
  for (uint32_t p = 0U; p < numPages; p++)
  {
    if (progress != NULL) progress("erase", p, numPages);
    uint32_t pageAddr = flashAddr + p * pageSize;
    if (!waitFlashReady(F1_FLASH_SR, F1_SR_BSY, F1_SR_ERRORS, F1_SR_EOP, 100U, err, errLen))
      return false;
    memWrite32(F1_FLASH_CR, F1_CR_PER);
    memWrite32(F1_FLASH_AR, pageAddr);
    memWrite32(F1_FLASH_CR, F1_CR_PER | F1_CR_STRT);
    if (!waitFlashReady(F1_FLASH_SR, F1_SR_BSY, F1_SR_ERRORS, F1_SR_EOP, 500U, err, errLen))
      return false;
  }
  memWrite32(F1_FLASH_CR, 0U);

  /* Program half-words (16-bit) */
  for (uint32_t off = 0U; off < length; off += 2U)
  {
    uint16_t hw = data[off];
    if ((off + 1U) < length) hw |= (uint16_t)((uint16_t)data[off + 1U] << 8U);
    else                     hw |= 0xFF00U;

    memWrite32(F1_FLASH_CR, F1_CR_PG);
    if (!memWrite16(flashAddr + off, hw))
    {
      memWrite32(F1_FLASH_CR, F1_CR_LOCK);
      SETERR("SWD write16 failed at 0x%08lX", (unsigned long)(flashAddr + off));
      return false;
    }
    if (!waitFlashReady(F1_FLASH_SR, F1_SR_BSY, F1_SR_ERRORS, F1_SR_EOP, 100U, err, errLen))
    {
      memWrite32(F1_FLASH_CR, F1_CR_LOCK);
      return false;
    }
    if (progress != NULL && ((off & 0x3FFU) == 0U))
    {
      progress("program", off, length);
    }
  }

  memWrite32(F1_FLASH_CR, F1_CR_LOCK);
  return true;
}

/* --- Family 2: STM32F2 / STM32F4 (Sector Erase + 32-bit Word Program) --- */
static uint32_t f4SectorSize(uint32_t sectorIdx)
{
  if (sectorIdx < 4U)  return 16U * 1024U;
  if (sectorIdx == 4U) return 64U * 1024U;
  return 128U * 1024U;
}

static bool flashF2F4(uint32_t flashAddr, const uint8_t *data, uint32_t length,
                      Stm32TargetProgressFn progress, char *err, size_t errLen)
{
  uint32_t cr = 0U;
  if (!memRead32(F4_FLASH_CR, &cr)) { SETERR("Cannot read F4 FLASH_CR"); return false; }
  if ((cr & F4_CR_LOCK) != 0U)
  {
    memWrite32(F4_FLASH_KEYR, FLASH_KEY1);
    memWrite32(F4_FLASH_KEYR, FLASH_KEY2);
    if (!memRead32(F4_FLASH_CR, &cr) || ((cr & F4_CR_LOCK) != 0U))
    {
      SETERR("F4 Flash unlock failed");
      return false;
    }
  }

  uint32_t covered = 0U;
  uint32_t neededSectors = 0U;
  while (covered < length && neededSectors < 12U)
  {
    covered += f4SectorSize(neededSectors++);
  }

  for (uint32_t s = 0U; s < neededSectors; s++)
  {
    if (progress != NULL) progress("erase", s, neededSectors);
    if (!waitFlashReady(F4_FLASH_SR, F4_SR_BSY, F4_SR_ERRORS, F4_SR_EOP, 100U, err, errLen))
      return false;
    memWrite32(F4_FLASH_SR, F4_SR_ERRORS | F4_SR_EOP);
    memWrite32(F4_FLASH_CR, F4_CR_SER | F4_CR_SNB(s) | F4_CR_PSIZE_X32);
    memWrite32(F4_FLASH_CR, F4_CR_SER | F4_CR_SNB(s) | F4_CR_PSIZE_X32 | F4_CR_STRT);
    if (!waitFlashReady(F4_FLASH_SR, F4_SR_BSY, F4_SR_ERRORS, F4_SR_EOP, 5000U, err, errLen))
      return false;
  }
  memWrite32(F4_FLASH_CR, F4_CR_PG | F4_CR_PSIZE_X32);

  for (uint32_t off = 0U; off < length; off += 4U)
  {
    uint32_t w = 0xFFFFFFFFUL;
    uint32_t rem = length - off;
    memcpy(&w, &data[off], (rem >= 4U) ? 4U : rem);

    if (!memWrite32(flashAddr + off, w))
    {
      memWrite32(F4_FLASH_CR, F4_CR_LOCK);
      SETERR("SWD write32 failed at 0x%08lX", (unsigned long)(flashAddr + off));
      return false;
    }
    if (!waitFlashReady(F4_FLASH_SR, F4_SR_BSY, F4_SR_ERRORS, F4_SR_EOP, 100U, err, errLen))
    {
      memWrite32(F4_FLASH_CR, F4_CR_LOCK);
      return false;
    }
    if (progress != NULL && ((off & 0x3FFU) == 0U))
    {
      progress("program", off, length);
    }
  }

  memWrite32(F4_FLASH_CR, F4_CR_LOCK);
  return true;
}

/* --- Family 3: STM32G4 / STM32L4 (Page Erase + 64-bit Double-Word Program) --- */
static bool flashG4L4(uint32_t flashAddr, const uint8_t *data, uint32_t length,
                      uint32_t pageSize, Stm32TargetProgressFn progress,
                      char *err, size_t errLen)
{
  uint32_t cr = 0U;
  if (!memRead32(G4_FLASH_CR, &cr)) { SETERR("Cannot read G4/L4 FLASH_CR"); return false; }
  if ((cr & G4_CR_LOCK) != 0U)
  {
    memWrite32(G4_FLASH_KEYR, FLASH_KEY1);
    memWrite32(G4_FLASH_KEYR, FLASH_KEY2);
    if (!memRead32(G4_FLASH_CR, &cr) || ((cr & G4_CR_LOCK) != 0U))
    {
      SETERR("G4/L4 Flash unlock failed");
      return false;
    }
  }

  uint32_t numPages = (length + pageSize - 1U) / pageSize;
  for (uint32_t p = 0U; p < numPages; p++)
  {
    if (progress != NULL) progress("erase", p, numPages);
    if (!waitFlashReady(G4_FLASH_SR, G4_SR_BSY, G4_SR_ERRORS, G4_SR_EOP, 100U, err, errLen))
      return false;
    memWrite32(G4_FLASH_SR, G4_SR_ERRORS | G4_SR_EOP);
    memWrite32(G4_FLASH_CR, G4_CR_PER | G4_CR_PNB(p));
    memWrite32(G4_FLASH_CR, G4_CR_PER | G4_CR_PNB(p) | G4_CR_STRT);
    if (!waitFlashReady(G4_FLASH_SR, G4_SR_BSY, G4_SR_ERRORS, G4_SR_EOP, 1000U, err, errLen))
      return false;
  }

  memWrite32(G4_FLASH_CR, G4_CR_PG);
  for (uint32_t off = 0U; off < length; off += 8U)
  {
    uint32_t dw[2] = { 0xFFFFFFFFUL, 0xFFFFFFFFUL };
    uint32_t rem = length - off;
    memcpy(dw, &data[off], (rem >= 8U) ? 8U : rem);

    if (!memWrite32(flashAddr + off, dw[0]) ||
        !memWrite32(flashAddr + off + 4U, dw[1]))
    {
      memWrite32(G4_FLASH_CR, G4_CR_LOCK);
      SETERR("SWD double-word write failed at 0x%08lX", (unsigned long)(flashAddr + off));
      return false;
    }
    if (!waitFlashReady(G4_FLASH_SR, G4_SR_BSY, G4_SR_ERRORS, G4_SR_EOP, 100U, err, errLen))
    {
      memWrite32(G4_FLASH_CR, G4_CR_LOCK);
      return false;
    }
    if (progress != NULL && ((off & 0x3FFU) == 0U))
    {
      progress("program", off, length);
    }
  }

  memWrite32(G4_FLASH_CR, G4_CR_LOCK);
  return true;
}

bool stm32TargetFlashBuffer(uint32_t flashAddr,
                            const uint8_t *data,
                            uint32_t length,
                            Stm32TargetProgressFn progress,
                            char *err,
                            size_t errLen)
{
  if (flashAddr == 0U)
  {
    flashAddr = STM32_FLASH_BASE;
  }

  Stm32TargetInfo_t info;
  if (!stm32TargetConnect(&info, err, errLen))
  {
    ledStatusSetErrorTemporary(2000U);
    return false;
  }

  if (info.rdpLevel != 0xAAU)
  {
    SETERR("Target RDP protection active (0x%02X)", info.rdpLevel);
    releaseTarget(true);
    ledStatusSetErrorTemporary(3000U);
    return false;
  }

  if (info.flashSizeKB > 0U && length > (info.flashSizeKB * 1024UL))
  {
    SETERR("Image %lu B exceeds target flash %lu KB",
           (unsigned long)length, (unsigned long)info.flashSizeKB);
    releaseTarget(true);
    ledStatusSetErrorTemporary(3000U);
    return false;
  }

  /* Set Busy / Flashing state for industrial LED indication */
  ledStatusSetState(LED_STATE_BUSY_FLASHING);

  bool ok = false;
  switch (info.family)
  {
    case STM32_FAMILY_F1_F3:
      ok = flashF1F3(flashAddr, data, length, info.pageOrSectorSize, progress, err, errLen);
      break;

    case STM32_FAMILY_F2_F4:
      ok = flashF2F4(flashAddr, data, length, progress, err, errLen);
      break;

    case STM32_FAMILY_G4_L4:
      ok = flashG4L4(flashAddr, data, length, info.pageOrSectorSize, progress, err, errLen);
      break;

    default:
      SETERR("Standalone flash driver not configured for DEV_ID 0x%03X (use USB CMSIS-DAP)",
             info.devId);
      releaseTarget(true);
      ledStatusSetErrorTemporary(3000U);
      return false;
  }

  if (ok)
  {
    /* Verify written words */
    for (uint32_t off = 0U; off < length; off += 4U)
    {
      ledStatusActivityPulse();
      uint32_t want = 0xFFFFFFFFUL;
      uint32_t got  = 0U;
      uint32_t rem  = length - off;
      memcpy(&want, &data[off], (rem >= 4U) ? 4U : rem);
      if (!memRead32(flashAddr + off, &got) || want != got)
      {
        SETERR("Verify mismatch at 0x%08lX (wrote 0x%08lX read 0x%08lX)",
               (unsigned long)(flashAddr + off), (unsigned long)want, (unsigned long)got);
        ok = false;
        break;
      }
    }
  }

  if (ok)
  {
    if (progress != NULL) progress("verify", length, length);
    stm32TargetReset(err, errLen);
    ledStatusSetState(LED_STATE_CONNECTED);
  }
  else
  {
    releaseTarget(true);
    ledStatusSetErrorTemporary(3000U);
  }

  return ok;
}
