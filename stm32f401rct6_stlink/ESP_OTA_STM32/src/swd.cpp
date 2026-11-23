// Low-level SWD (ADIv5) bit-bang driver for ESP32.
//
// Timing (ADIv5 B4.2): host drives SWDIO while SWCLK is low and the target
// samples on the rising edge; the target drives on the rising edge and the
// host samples just before the next rising edge.

#include "swd.h"
#include "config.h"

#include <driver/gpio.h>
#include <soc/gpio_struct.h>

#if SWD_PIN_SWDIO >= 32 || SWD_PIN_SWCLK >= 32
#error "SWD pins must be in GPIO bank 0 (GPIO0..31)"
#endif

#define DIO_BIT (1UL << SWD_PIN_SWDIO)
#define CLK_BIT (1UL << SWD_PIN_SWCLK)

#define DIO_HIGH()  (GPIO.out_w1ts = DIO_BIT)
#define DIO_LOW()   (GPIO.out_w1tc = DIO_BIT)
#define CLK_HIGH()  (GPIO.out_w1ts = CLK_BIT)
#define CLK_LOW()   (GPIO.out_w1tc = CLK_BIT)
#define DIO_READ()  ((GPIO.in & DIO_BIT) != 0)
#define DIO_DRIVE() (GPIO.enable_w1ts = DIO_BIT)
#define DIO_FLOAT() (GPIO.enable_w1tc = DIO_BIT)

// Half-period padding (NOP loop). ~100 gives ~120-150 kHz SWCLK for maximum
// signal integrity on jumper wires.
#ifndef SWD_CLOCK_DELAY
#define SWD_CLOCK_DELAY 100
#endif

#define SWD_DP_TIMEOUT_MS 50
// AP accesses can sit in WAIT while the flash controller programs a word.
#define SWD_AP_TIMEOUT_MS 1000

static int s_lastError = 0;
static int s_lastAck = -1;

static inline void IRAM_ATTR swdDelay() {
  for (volatile int i = 0; i < SWD_CLOCK_DELAY; i++) {
    __asm__ __volatile__("nop");
  }
}

static inline void IRAM_ATTR clockPulse() {
  CLK_HIGH();
  swdDelay();
  CLK_LOW();
  swdDelay();
}

static inline void IRAM_ATTR writeBit(uint32_t bit) {
  if (bit) DIO_HIGH(); else DIO_LOW();
  swdDelay();
  clockPulse();
}

// Target drives one bit on the rising edge; sample while SWCLK is low (just
// before the next rising edge), matching ARM CMSIS-DAP / ADIv5 B4.2.2.
static inline uint32_t IRAM_ATTR readBit() {
  uint32_t bit = DIO_READ() ? 1 : 0;
  clockPulse();
  return bit;
}

static void writeBits(uint32_t value, int count) {
  for (int i = 0; i < count; i++) writeBit((value >> i) & 1);
}

static uint32_t readBits(int count) {
  uint32_t v = 0;
  for (int i = 0; i < count; i++) v |= readBit() << i;
  return v;
}

static void idleCycles(int n) {
  DIO_DRIVE();
  DIO_LOW();
  for (int i = 0; i < n; i++) clockPulse();
}

void swdInit() {
  gpio_reset_pin((gpio_num_t)SWD_PIN_SWCLK);
  gpio_reset_pin((gpio_num_t)SWD_PIN_SWDIO);
  pinMode(SWD_PIN_SWCLK, OUTPUT);
  pinMode(SWD_PIN_SWDIO, OUTPUT);
  gpio_set_direction((gpio_num_t)SWD_PIN_SWDIO, GPIO_MODE_INPUT_OUTPUT);
  // Internal pull-up keeps SWDIO defined during turnaround; lowest drive reduces ringing.
  gpio_pullup_en((gpio_num_t)SWD_PIN_SWDIO);
  gpio_set_drive_capability((gpio_num_t)SWD_PIN_SWCLK, GPIO_DRIVE_CAP_0);
  gpio_set_drive_capability((gpio_num_t)SWD_PIN_SWDIO, GPIO_DRIVE_CAP_0);
  CLK_LOW();
  DIO_LOW();
  DIO_DRIVE();
  s_lastError = 0;
}

void swdRelease() {
  pinMode(SWD_PIN_SWCLK, INPUT);
  pinMode(SWD_PIN_SWDIO, INPUT_PULLUP);
}

static void lineResetOnly() {
  DIO_DRIVE();
  DIO_HIGH();
  for (int i = 0; i < 60; i++) clockPulse();
}

void swdLineReset() {
  lineResetOnly();
  idleCycles(4);
}

void swdJtagToSwd() {
  DIO_DRIVE();
  writeBits(0xE79E, 16);
}

static uint8_t buildRequest(bool ap, bool rd, uint8_t addr) {
  uint8_t req = 0x81;                    // start + park
  if (ap) req |= 0x02;
  if (rd) req |= 0x04;
  if (addr & 0x04) req |= 0x08;          // A2
  if (addr & 0x08) req |= 0x10;          // A3
  if (__builtin_popcount(req & 0x1E) & 1) req |= 0x20;
  return req;
}

// One raw transaction, no retry. Returns the ACK or a negative error.
static int rawTransfer(uint8_t req, uint32_t* data) {
  const bool rd = req & 0x04;

  DIO_DRIVE();
  writeBits(req, 8);

  DIO_FLOAT();
  clockPulse();                          // turnaround host -> target
  int ack = (int)readBits(3);
  s_lastAck = ack;

  if (ack == SWD_ACK_OK) {
    if (rd) {
      uint32_t v = readBits(32);
      uint32_t par = readBit();
      clockPulse();                      // turnaround target -> host
      idleCycles(2);
      if ((uint32_t)(__builtin_popcount(v) & 1) != par) return SWD_ERR_PARITY;
      *data = v;
    } else {
      clockPulse();                      // turnaround target -> host
      DIO_DRIVE();
      writeBits(*data, 32);
      writeBit(__builtin_popcount(*data) & 1);
      idleCycles(2);
    }
    return SWD_ACK_OK;
  }

  if (ack == SWD_ACK_WAIT || ack == SWD_ACK_FAULT) {
    clockPulse();                        // turnaround
    idleCycles(1);
    return ack;
  }

  // No / garbage ACK: let a possible data phase run out, then go idle.
  for (int i = 0; i < 34; i++) clockPulse();
  idleCycles(2);
  return SWD_ERR_NOACK;
}

void swdClearErrors() {
  uint32_t v = ABORT_CLEAR;
  rawTransfer(buildRequest(false, false, DP_ABORT), &v);
}

static int swdTransfer(bool ap, bool rd, uint8_t addr, uint32_t* data, uint32_t timeoutMs) {
  const uint8_t req = buildRequest(ap, rd, addr);
  const uint32_t start = millis();
  uint32_t spins = 0;

  for (;;) {
    int r = rawTransfer(req, data);
    if (r == SWD_ACK_OK) {
      s_lastError = 0;
      return r;
    }
    if (r == SWD_ACK_WAIT) {
      if (millis() - start > timeoutMs) {
        swdClearErrors();                // DAPABORT-free: just clear stickies
        s_lastError = SWD_ERR_TIMEOUT;
        return s_lastError;
      }
      if ((++spins & 0x3FF) == 0) vTaskDelay(1);
      continue;
    }
    if (r == SWD_ACK_FAULT) swdClearErrors();
    s_lastError = r;
    return r;
  }
}

bool swdReadDP(uint8_t addr, uint32_t* out) {
  return swdTransfer(false, true, addr, out, SWD_DP_TIMEOUT_MS) == SWD_ACK_OK;
}

bool swdWriteDP(uint8_t addr, uint32_t val) {
  return swdTransfer(false, false, addr, &val, SWD_DP_TIMEOUT_MS) == SWD_ACK_OK;
}

// AP reads are posted: the result arrives with the next AP read or RDBUFF.
bool swdReadAP(uint8_t addr, uint32_t* out) {
  uint32_t dummy = 0;
  if (swdTransfer(true, true, addr, &dummy, SWD_AP_TIMEOUT_MS) != SWD_ACK_OK) return false;
  return swdReadDP(DP_RDBUFF, out);
}

bool swdWriteAP(uint8_t addr, uint32_t val) {
  return swdTransfer(true, false, addr, &val, SWD_AP_TIMEOUT_MS) == SWD_ACK_OK;
}

uint32_t swdConnect() {
  uint32_t lastId = 0;
  for (int attempt = 0; attempt < 4; attempt++) {
    // ADIv5 B5.2.2: >=50 clocks HIGH immediately followed by 0xE79E (no 0 gap).
    lineResetOnly();
    swdJtagToSwd();
    swdLineReset();
    uint32_t id = 0;
    if (swdReadDP(DP_IDCODE, &id) && id != 0 && id != 0xFFFFFFFFUL) {
      swdClearErrors();
      swdWriteDP(DP_SELECT, 0);          // AP 0, bank 0
      return id;
    }
    lastId = id;
    delay(5);
  }
  DIO_DRIVE();
  DIO_LOW();
  delayMicroseconds(5);
  int drvLow = DIO_READ() ? 1 : 0;
  DIO_FLOAT();
  delayMicroseconds(5);
  int idleLvl = DIO_READ() ? 1 : 0;

  // Test A: 48 clocks with NO request packet (should stay all 1s if SWD-DP).
  lineResetOnly();
  swdJtagToSwd();
  swdLineReset();
  DIO_FLOAT();
  uint32_t noReqLo = readBits(32);

  // Test B: Capture 48 raw bits right after sending DP_IDCODE read (0xA5) + DIO_FLOAT().
  lineResetOnly();
  swdJtagToSwd();
  swdLineReset();
  DIO_DRIVE();
  writeBits(buildRequest(false, true, DP_IDCODE), 8);
  DIO_FLOAT();
  uint32_t rawLo = readBits(32);
  uint32_t rawHi = readBits(16);

  static int lastLoggedAck = -2;
  static uint32_t lastLoggedLo = 0xFFFFFFFFUL;
  if (s_lastAck != lastLoggedAck || rawLo != lastLoggedLo) {
    lastLoggedAck = s_lastAck;
    lastLoggedLo = rawLo;
    Serial.printf("[SWD] no target: dio_low=%d dio_idle=%d ack=%d noreq=%08lX raw=%04lX%08lX id=0x%08lX err=%d\n",
                  drvLow, idleLvl, s_lastAck, (unsigned long)noReqLo,
                  (unsigned long)rawHi, (unsigned long)rawLo,
                  (unsigned long)lastId, s_lastError);
  }
  if (s_lastError == 0) s_lastError = SWD_ERR_NOACK;
  return 0;
}

bool swdPowerUp() {
  if (!swdWriteDP(DP_CTRL_STAT, 0x50000000UL)) return false;   // CSYS|CDBG PWRUPREQ
  const uint32_t start = millis();
  bool up = false;
  while (millis() - start < 200) {
    uint32_t st = 0;
    if (swdReadDP(DP_CTRL_STAT, &st) && (st & 0xA0000000UL) == 0xA0000000UL) {
      up = true;
      break;
    }
    delay(1);
  }
  if (!up) return false;
  return swdWriteAP(AP_CSW, CSW_32BIT_INC);
}

bool memRead32(uint32_t addr, uint32_t* out) {
  return swdWriteAP(AP_TAR, addr) && swdReadAP(AP_DRW, out);
}

bool memWrite32(uint32_t addr, uint32_t val) {
  if (!swdWriteAP(AP_TAR, addr) || !swdWriteAP(AP_DRW, val)) return false;
  uint32_t dummy;
  return swdReadDP(DP_RDBUFF, &dummy);   // flush posted write, catch errors
}

// Words that fit before the next 1 KB TAR auto-increment boundary.
static size_t wordsToBoundary(uint32_t addr, size_t count) {
  size_t room = (0x400 - (addr & 0x3FF)) / 4;
  return count < room ? count : room;
}

bool memWriteBlock(uint32_t addr, const uint32_t* words, size_t count) {
  while (count) {
    size_t n = wordsToBoundary(addr, count);
    if (!swdWriteAP(AP_TAR, addr)) return false;
    for (size_t i = 0; i < n; i++) {
      if (!swdWriteAP(AP_DRW, words[i])) return false;
    }
    uint32_t dummy;
    if (!swdReadDP(DP_RDBUFF, &dummy)) return false;
    addr += n * 4;
    words += n;
    count -= n;
  }
  return true;
}

bool memReadBlock(uint32_t addr, uint32_t* words, size_t count) {
  while (count) {
    size_t n = wordsToBoundary(addr, count);
    if (!swdWriteAP(AP_TAR, addr)) return false;
    uint32_t v = 0;
    if (swdTransfer(true, true, AP_DRW, &v, SWD_AP_TIMEOUT_MS) != SWD_ACK_OK) return false;
    for (size_t i = 1; i < n; i++) {
      if (swdTransfer(true, true, AP_DRW, &words[i - 1], SWD_AP_TIMEOUT_MS) != SWD_ACK_OK) return false;
    }
    if (!swdReadDP(DP_RDBUFF, &words[n - 1])) return false;
    addr += n * 4;
    words += n;
    count -= n;
  }
  return true;
}

int swdLastError() { return s_lastError; }

const char* swdErrorStr(int err) {
  switch (err) {
    case 0:               return "OK";
    case SWD_ACK_WAIT:    return "WAIT";
    case SWD_ACK_FAULT:   return "FAULT";
    case SWD_ERR_PARITY:  return "parity error";
    case SWD_ERR_NOACK:   return "no ACK (target not connected?)";
    case SWD_ERR_TIMEOUT: return "WAIT timeout";
    default:              return "unknown";
  }
}
