// STM32F401 core control and F4 flash programmer over SWD.

#include "stm32_flash.h"
#include "config.h"
#include "swd.h"

#include <LittleFS.h>

// Cortex-M debug registers.
#define DHCSR          0xE000EDF0UL
#define DEMCR          0xE000EDFCUL
#define AIRCR          0xE000ED0CUL
#define DHCSR_HALT     0xA05F0003UL   // DBGKEY | C_HALT | C_DEBUGEN
#define DHCSR_RUN      0xA05F0001UL   // DBGKEY | C_DEBUGEN
#define DHCSR_S_HALT   (1UL << 17)
#define DEMCR_VC_CORERESET (1UL << 0)
#define AIRCR_SYSRESET 0x05FA0004UL

// STM32 DBGMCU.
#define DBGMCU_IDCODE  0xE0042000UL
#define DBGMCU_APB1_FZ 0xE0042008UL
#define DBG_WWDG_STOP  (1UL << 11)
#define DBG_IWDG_STOP  (1UL << 12)
#define FLASH_SIZE_REG 0x1FFF7A20UL   // F_SIZE (KB) in the upper half-word

// F4 flash interface (RM0090 / RM0368).
#define FLASH_R_BASE   0x40023C00UL
#define FLASH_KEYR     (FLASH_R_BASE + 0x04)
#define FLASH_SR       (FLASH_R_BASE + 0x0C)
#define FLASH_CR       (FLASH_R_BASE + 0x10)
#define FLASH_OPTCR    (FLASH_R_BASE + 0x14)
#define FLASH_KEY1     0x45670123UL
#define FLASH_KEY2     0xCDEF89ABUL

#define SR_EOP         (1UL << 0)
#define SR_OPERR       (1UL << 1)
#define SR_WRPERR      (1UL << 4)
#define SR_PGAERR      (1UL << 5)
#define SR_PGPERR      (1UL << 6)
#define SR_PGSERR      (1UL << 7)
#define SR_RDERR       (1UL << 8)
#define SR_BSY         (1UL << 16)
#define SR_ERRORS      (SR_OPERR | SR_WRPERR | SR_PGAERR | SR_PGPERR | SR_PGSERR | SR_RDERR)

#define CR_PG          (1UL << 0)
#define CR_SER         (1UL << 1)
#define CR_SNB(n)      ((uint32_t)(n) << 3)
#define CR_PSIZE_X32   (2UL << 8)
#define CR_STRT        (1UL << 16)
#define CR_LOCK        (1UL << 31)

#define ERASE_TIMEOUT_MS   5000      // 128 KB sector max ~2 s at x32
#define PROGRAM_TIMEOUT_MS 100

#define CHUNK_WORDS    256           // 1 KB per SWD block / progress step

// STM32F401xC sector map (256 KB).
static const uint32_t kSectorSize[] = {
  16 * 1024, 16 * 1024, 16 * 1024, 16 * 1024, 64 * 1024, 128 * 1024,
};
static const int kSectorCount = sizeof(kSectorSize) / sizeof(kSectorSize[0]);

#define SETERR(...) do { if (err) snprintf(err, errLen, __VA_ARGS__); } while (0)

// ---------------------------------------------------------------------------

void stm32HardReset() {
#if STM32_NRST_PIN >= 0
  pinMode(STM32_NRST_PIN, OUTPUT);
  digitalWrite(STM32_NRST_PIN, LOW);
  delay(20);
  pinMode(STM32_NRST_PIN, INPUT);    // release; STM32 has an internal pull-up
  delay(10);
#endif
}

static bool waitHalted(uint32_t timeoutMs) {
  uint32_t start = millis();
  do {
    uint32_t v = 0;
    if (memRead32(DHCSR, &v) && (v & DHCSR_S_HALT)) return true;
    delay(1);
  } while (millis() - start < timeoutMs);
  return false;
}

static bool attachAndHalt(uint32_t* dpId) {
  swdInit();
  *dpId = swdConnect();
  if (!*dpId) return false;
  if (!swdPowerUp()) return false;
  if (!memWrite32(DHCSR, DHCSR_HALT)) return false;
  return waitHalted(100);
}

#if STM32_NRST_PIN >= 0
// For targets whose firmware disabled SWD or sleeps: hold NRST, attach, arm
// halt-on-reset, then release NRST so the core halts at the reset vector.
static bool connectUnderReset(uint32_t* dpId) {
  pinMode(STM32_NRST_PIN, OUTPUT);
  digitalWrite(STM32_NRST_PIN, LOW);
  delay(10);

  bool ok = false;
  swdInit();
  *dpId = swdConnect();
  if (*dpId && swdPowerUp()) {
    memWrite32(DHCSR, DHCSR_HALT);
    memWrite32(DEMCR, DEMCR_VC_CORERESET);
    ok = true;
  }
  pinMode(STM32_NRST_PIN, INPUT);
  delay(20);
  if (!ok) return false;

  bool halted = waitHalted(200);
  memWrite32(DEMCR, 0);
  return halted;
}
#endif

static void releaseTarget(bool run) {
  if (run) {
    memWrite32(DEMCR, 0);
    memWrite32(DHCSR, DHCSR_RUN);
  }
  swdWriteDP(DP_CTRL_STAT, 0);       // drop debug power-up requests
  swdRelease();
}

bool stm32CheckLink(uint32_t* dpIdOut) {
  swdInit();
  uint32_t dpId = swdConnect();
  swdRelease();
  if (dpIdOut) *dpIdOut = dpId;
  return dpId != 0;
}

bool stm32Probe(Stm32Info* info, char* err, size_t errLen) {
  if (!stm32Connect(info, err, errLen)) return false;
  releaseTarget(true);
  return true;
}

bool stm32Connect(Stm32Info* info, char* err, size_t errLen) {
  memset(info, 0, sizeof(*info));

  uint32_t dpId = 0;
  bool halted = attachAndHalt(&dpId);
#if STM32_NRST_PIN >= 0
  if (!halted) {
    Serial.println("[STM32] normal attach failed, trying connect-under-reset");
    halted = connectUnderReset(&dpId);
  }
#endif
  info->dpIdcode = dpId;
  if (!dpId) {
    SETERR("No SWD target (DP IDCODE read failed: %s). Check SWDIO/SWCLK/GND/power",
           swdErrorStr(swdLastError()));
    swdRelease();
    return false;
  }
  Serial.printf("[SWD] DP IDCODE = 0x%08lX\n", (unsigned long)dpId);
  if (!halted) {
    SETERR("Debug power-up / core halt failed (DP 0x%08lX)", (unsigned long)dpId);
    swdRelease();
    return false;
  }

  if (!memRead32(DBGMCU_IDCODE, &info->dbgIdcode)) {
    SETERR("Cannot read DBGMCU_IDCODE");
    releaseTarget(true);
    return false;
  }
  info->devId = info->dbgIdcode & 0xFFF;
  info->revId = info->dbgIdcode >> 16;
  Serial.printf("[STM32] DBGMCU_IDCODE = 0x%08lX (DEV_ID 0x%03X REV 0x%04X)\n",
                (unsigned long)info->dbgIdcode, info->devId, info->revId);
  if (info->devId != STM32_EXPECTED_DEV_ID) {
    SETERR("Unexpected DEV_ID 0x%03X (expected 0x%03X STM32F401xB/C)",
           info->devId, STM32_EXPECTED_DEV_ID);
    releaseTarget(true);
    return false;
  }

  uint32_t optcr = 0;
  if (!memRead32(FLASH_OPTCR, &optcr)) {
    SETERR("Cannot read FLASH_OPTCR");
    releaseTarget(true);
    return false;
  }
  info->rdpLevel = (optcr >> 8) & 0xFF;
  if (info->rdpLevel != 0xAA) {
    SETERR("Read-out protection active (RDP=0x%02X). Remove RDP with ST-Link/CubeProgrammer first",
           info->rdpLevel);
    releaseTarget(true);
    return false;
  }

  uint32_t fsz = 0;
  if (memRead32(FLASH_SIZE_REG, &fsz)) info->flashSizeKB = fsz >> 16;
  Serial.printf("[STM32] flash size %lu KB, RDP level 0\n", (unsigned long)info->flashSizeKB);

  // Freeze IWDG/WWDG while the core is halted.
  uint32_t fz = 0;
  memRead32(DBGMCU_APB1_FZ, &fz);
  memWrite32(DBGMCU_APB1_FZ, fz | DBG_IWDG_STOP | DBG_WWDG_STOP);
  return true;
}

// ---------------------------------------------------------------------------
// Flash controller helpers

static const char* decodeSr(uint32_t sr) {
  if (sr & SR_WRPERR) return "write-protection error (WRPERR)";
  if (sr & SR_PGAERR) return "alignment error (PGAERR)";
  if (sr & SR_PGPERR) return "parallelism error (PGPERR)";
  if (sr & SR_PGSERR) return "sequence error (PGSERR)";
  if (sr & SR_OPERR)  return "operation error (OPERR)";
  if (sr & SR_RDERR)  return "read protection error (RDERR)";
  return "unknown";
}

static bool flashWaitReady(uint32_t timeoutMs, char* err, size_t errLen) {
  uint32_t start = millis();
  for (;;) {
    uint32_t sr = 0;
    if (!memRead32(FLASH_SR, &sr)) {
      SETERR("SWD lost while polling FLASH_SR (%s)", swdErrorStr(swdLastError()));
      return false;
    }
    if (sr & SR_ERRORS) {
      SETERR("Flash %s, SR=0x%08lX", decodeSr(sr), (unsigned long)sr);
      memWrite32(FLASH_SR, SR_ERRORS | SR_EOP);
      return false;
    }
    if (!(sr & SR_BSY)) return true;
    if (millis() - start > timeoutMs) {
      SETERR("Flash timeout (BSY stuck), SR=0x%08lX", (unsigned long)sr);
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

static bool flashUnlock(char* err, size_t errLen) {
  uint32_t cr = 0;
  if (!memRead32(FLASH_CR, &cr)) { SETERR("Cannot read FLASH_CR"); return false; }
  if (cr & CR_LOCK) {
    memWrite32(FLASH_KEYR, FLASH_KEY1);
    memWrite32(FLASH_KEYR, FLASH_KEY2);
    if (!memRead32(FLASH_CR, &cr) || (cr & CR_LOCK)) {
      SETERR("Flash unlock failed (CR=0x%08lX)", (unsigned long)cr);
      return false;
    }
  }
  return true;
}

static void flashLock() {
  memWrite32(FLASH_CR, CR_LOCK);
}

static bool flashEraseSectors(uint32_t length, Stm32ProgressFn progress, char* err, size_t errLen) {
  int needed = 0;
  uint32_t covered = 0;
  while (needed < kSectorCount && covered < length) covered += kSectorSize[needed++];
  if (covered < length) { SETERR("Image larger than flash"); return false; }

  for (int s = 0; s < needed; s++) {
    if (progress) progress("erase", s, needed);
    Serial.printf("[FLASH] erasing sector %d (%lu KB)\n", s, (unsigned long)(kSectorSize[s] / 1024));
    if (!flashWaitReady(PROGRAM_TIMEOUT_MS, err, errLen)) return false;
    memWrite32(FLASH_SR, SR_ERRORS | SR_EOP);
    if (!memWrite32(FLASH_CR, CR_SER | CR_SNB(s) | CR_PSIZE_X32) ||
        !memWrite32(FLASH_CR, CR_SER | CR_SNB(s) | CR_PSIZE_X32 | CR_STRT)) {
      SETERR("SWD write failed starting erase of sector %d", s);
      return false;
    }
    if (!flashWaitReady(ERASE_TIMEOUT_MS, err, errLen)) return false;
  }
  memWrite32(FLASH_CR, 0);
  if (progress) progress("erase", needed, needed);
  return true;
}

// Read up to CHUNK_WORDS words from the file, 0xFF padded. Returns words read.
static size_t readChunk(File& f, uint32_t* words) {
  uint8_t* bytes = reinterpret_cast<uint8_t*>(words);
  memset(bytes, 0xFF, CHUNK_WORDS * 4);
  size_t got = f.read(bytes, CHUNK_WORDS * 4);
  return (got + 3) / 4;
}

static bool flashProgram(const char* path, uint32_t size, Stm32ProgressFn progress,
                         char* err, size_t errLen) {
  File f = LittleFS.open(path, "r");
  if (!f) { SETERR("Cannot open %s", path); return false; }

  static uint32_t buf[CHUNK_WORDS];
  uint32_t offset = 0;
  if (!memWrite32(FLASH_CR, CR_PG | CR_PSIZE_X32)) {
    f.close();
    SETERR("SWD write failed setting PG");
    return false;
  }

  while (offset < size) {
    size_t n = readChunk(f, buf);
    if (n == 0) { f.close(); SETERR("Short read from %s", path); return false; }
    if (!memWriteBlock(STM32_FLASH_BASE + offset, buf, n)) {
      f.close();
      SETERR("SWD error programming at 0x%08lX (%s)",
             (unsigned long)(STM32_FLASH_BASE + offset), swdErrorStr(swdLastError()));
      return false;
    }
    if (!flashWaitReady(PROGRAM_TIMEOUT_MS, err, errLen)) { f.close(); return false; }
    offset += n * 4;
    if (progress) progress("program", offset < size ? offset : size, size);
    vTaskDelay(1);
  }
  f.close();
  memWrite32(FLASH_CR, 0);
  return true;
}

static bool flashVerify(const char* path, uint32_t size, Stm32ProgressFn progress,
                        char* err, size_t errLen) {
  File f = LittleFS.open(path, "r");
  if (!f) { SETERR("Cannot open %s", path); return false; }

  static uint32_t want[CHUNK_WORDS];
  static uint32_t got[CHUNK_WORDS];
  uint32_t offset = 0;
  while (offset < size) {
    size_t n = readChunk(f, want);
    if (n == 0) { f.close(); SETERR("Short read from %s", path); return false; }
    if (!memReadBlock(STM32_FLASH_BASE + offset, got, n)) {
      f.close();
      SETERR("SWD error reading back 0x%08lX (%s)",
             (unsigned long)(STM32_FLASH_BASE + offset), swdErrorStr(swdLastError()));
      return false;
    }
    for (size_t i = 0; i < n; i++) {
      if (want[i] != got[i]) {
        f.close();
        SETERR("Verify mismatch at 0x%08lX: wrote 0x%08lX read 0x%08lX",
               (unsigned long)(STM32_FLASH_BASE + offset + i * 4),
               (unsigned long)want[i], (unsigned long)got[i]);
        return false;
      }
    }
    offset += n * 4;
    if (progress) progress("verify", offset < size ? offset : size, size);
    vTaskDelay(1);
  }
  f.close();
  return true;
}

static void resetAndRun() {
  memWrite32(DEMCR, 0);                 // no halt on reset
  memWrite32(DHCSR, DHCSR_RUN);         // clear C_HALT (survives system reset)
  memWrite32(AIRCR, AIRCR_SYSRESET);    // may not ACK while resetting
  delay(50);
  if (!memWrite32(DHCSR, DHCSR_RUN)) {
    swdConnect();
    swdPowerUp();
    memWrite32(DHCSR, DHCSR_RUN);
  }
  swdWriteDP(DP_CTRL_STAT, 0);
  swdRelease();
}

// ---------------------------------------------------------------------------

static bool validateStm32F401VectorTable(const char* path, uint32_t size, char* err, size_t errLen) {
  if (size < 8) {
    SETERR("Firmware too small (%lu B < 8 B)", (unsigned long)size);
    return false;
  }
  File f = LittleFS.open(path, "r");
  if (!f) {
    SETERR("Firmware file %s not found", path);
    return false;
  }
  uint32_t vec[2] = { 0, 0 };
  size_t rd = f.read((uint8_t*)vec, sizeof(vec));
  f.close();
  if (rd != sizeof(vec)) {
    SETERR("Failed reading vector table from %s", path);
    return false;
  }
  uint32_t initialSp = vec[0];
  uint32_t resetVec  = vec[1];
  /* STM32F401CCU6/RCT6 SRAM: 0x20000000..0x20010000 (64 KB) */
  bool spOk = (initialSp > 0x20000000UL && initialSp <= 0x20010000UL && (initialSp & 0x3UL) == 0UL);
  /* Reset handler must be Thumb (bit 0 == 1) inside STM32 Flash */
  bool rvOk = ((resetVec & 1UL) == 1UL) &&
              (resetVec >= STM32_FLASH_BASE) &&
              (resetVec < (STM32_FLASH_BASE + STM32_FLASH_SIZE));
  if (!spOk || !rvOk) {
    SETERR("Invalid F401 image vector (SP=0x%08lX Reset=0x%08lX)",
           (unsigned long)initialSp, (unsigned long)resetVec);
    return false;
  }
  return true;
}

bool stm32FlashFile(const char* path, Stm32ProgressFn progress, char* err, size_t errLen) {
  File f = LittleFS.open(path, "r");
  if (!f) { SETERR("Firmware file %s not found", path); return false; }
  uint32_t size = f.size();
  f.close();
  if (size < 8) { SETERR("Firmware file too small (%lu B)", (unsigned long)size); return false; }
  if (size > STM32_FLASH_SIZE) {
    SETERR("Firmware %lu B exceeds flash %lu B", (unsigned long)size, (unsigned long)STM32_FLASH_SIZE);
    return false;
  }
  if (!validateStm32F401VectorTable(path, size, err, errLen)) {
    return false;
  }

  Stm32Info info;
  if (!stm32Connect(&info, err, errLen)) return false;
  if (info.flashSizeKB && size > info.flashSizeKB * 1024UL) {
    SETERR("Firmware %lu B exceeds target flash %lu KB", (unsigned long)size,
           (unsigned long)info.flashSizeKB);
    releaseTarget(true);
    return false;
  }

  uint32_t t0 = millis();
  bool ok = flashUnlock(err, errLen) &&
            flashEraseSectors(size, progress, err, errLen) &&
            flashProgram(path, size, progress, err, errLen);
  flashLock();
  if (ok) ok = flashVerify(path, size, progress, err, errLen);

  if (!ok) {
    Serial.printf("[FLASH] failed: %s\n", err ? err : "");
    releaseTarget(true);
    return false;
  }

  Serial.printf("[FLASH] %lu bytes written + verified in %lu ms, resetting target\n",
                (unsigned long)size, (unsigned long)(millis() - t0));
  if (progress) progress("restart", 0, 1);
  resetAndRun();
  if (progress) progress("restart", 1, 1);
  return true;
}

bool stm32Reset(char* err, size_t errLen) {
  swdInit();
  if (swdConnect() && swdPowerUp()) {
    memWrite32(DEMCR, 0);
    memWrite32(DHCSR, DHCSR_RUN);
    memWrite32(AIRCR, AIRCR_SYSRESET);
    delay(20);
    swdWriteDP(DP_CTRL_STAT, 0);
    swdRelease();
    Serial.println("[STM32] reset via SWD AIRCR.SYSRESETREQ");
    return true;
  }
  swdRelease();
#if STM32_NRST_PIN >= 0
  stm32HardReset();
  Serial.println("[STM32] SWD unavailable, reset via NRST pin");
  return true;
#else
  SETERR("SWD reset failed (%s) and NRST not wired", swdErrorStr(swdLastError()));
  return false;
#endif
}
