
/*********************** Remote loggging ***********************/
/*
 * Log mode selection in user interface: 
 * false : log to serial / web monitor only
 * true  : also saves log on SD card. To download the log generated, either:
 *  - To view the log, press Show Log button on the browser
 * - To clear the log file contents, on log web page press Clear Log link
 */
 
#include "appGlobals.h"
#include "freertos/atomic.h"

bool dbgVerbose = false;

#define LOG_BUF_COUNT 4  // number of pool buffers —  number of concurrent calling tasks
#define LOG_QUEUE_DEPTH LOG_BUF_COUNT
#define HWM_MIN 32 // less than these bytes with debug exception probably indicates stack overflow
#define HWM_MAX 128 // more than these bytes with debug exception probably indicates printf formatting causing break
#define WRITE_CACHE_CYCLE 5

// Pool: fixed array of buffers 
static char (*poolBufs)[MAX_OUT] = NULL;
// Queue storage sized correctly as raw bytes (sizeof(char*) per slot)
static uint8_t freePoolStorage[LOG_BUF_COUNT * sizeof(char*)];
static uint8_t logQueueStorage[LOG_QUEUE_DEPTH * sizeof(char*)];
static StaticQueue_t freePoolStatic;
static StaticQueue_t logQueueStatic;
QueueHandle_t logFreePool = NULL;
QueueHandle_t logQueue = NULL;
TaskHandle_t logHandle = NULL;
static uint32_t dropCount = 0; // Atomic drop counter — safe to increment from any task

bool useLogColors = false;  // true to colorise log messages (eg if using idf.py, but not arduino)
bool sdLog = false; // log to SD
int logType = 0; // which log contents to display (0 : ram, 1 : sd)
static FILE* log_remote_fp = NULL;
static uint32_t counter_write = 0;
// allow any startup failures to be reported via browser for remote devices
char startupFailure[SF_LEN] = {0};

// RAM memory based logging in RTC slow memory (cannot init)
RTC_NOINIT_ATTR char messageLog[RAM_LOG_LEN];
RTC_NOINIT_ATTR uint16_t mlogEnd;
static RTC_NOINIT_ATTR char brownoutStatus;
// Deliberately NOT in RTC memory: this is "the boot we are on right now began with a
// brownout", which is a property of one boot and must not survive into the next. The RTC
// flag alone could not express that - it goes to 'R' on the boot after the brownout and
// stayed there until a power cycle, so hadBrownout() answered yes forever and every
// subsequent boot served the 60s recording holdoff for a supply failure long past.
// initBrownout() sets this once, from the RTC flag and the reset reason, and it clears
// itself by being reinitialised on the next boot
static bool brownoutThisBoot = false;
// Survives the reset so the next boot can say whether the graceful sag response actually
// ran before the supply went. Without it, a hardware brownout reset and a Stage 1 landing
// followed by a hardware reset are indistinguishable after the fact - which is exactly
// the ambiguity that made the first battery run uninterpretable
static RTC_NOINIT_ATTR uint32_t sagStage1Count;
// RTC noinit memory holds whatever was there before, and only a power on cycle clears it,
// so a newly added counter reads as garbage until then - it needs its own validity marker
// rather than trusting the value. Same reason crashLoop carries MAGIC_NUM
#define SAG_MAGIC 0x5A61C0DE
static RTC_NOINIT_ATTR uint32_t sagMagic;
// Restart-stage breadcrumb. doRestart() has hung twice in ~12 OTA restarts after writing its
// first log line, and never on the 100-cycle web-reset harness, and the SD log cannot say
// where: nothing is ever written between that line and the next boot, even on success.
// Each step stamps its number here and the next boot reports the last one reached. A clean
// restart reports RESTART_IN_ESP_RESTART, which is also the proof the instrument works.
// Own magic, for the same reason as sagMagic
#define RESTART_MAGIC 0x5E57A6E5
static RTC_NOINIT_ATTR uint32_t restartMagic;
static RTC_NOINIT_ATTR uint8_t restartStage;
static uint8_t restartReport = 0; // this boot only: what printResetReason found, for prepRecording to re-log

void markRestartStage(restartStage_t stage) {
  restartStage = stage;
  restartMagic = RESTART_MAGIC;
}

uint8_t previousRestartStage() {
  return restartReport;
}

const char* restartStageName(uint8_t stage) {
  static const char* names[] = {"none", "entered", "brownout disarmed", "appShutdown done",
    "mqtt stopped", "crash loop reset", "log flushed and closed", "2s delay done",
    "calling esp_restart", "inside esp_restart"};
  return stage < sizeof(names) / sizeof(names[0]) ? names[stage] : "?";
}
static RTC_NOINIT_ATTR uint32_t crashLoop;
static bool crashLoopSuspected = false; // this boot only; reported once logging is up
static RTC_NOINIT_ATTR uint32_t backtrace[60]; // array of backtrace addresses 
static RTC_NOINIT_ATTR size_t btLen; // number of backtrace entries
static RTC_NOINIT_ATTR char btReason[64]; // reason for panic
static RTC_NOINIT_ATTR char btTask[20]; // task name
static RTC_NOINIT_ATTR int btCore; // cpu core id
static RTC_NOINIT_ATTR uint32_t btHWM; // high water mark bytes left
static RTC_NOINIT_ATTR uint32_t haveTrace; // boolean


void resetCrashLoop() {
  crashLoop = 0;
}

void logIncrementDropCount(void) {
  Atomic_Increment_u32(&dropCount);
}

void saveRamLog(const char* ramLogName) {
  // save ramlog to storage 
  File ramFile = STORAGE.open(ramLogName, FILE_WRITE);
  int startPtr, endPtr;
  startPtr = endPtr = mlogEnd;  
  // write log in chunks
  do {
    int maxChunk = startPtr < endPtr ? endPtr - startPtr : RAM_LOG_LEN - startPtr;
    size_t chunkSize = std::min(CHUNKSIZE, maxChunk);    
    if (chunkSize > 0) ramFile.write((uint8_t*)messageLog + startPtr, chunkSize);
    startPtr += chunkSize;
    if (startPtr >= RAM_LOG_LEN) startPtr = 0;
  } while (startPtr != endPtr);
  ramFile.close();
}

static void ramLogClear() {
  mlogEnd = 0;
  memset(messageLog, 0, RAM_LOG_LEN);
}
  
static void ramLogStore(const char* outBuf, size_t msgLen) {
  // save log entry in ram buffer
  if (mlogEnd + msgLen >= RAM_LOG_LEN) {
    // log needs to roll around cyclic buffer
    uint16_t firstPart = RAM_LOG_LEN - mlogEnd;
    memcpy(messageLog + mlogEnd, outBuf, firstPart);
    msgLen -= firstPart;
    memcpy(messageLog, outBuf + firstPart, msgLen);
    mlogEnd = 0;
  } else memcpy(messageLog + mlogEnd, outBuf, msgLen);
  mlogEnd += msgLen;
}

void markSagStage1() {
  if (sagMagic != SAG_MAGIC) { // first use, or contents lost to a power off
    sagMagic = SAG_MAGIC;
    sagStage1Count = 0;
  }
  sagStage1Count++;
}

void logSyncSD() {
  // Push the SD log to the card now, without flush_log()'s one second delay - meant for
  // the moments where the next thing that happens may be the supply disappearing.
  // Drain the queue first: the caller's lines are normally still in logQueue waiting for
  // logTask, and an fsync before they reach the file persists nothing - the 30 Aug
  // battery run parked flawlessly but left no SUPPLY SAG line on the card because of
  // exactly this. Bounded, so a wedged logTask cannot hold the sag response hostage
  uint32_t syncStart = millis();
  while (logQueue != NULL && uxQueueMessagesWaiting(logQueue) > 0 && millis() - syncStart < 250) delay(10);
  delay(30); // the last message leaves the queue before it is written - let it land
  if (log_remote_fp != NULL) {
    fflush(log_remote_fp);
    fsync(fileno(log_remote_fp));
  }
}

void flush_log(bool andClose) {
  if (log_remote_fp != NULL) {
    // fflush BEFORE fsync: fwrite buffers in stdio, and fsync only pushes what has
    // already reached the fd - the old order synced everything except the newest lines
    fflush(log_remote_fp);
    fsync(fileno(log_remote_fp));
    if (andClose) {
      LOG_INF("Closed SD file for logging");
      fclose(log_remote_fp);
      log_remote_fp = NULL;
    } else delay(1000);
  }  
}

static void remote_log_init_SD() {
#if !CONFIG_IDF_TARGET_ESP32C3
  STORAGE.mkdir(DATA_DIR);
  // Open remote file
  log_remote_fp = NULL;
  log_remote_fp = fopen("/sdcard" LOG_FILE_PATH, "a");
  if (log_remote_fp == NULL) {LOG_WRN("Failed to open SD log file %s", LOG_FILE_PATH);}
  else {
    logLine();
    LOG_INF("Opened SD file for logging");
  }
#endif
}

void reset_log() {
  if (logType == 0) ramLogClear();
  if (logType == 1) {
    if (log_remote_fp != NULL) flush_log(true); // Close log file
    STORAGE.remove(LOG_FILE_PATH);
    remote_log_init_SD();
  }
  LOG_INF("Cleared %s log file", logType == 0 ? "RAM" : "SD"); 
}

static void saveCrashRamLog() {
  // The previous boot ended without a controlled restart (crash, watchdog, brownout, a hang
  // cleared by the reset button) or in a controlled restart that never completed. The RTC
  // ring still holds the pre-crash tail at this point - printResetReason() has added the
  // reason and any backtrace, and new boot lines overwrite the OLDEST bytes first, with only
  // a few written so far - so copy it out to a file now. Left in the ring it is gone within
  // the boot: one boot's chatter is ~6KB of a 7KB ring, and the reason for two of the 2 Sep
  // reboots scrolled away exactly this way before anyone read it. Once per boot
  static bool done = false;
  if (done || esp_reset_reason() == ESP_RST_POWERON) return; // RTC contents undefined after power on
  done = true;
  uint8_t st = restartReport ? restartReport : (restartMagic == RESTART_MAGIC ? restartStage : 0);
  bool hungRestart = st && st != RESTART_IN_ESP_RESTART;
  // The reset reason is the signal, not crashLoopSuspected: that flag is cleared by the first
  // successful ping, so a crash after a healthy boot looks controlled to it (the crashTest
  // dirty reboot proved it). Anything but a software restart, a power on or a deep sleep
  // wake means the previous boot did not end on its own terms - panic, any watchdog,
  // brownout, or the reset button (USB/EXT) after a hang
  esp_reset_reason_t why = esp_reset_reason();
  bool controlled = (why == ESP_RST_SW || why == ESP_RST_POWERON || why == ESP_RST_DEEPSLEEP);
  if (controlled && !hungRestart && !crashLoopSuspected) return;
  char crashName[FILE_NAME_LEN];
  time_t now = getEpoch();
  strftime(crashName, sizeof(crashName), DATA_DIR "/crash_%Y%m%d_%H%M%S" TEXT_EXT, localtime(&now));
  saveRamLog(crashName);
  LOG_WRN("Previous boot ended %s (reset reason %d) - its RAM log is saved as %s",
    hungRestart ? "in a restart that never completed" : "without a controlled restart", (int)why, crashName);
}

void remote_log_init() {
  // setup required log mode
  if (sdLog) {
    flush_log(false);
    remote_log_init_SD(); // store log on sd card
    saveCrashRamLog();
  } else flush_log(true);
}

static void logTask(void *pvParams) {
  // separate task to reduce stack size in other tasks
  char *msg;
  while (true) {
    if (xQueueReceive(logQueue, &msg, portMAX_DELAY) == pdTRUE) {
      // logTask is sole consumer so safe without locking
      msg[MAX_OUT - 2] = '\n'; 
      msg[MAX_OUT - 1] = 0; // ensure always have ending newline
      // output message to various recipients
      size_t msgLen = strlen(msg);
      // LOG_DIA marks a line as not for the RTC ring; strip the mark before anything sees it
      bool noRam = msgLen > 1 && msg[msgLen - 2] == LOG_DIA_MARK;
      if (noRam) {
        msg[msgLen - 2] = '\n';
        msg[--msgLen] = 0;
      }
      if (msgLen > 1) {
        wsAsyncSendText(msg); // output to browser over web socket
        if (msg[msgLen - 2] == '~') msg[msgLen - 2] = ' '; // remove '~' if present
      }
      if (monitorOpen) {
        // output to monitor console if attached
        fputs(msg, stdout);
        fflush(stdout);
      } else delay(10); // allow time for other tasks
      if (msgLen > 1) {
        if (!noRam) ramLogStore(msg, msgLen); // store in rtc ram 
        if (sdLog) {
          if (log_remote_fp != NULL) {
            // output to SD if file opened
            fwrite(msg, sizeof(char), msgLen, log_remote_fp); // log.txt
            // Periodic sync to SD, but never for a warning or an error: those are the
            // lines that explain a failure, and on a battery board the failure is a
            // supply loss that discards everything not yet synced. The whole of a 85
            // minute battery run was lost this way (28 Aug) - the SD log is only a black
            // box if the interesting lines are on the card before the lights go out
            if (counter_write++ % WRITE_CACHE_CYCLE == 0
                || strstr(msg, "WARN") != NULL || strstr(msg, "ERR") != NULL) {
              // fflush first or the fsync pushes everything EXCEPT this line - fwrite
              // leaves it in the stdio buffer, which defeated the whole point of syncing
              // on warnings (they persisted only when a later line happened to flush them)
              fflush(log_remote_fp);
              fsync(fileno(log_remote_fp));
            }
          } 
        }
      }
      // return buffer to pool — use portMAX_DELAY as pool should
      // always have space (only send as many messages as pool slots)
      xQueueSend(logFreePool, &msg, portMAX_DELAY);
    }
  }
}

void logLine() {
  LOG_SEND(" \n");
}

int vprintfRedirect(const char* format, va_list args) {
  // format esp_log() output for LOG_SEND()
  char buffer[256];
  int len = vsnprintf(buffer, sizeof(buffer), format, args);
  LOG_SEND("%s", buffer);
  return len;
}

void formatHex(const char* inData, size_t inLen) {
  // format data as hex bytes for output
  char formatted[(inLen * 3) + 1];
  for (int i=0; i<inLen; i++) sprintf(formatted + (i*3), "%02x ", inData[i]);
  formatted[(inLen * 3)] = 0; // terminator
  LOG_INF("Hex: %s", formatted);
}

const char* espErrMsg(esp_err_t errCode) {
  // convert esp error code to text
  // https://github.com/espressif/esp-idf/blob/master/components/esp_common/include/esp_err.h
  static char errText[100];
  esp_err_to_name_r(errCode, errText, 100);
  return errText;
}

static void appPanicHandler(arduino_panic_info_t *info, void *arg) {
  // store crash backtrace and delay reboot to avoid thrashing
  // https://github.com/espressif/arduino-esp32/blob/master/cores/esp32/esp32-hal-misc.c
    TaskHandle_t task =  xTaskGetCurrentTaskHandleForCore(info->core);
    btHWM = uxTaskGetStackHighWaterMark(task);
    const char* taskName = task ? pcTaskGetName(task) : "idle";
    strncpy(btTask, taskName, sizeof(btTask) - 1);
    strncpy(btReason, info->reason, sizeof(btReason) - 1);
    btCore = info->core;
  btLen = info->backtrace_len;
  for (int i = 0; i < info->backtrace_len; i++) backtrace[i] = info->backtrace[i];
  haveTrace = MAGIC_NUM; // flag that backtrace available
  esp_rom_delay_us(PANIC_DELAY * 1000 * 1000);
}

static void expandReason() {
  if (!strlen(btReason)) strcpy(btReason, "unknown");
  // Xtensa
  else if (strstr(btReason, "Unhandled debug exception") != NULL) {
    if (btHWM < HWM_MIN) sprintf(btReason, "probably stack overflow @ HWM: %lu bytes", btHWM);
    else if (btHWM > HWM_MAX) sprintf(btReason, "probably printf format"); // usually misplaced or misformatted vsnprintf()
    else sprintf(btReason, "stack overflow / printf format. HWM: %lu bytes", btHWM); 
  }
  else if (!strcmp(btReason, "LoadProhibited") || !strcmp(btReason, "StoreProhibited") || !strcmp(btReason, "InstructionFetchError")) strcat(btReason, " (pointer issue)");
}

static void showBacktrace() {
  // display reason and backtrace following restart after a panic
  if (haveTrace == MAGIC_NUM) {
    haveTrace = 0;
    expandReason();
    LOG_WRN("Core %d task: %s - %s", btCore, btTask, btReason); 
    char bt[(11 * btLen) + 1]; // 11 is size of each trace hex
    for (int i = 0; i < btLen; i++) 
      snprintf(bt + strlen(bt), sizeof(bt) - strlen(bt) - 11, "0x%08x ", (unsigned int)backtrace[i]); 
    LOG_WRN("Paste backtrace below into Arduino Exception Decoder:\n");
    LOG_SEND("Backtrace: %s\n\n", bt);
  }
}

static esp_sleep_wakeup_cause_t printWakeupReason() {
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  switch(wakeup_reason) {
    case ESP_SLEEP_WAKEUP_EXT0 : LOG_INF("Wakeup by external signal using RTC_IO"); break;
    case ESP_SLEEP_WAKEUP_EXT1 : LOG_INF("Wakeup by external signal using RTC_CNTL"); break;
    case ESP_SLEEP_WAKEUP_TIMER : LOG_INF("Wakeup by internal timer"); break;
    case ESP_SLEEP_WAKEUP_TOUCHPAD : LOG_INF("Wakeup by touchpad"); break;
    case ESP_SLEEP_WAKEUP_ULP : LOG_INF("Wakeup by ULP program"); break;
    case ESP_SLEEP_WAKEUP_GPIO: LOG_INF("Wakeup by GPIO"); break;    
    case ESP_SLEEP_WAKEUP_UART: LOG_INF("Wakeup by UART"); break; 
    default : LOG_INF("Wakeup by reset"); break;
  }
  return wakeup_reason;
}

static esp_reset_reason_t printResetReason() {
  esp_reset_reason_t bootReason = esp_reset_reason();
  switch (bootReason) {
    case ESP_RST_UNKNOWN: LOG_INF("Reset for unknown reason"); break;
    case ESP_RST_POWERON: {
      LOG_INF("Power on reset");
      brownoutStatus = 0;
      sagMagic = 0; // RTC contents are undefined after a true power off
      sagStage1Count = 0;
      restartMagic = 0;
      messageLog[0] = 0;
      break;
    }
    case ESP_RST_EXT: LOG_INF("Reset from external pin"); break;
    case ESP_RST_SW: LOG_INF("Software reset via esp_restart"); break;
    case ESP_RST_PANIC: LOG_INF("Software reset due to exception/panic"); break;
    case ESP_RST_INT_WDT: LOG_INF("Reset due to interrupt watchdog"); break;
    case ESP_RST_TASK_WDT: LOG_INF("Reset due to task watchdog"); break;
    case ESP_RST_WDT: LOG_INF("Reset due to other watchdogs"); break;
    case ESP_RST_DEEPSLEEP: LOG_INF("Reset after exiting deep sleep mode"); break;
    case ESP_RST_BROWNOUT: LOG_INF("Software reset due to brownout"); break;
    case ESP_RST_SDIO: LOG_INF("Reset over SDIO"); break;
    default: LOG_WRN("Unhandled reset reason"); break;
  }
  // Logging is alive by now, unlike in logSetup(), so this is where the sag history is
  // safe to report. Bounded because RTC memory is undefined after a true power off
  if (bootReason == ESP_RST_BROWNOUT) {
    if (sagMagic == SAG_MAGIC && sagStage1Count > 0)
      LOG_ALT("Supply sag response had run %lu time(s) before this brownout - the clip was landed", sagStage1Count);
    else LOG_WRN("Brownout with no sag response recorded - the rail fell faster than the warning level could catch");
  }
  if (crashLoopSuspected) LOG_WRN("Previous boot ended without a controlled restart%s - check the log",
    (bootReason == ESP_RST_BROWNOUT) ? " (brownout)" : "");
  if (restartMagic == RESTART_MAGIC) {
    // a controlled restart began on the previous boot - did it get all the way through?
    // This runs before the SD log opens, so it reaches only the RTC ring and serial;
    // prepRecording() logs it again once the SD log is up (previousRestartStage)
    if (restartStage == RESTART_IN_ESP_RESTART) LOG_INF("Previous controlled restart completed every stage");
    else LOG_WRN("Previous controlled restart HUNG after stage %u (%s) - see doRestart()", restartStage, restartStageName(restartStage));
    restartReport = restartStage;
    restartMagic = 0; // consumed
  }
  showBacktrace();
  return bootReason;
}

esp_sleep_wakeup_cause_t wakeupResetReason() {
  printResetReason();
  esp_sleep_wakeup_cause_t wakeupReason = printWakeupReason();
  return wakeupReason;
}

// catch software resets due to brownouts
//https://github.com/espressif/esp-idf/blob/master/components/esp_system/port/brownout.c

#include "esp_private/system_internal.h"
#include "esp_private/rtc_ctrl.h"
#include "hal/brownout_ll.h"

#include "soc/rtc_periph.h"
#include "hal/brownout_hal.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/regi2c_brownout.h"
#include "esp_private/regi2c_ctrl.h"

// The brownout comparator is the only supply sensor this board has - there is no battery
// divider wired - so it is used as a staged sag detector rather than only as a killswitch.
// S3 trip points from the IDF Kconfig: lvl 1 3.30V, 2 3.19V, 3 2.98V, 4 2.84V, 5 2.67V,
// 6 2.56V, 7 2.44V.
// Level 7 is far below the SD card's 2.7V minimum and is only good as the terminal
// backstop, which is what this used to be armed at permanently - by the time it fires
// there is no writing left to do. The warning stage needs the opposite end of the range;
// which end exactly is the subject of the note below
// The threshold has to clear the BATTERY's protection cutoff, not just the chip and card
// minimums. The XIAO's regulator is an SGM6029 synchronous buck, not an LDO: below ~3.3V
// in it goes to 100% duty cycle with the high-side switch held on ("even when the input
// voltage falls below the output"), so the rail becomes a pass-through and tracks the cell
// down, less only I x RDSON (185mOhm typ, ~30mV here). The converter itself runs to a
// 1.76V UVLO, so it never gives up first. That means a level 3 trip at ~2.98V corresponds
// to a cell at ~3.01V - right where a protected LiPo's disconnect fires, and in two
// battery runs the protection won the race every time and no warning was ever produced
// (28 Aug: "no sag response recorded", rail band 0 to the last poll before reset).
// Level 2 trips at 3.19V, so the cell is still ~3.22V when the sag response starts -
// depleted but well above a protected pack's disconnect, and far above the SD card's 2.7V
// floor. It survived the same zero-false-trip qualification level 3 did.
// The camera supply says the same thing independently. The OV5640 runs from an SGM2036-2.8
// LDO fed off this rail, and its dropout is 215mV typ / 280mV max at 300mA, so the 2.8V
// analog rail leaves regulation once this rail reaches ~3.02-3.08V and then simply follows
// it down (that part keeps running to a 1.6V input). Tripping at 2.98V would therefore
// have landed the clip AFTER the sensor's analog supply had already sagged - the last
// frames written would have been captured on a failing rail, with no way to tell from the
// file. 3.19V lands it while the camera is still properly supplied
// Level 1 (3.30V) is exactly the nominal rail and cannot be used at all; level 2 is the
// only usable warning point on this board. Its margin is genuinely tight - 110mV nominal,
// and only ~44mV against the buck's +-2% regulation worst case (3.234V) - so the soak is
// what justifies it, not the arithmetic. Re-run that soak before trusting any change here
#define BROWNOUT_WARN_LVL 2 // 3.19V - above the pack's cutoff, well above SD's 2.7V min
#define BROWNOUT_DET_LVL 7  // ~2.44V - terminal backstop, dirty reboot

volatile bool supplySagging = false;   // set by the ISR, actioned in task context
volatile uint32_t sagTripCount = 0;    // total trips since boot (soak instrumentation)
static volatile bool brownoutTerminal = false; // true once armed as the killswitch
static uint8_t armedLevel = BROWNOUT_WARN_LVL;
static bool isrRegistered = false;

IRAM_ATTR static void notifyBrownout(void *arg) {
  // The trip condition is level based, so the interrupt must be silenced immediately or it
  // re-enters for as long as the rail stays low. Warning stages therefore only record the
  // event and hand off to task context, where there is a stack, flash access and time to
  // close the recording - the terminal stage keeps the original dirty reboot
  brownout_ll_intr_enable(false);
  brownout_ll_intr_clear();
  sagTripCount++;
  supplySagging = true;
  if (brownoutTerminal) {
    esp_cpu_stall(!xPortGetCoreID());  // Stop the other core.
    esp_reset_reason_set_hint(ESP_RST_BROWNOUT);
    brownoutStatus = 'B';
    esp_restart_noos(); // dirty reboot
  }
}

void disarmBrownout() {
  // Quiesce the comparator entirely - no interrupt, no reset, no flash or RF power-down,
  // not even enabled. Called at the top of doRestart(): the restart teardown (RF
  // shutdown, cache and clock transitions) otherwise runs with flash_power_down latched
  // on a live comparator, and a transient trip there powers flash down mid-execution -
  // the chip stops dead, no watchdog, no USB, physical reset only. Reproduced on demand
  // 31 Aug (harness cycle 2: silence straight after the closeAvi stats, esptool Write
  // timeout); three field occurrences before that, all on doRestart teardowns. The next
  // boot is NOT this function's concern: IDF's esp_brownout_init re-arms the comparator
  // with its stock config early in every boot (the forensic dump in logSetup measures
  // that, not reset survival), and initBrownout() then takes over
  brownout_ll_intr_enable(false);
  brownout_ll_intr_clear();
  brownout_hal_config_t off = {
    .threshold = armedLevel,
    .enabled = false,
    .reset_enabled = false,
    .flash_power_down = false,
    .rf_power_down = false,
  };
  brownout_hal_config(&off);
  brownoutTerminal = false;
}

void armBrownout(uint8_t level, bool terminal) {
  // (re)arm the comparator at a given level. flash_power_down / rf_power_down MUST stay
  // false for a warning stage: they tell the hardware to drop flash and the radio on trip,
  // which would strand the code that is supposed to close the file (it executes from
  // flash) and kill wifi, the only interface a battery powered board has.
  // Quiesce first: reconfiguring a LIVE comparator with action bits latched is the
  // lockup window this file's forensic trace was built to catch - disable it, let the
  // analog side settle, then configure and enable with the final bits in one step
  brownout_ll_intr_enable(false);
  brownout_ll_intr_clear();
  brownout_hal_config_t off = {
    .threshold = level,
    .enabled = false,
    .reset_enabled = false,
    .flash_power_down = false,
    .rf_power_down = false,
  };
  brownout_hal_config(&off);
  delayMicroseconds(200); // same settle idiom as brownoutProbeBand()
  brownoutTerminal = terminal;
  armedLevel = level;
  brownout_hal_config_t cfg = {
    .threshold = level,
    .enabled = true,
    .reset_enabled = terminal, // let hardware reset if software is past saving itself
    .flash_power_down = terminal,
    .rf_power_down = terminal,
  };
  // Forensic trace: hal_config enables the comparator with the action bits latched while
  // the threshold (an analog REGI2C setting) is still settling - the suspected lockup
  // window. Only ever prints at boot and on a manual bodLevel change: brownoutProbeBand
  // refuses to run once the terminal stage is armed, so there is no periodic spam
  printf("armBrownout: level %u terminal %d, hal_config...\n", level, terminal ? 1 : 0);
  fflush(stdout); delay(10);
  brownout_hal_config(&cfg);
  delayMicroseconds(200); // let the comparator settle on the new config
  printf("armBrownout: hal_config done\n"); fflush(stdout);
  brownout_ll_intr_clear(); // discard any glitch raised by the reconfiguration itself
  if (!isrRegistered) {
    rtc_isr_register(notifyBrownout, NULL, RTC_CNTL_BROWN_OUT_INT_ENA_M, RTC_INTR_FLAG_IRAM);
    isrRegistered = true;
  }
  brownout_ll_intr_enable(true);
  printf("armBrownout: armed\n"); fflush(stdout);
}

uint8_t brownoutArmedLevel() {
  return armedLevel;
}

void brownoutDump() {
  // Read back what the comparator hardware actually holds. Everything reported up to now
  // has been the value this code STORED, never a readback, so "armed at level 2" was an
  // assumption carried through three battery runs. Read-only and safe - unlike arming a
  // threshold at or above the rail to see whether it trips, which hangs the board.
  // On the S3 the THRESHOLD is not in RTC_CNTL at all: it is an analog setting reached
  // over the internal REGI2C bus (brownout_ll_set_threshold uses REGI2C_WRITE_MASK with
  // I2C_BOD / I2C_BOD_THRESHOLD), which is why the RTC_CNTL_DBROWN_OUT_* symbols exist
  // only on the original ESP32. Read it back the same way it is written
  // Two different registers are involved and mixing them up produces a convincing lie:
  // the enable/reset/power-down bits are in RTC_CNTL_BROWN_OUT_REG, but the INTERRUPT
  // enable is bit 9 of RTC_CNTL_INT_ENA_REG (brownout_ll_intr_enable writes
  // RTCCNTL.int_ena.rtc_brown_out). Reading that bit from the wrong register reports the
  // interrupt as permanently disabled, which looks exactly like a root cause
  uint32_t r = READ_PERI_REG(RTC_CNTL_BROWN_OUT_REG);
  uint32_t ie = READ_PERI_REG(RTC_CNTL_INT_ENA_REG);
  uint32_t thres = REGI2C_READ_MASK(I2C_BOD, I2C_BOD_THRESHOLD);
  LOG_ALT("Brownout HW: threshold=%lu (code thinks %u), ena=%d rst_ena=%d close_flash=%d pd_rf=%d int_ena=%d, terminal=%d, bo 0x%08lX ie 0x%08lX",
    thres, armedLevel,
    (r & RTC_CNTL_BROWN_OUT_ENA) ? 1 : 0,
    (r & RTC_CNTL_BROWN_OUT_RST_ENA) ? 1 : 0,
    (r & RTC_CNTL_BROWN_OUT_CLOSE_FLASH_ENA) ? 1 : 0,
    (r & RTC_CNTL_BROWN_OUT_PD_RF_ENA) ? 1 : 0,
    (ie & RTC_CNTL_BROWN_OUT_INT_ENA) ? 1 : 0,
    brownoutTerminal ? 1 : 0, r, ie);
  logSyncSD();
}

bool hadBrownout() {
  // A brownout on the boot immediately before this one, and only that one. Both sources -
  // the RTC flag our ISR sets, and the reset reason a terminal hardware reset leaves
  // instead (the ISR never runs in that case) - are read once in initBrownout(), which
  // runs from logSetup() long before anything asks. Reading the RTC flag here instead was
  // the bug: it is still 'R' on every later boot
  return brownoutThisBoot;
}

uint8_t brownoutProbeBand() {
  // Coarse rail reading, and the only voltage telemetry available without a divider: walk
  // the threshold down and report the lowest level that still trips, ie how far the rail
  // has fallen. 0 = healthy (above ~2.98V, nothing trips).
  // Safe only because warning stages neither reset nor power anything down; every exit
  // path restores the armed warning level.
  // Rate limited and cached: this is called from the status poll, and re-arming the
  // comparator on every browser refresh is both wasteful and a needless window in which
  // the armed level is not the warning level
  static uint8_t cachedBand = 0;
  static uint32_t lastProbeMs = 0;
  if (brownoutTerminal) return cachedBand; // never probe once the killswitch is armed
  uint32_t now = millis();
  if (lastProbeMs && now - lastProbeMs < 30000) return cachedBand;
  lastProbeMs = now;
  // A genuine trip may be pending in supplySagging, waiting for the capture task to
  // action it. The probe uses that same flag, so it must be preserved across the walk or
  // a status poll landing at the wrong moment would silently discard a real sag
  bool pendingSag = supplySagging;
  uint8_t restore = armedLevel;
  uint8_t band = 0;
  for (uint8_t lvl = BROWNOUT_WARN_LVL; lvl <= BROWNOUT_DET_LVL; lvl++) {
    supplySagging = false;
    armBrownout(lvl, false);
    delayMicroseconds(200); // let the comparator settle before believing it
    if (!supplySagging) break; // rail is above this level - done
    band = lvl; // tripped, so the rail is below this level's threshold
  }
  armBrownout(restore, false);
  supplySagging = pendingSag; // never lose a real trip to a probe
  cachedBand = band;
  return band;
}

void debugDirtyReboot() {
  // Debug: reboot with no cleanup at all, exactly as the terminal brownout stage does -
  // same reset hint and same RTC flag, so the whole post-brownout boot path is exercised.
  // Lives here because esp_restart_noos() needs the private system header this file
  // already pulls in. Leaves the filesystem holding only what was actually flushed,
  // which is how a recording looks after a supply loss, so boot recovery can be tested
  // repeatably without pulling the plug
  esp_reset_reason_set_hint(ESP_RST_BROWNOUT);
  brownoutStatus = 'B';
  esp_restart_noos();
}

static void initBrownout(void) {
  // Report only once to prevent a bootloop of warnings, but ALWAYS arm the detector: the
  // old code only configured it when brownoutStatus was clear, so after the first sag set
  // 'B' (then 'R') the custom detector was never re-armed again for the rest of the
  // discharge - it went missing exactly when a battery needs it most
  // This is also the single place the per-boot brownout answer is decided, because it runs
  // from logSetup() before any caller. 'B' means the previous boot ended in a brownout our
  // ISR saw; the reset reason covers the terminal hardware reset, where it did not. 'R'
  // means the boot AFTER that one, which has already had its holdoff - report it once more
  // for the record, then clear, so the flag cannot outlive the event that set it
  if (brownoutStatus == 'R') {
    LOG_WRN("Brownout warning previously notified - holdoff already served, clearing");
    brownoutStatus = 0;
  } else if (brownoutStatus == 'B') {
    LOG_WRN("Brownout occurred due to inadequate power supply");
    brownoutStatus = 'R';
    brownoutThisBoot = true;
  } else brownoutStatus = 0;
  if (esp_reset_reason() == ESP_RST_BROWNOUT) brownoutThisBoot = true;
  // Back to the IDF default: level 7 with hardware reset. Arming the warning level was
  // pointless and actively harmful here - the rail carries no usable warning (the buck
  // holds it regulated until the cell is at the edge), and a trip wedges the chip rather
  // than delivering an interrupt, which on a field device means it stops until someone
  // power cycles it. A clean hardware reset at 2.44V is what boot recovery is built for.
  // The warning now comes from battMonitor(), which watches the battery instead
  armBrownout(BROWNOUT_DET_LVL, true);
}

static void boardInfo() {
  LOG_INF("Chip %s, %u cores @ %luMhz, rev %u", ESP.getChipModel(), ESP.getChipCores(), ESP.getCpuFreqMHz(), ESP.getChipRevision() / 100);
  FlashMode_t ideMode = ESP.getFlashChipMode();
  LOG_INF("Flash %s, mode %s @ %luMhz", fmtSize(ESP.getFlashChipSize()), (ideMode == FM_QIO ? "QIO" : ideMode == FM_QOUT ? "QOUT" : ideMode == FM_DIO ? "DIO" : ideMode == FM_DOUT ? "DOUT" : "UNKNOWN"), ESP.getFlashChipSpeed() / OneMHz);

#if defined(CONFIG_SPIRAM_MODE_OCT)
  const char* psramMode = "OPI";
#else 
  const char* psramMode = "QSPI";
#endif
  char memInfo[100] = "none";
#if !CONFIG_IDF_TARGET_ESP32C3
  if (psramFound()) sprintf(memInfo, "%s, mode %s @ %dMhz", fmtSize(ESP.getPsramSize()), psramMode, CONFIG_SPIRAM_SPEED);
#endif
  LOG_INF("PSRAM %s", memInfo);
}

void logSetup() {
  // prep logging environment
  // A supply failure is not a software crash loop, and must not be treated as one. Any
  // uncleaned reboot leaves crashLoop set (resetCrashLoop only runs on a controlled
  // restart), and a startupFailure here makes startWebServer() return false, which stops
  // setup() ever calling prepRecording() - so a battery board that browned out would come
  // back web-only and never record or self-repair again. Brownouts get the 60s recording
  // holdoff in prepRecording() as their loop guard instead, which is the right shape
  // NOTHING here may log: the queues LOG_* posts to are created further down this
  // function, and logging before that asserts in xQueueReceive on a NULL handle - an
  // unrecoverable boot loop, because the condition that reaches it survives in RTC memory.
  // printResetReason() already reports the brownout once logging is alive
  // esp_reset_reason() is the authoritative signal and the RTC flag is not: the terminal
  // stage arms the comparator with reset_enabled, so the HARDWARE resets and our ISR never
  // runs to set brownoutStatus. Trusting the flag alone made every real brownout look like
  // a software crash loop, which suppressed prepRecording() - a battery camera came back
  // web-only and never recorded again (measured on the 28 Aug battery run, tasks 14 not 19)
  // A suspected crash loop is INFORMATION, not a startup failure. Setting startupFailure
  // here made startWebServer() return false, which made setup() skip prepRecording()
  // entirely - 15 tasks instead of 19, no capture task, no recording at all. And crashLoop
  // is set on every boot while resetCrashLoop() only runs on a controlled restart or from
  // the ping success callback, so on a board whose wifi is unreliable it never gets
  // cleared: one uncontrolled reset (button, power cut, brownout) then silently disabled
  // recording for every boot thereafter. Record it and report it once logging is alive
  crashLoopSuspected = (crashLoop == MAGIC_NUM);
  crashLoop = MAGIC_NUM;
  if (logHandle == NULL) {
    set_arduino_panic_handler(appPanicHandler, NULL);
    Serial.begin(115200);
    Serial.setDebugOutput(DBG_ON);
    printf("\n\n");
    if (DEBUG_MEM) printf("init > Free: heap %lu\n", ESP.getFreeHeap());
    // Lockup forensics: dump the comparator state the app starts with, before
    // initBrownout() rearms it. This is IDF's esp_brownout_init config (re-applied
    // early in every boot), not reset survival - measured 31 Aug: identical
    // 0x4BFFC020 on hard and soft boots. Kept because a deviation here on a future
    // wedge would be the first clue. Raw printf + fflush + a beat of delay,
    // deliberately not the log system: these lines must reach the host even if the
    // chip dies inside the arm a few lines later
    uint32_t bodInherit = READ_PERI_REG(RTC_CNTL_BROWN_OUT_REG);
    uint32_t bodIntInherit = READ_PERI_REG(RTC_CNTL_INT_ENA_REG);
    uint32_t bodThresInherit = REGI2C_READ_MASK(I2C_BOD, I2C_BOD_THRESHOLD);
    printf("BOD inherited: bo 0x%08lX ie 0x%08lX thres %lu (ena=%d rst=%d close_flash=%d pd_rf=%d int=%d)\n",
      bodInherit, bodIntInherit, bodThresInherit,
      (bodInherit & RTC_CNTL_BROWN_OUT_ENA) ? 1 : 0,
      (bodInherit & RTC_CNTL_BROWN_OUT_RST_ENA) ? 1 : 0,
      (bodInherit & RTC_CNTL_BROWN_OUT_CLOSE_FLASH_ENA) ? 1 : 0,
      (bodInherit & RTC_CNTL_BROWN_OUT_PD_RF_ENA) ? 1 : 0,
      (bodIntInherit & RTC_CNTL_BROWN_OUT_INT_ENA) ? 1 : 0);
    fflush(stdout);
    delay(20); // let the USB host drain the line while the chip is provably alive
    (DBG_ON) ? esp_log_level_set("*", DBG_LVL) : esp_log_level_set("*", ESP_LOG_NONE); // suppress esp log messages
    esp_log_set_vprintf(vprintfRedirect); // redirect esp_log output to app log

    UBaseType_t poolMem = psramFound() ? MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT : MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    poolBufs = (char (*)[MAX_OUT])heap_caps_malloc(LOG_BUF_COUNT * MAX_OUT, poolMem);
    if (!poolBufs) snprintf(startupFailure, SF_LEN, STARTUP_FAIL "Failed to alloc poolBufs");
    else {
      // Create pool queue from static storage
      logFreePool = xQueueCreateStatic(
          LOG_BUF_COUNT,
          sizeof(char*),
          freePoolStorage,
          &freePoolStatic
      );
      // Create log queue from static storage
      logQueue = xQueueCreateStatic(
          LOG_QUEUE_DEPTH,
          sizeof(char*),
          logQueueStorage,
          &logQueueStatic
      );
      // Populate the free pool
      for (int i = 0; i < LOG_BUF_COUNT; i++) {
          char *p = poolBufs[i];
          xQueueSend(logFreePool, &p, 0);
      }
      xTaskCreateWithCaps(logTask, "logTask", LOG_STACK_SIZE, NULL, LOG_PRI, &logHandle, STACK_MEM);
      
      if (mlogEnd >= RAM_LOG_LEN) ramLogClear(); // init
      LOG_SEND("\n\n=============== %s %s ===============\n", APP_NAME, APP_VER);
      LOG_INF("Setup RAM based log, size %u, starting from %u", RAM_LOG_LEN, mlogEnd);
      if (!DBG_ON) esp_log_level_set("*", ESP_LOG_ERROR); // show ESP_LOG_ERROR messages during init
      wakeupResetReason();
      // breadcrumbs bracketing the prime lockup suspect - if a wedge boot's capture ends
      // between these two lines, armBrownout's own trace narrows it to the exact call
      printf("initBrownout: entering\n"); fflush(stdout); delay(20);
      initBrownout();
      printf("initBrownout: done, BOD now bo 0x%08lX ie 0x%08lX thres %lu\n",
        READ_PERI_REG(RTC_CNTL_BROWN_OUT_REG), READ_PERI_REG(RTC_CNTL_INT_ENA_REG),
        (uint32_t)REGI2C_READ_MASK(I2C_BOD, I2C_BOD_THRESHOLD));
      fflush(stdout);
      boardInfo();
      debugMemory("logSetup");
    }
  }
}

/************** task monitoring ***************/

#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 1, 0)

static const char* getTaskStateString(eTaskState state) {
  // 
  switch (state) { 
    case eRunning: return "Running"; 
    case eReady: return "Ready"; 
    case eBlocked: return "Blocked"; 
    case eSuspended: return "Suspended"; 
    case eDeleted: return "Deleted"; 
    case eInvalid: return "Invalid"; 
    default: return "Unknown";
  }
}

static void statsTask(void *arg) { 
  // Output real time task stats periodically
  #define STATS_TASK_PRIO     10
  #define STATS_INTERVAL      30000 // ms
  #define ARRAY_SIZE_OFFSET   40   // Increase this if ESP_ERR_INVALID_SIZE

  bool onceOnly = *(bool*)arg; 
  esp_err_t ret = ESP_OK;  
  TaskStatus_t *statsArray = NULL;
  UBaseType_t statsArraySize;
  static configRUN_TIME_COUNTER_TYPE prevRunCounter = 0;
  configRUN_TIME_COUNTER_TYPE runCounter;
  
  do {
    delay(STATS_INTERVAL);

    do { // fake loop for breaks
      // Allocate array to store current task states
      statsArraySize = uxTaskGetNumberOfTasks() + ARRAY_SIZE_OFFSET;
      statsArray = (TaskStatus_t *)malloc(sizeof(TaskStatus_t) * statsArraySize);
      if (statsArray == NULL) {
        ret = ESP_ERR_NO_MEM;
        break;
      }

      // Get current task states
      statsArraySize = uxTaskGetSystemState(statsArray, statsArraySize, &runCounter);
      if (statsArraySize == 0) {
        ret = ESP_ERR_INVALID_SIZE;
        break;
      }

      // Calculate total_elapsed_time in units of run time stats clock period
      if (runCounter - prevRunCounter == 0) {
        ret = ESP_ERR_INVALID_STATE;
        break;
      }

      LOG_SEND("\nTask stats interval %ds on %u cores\n", STATS_INTERVAL / 1000, CONFIG_FREERTOS_NUMBER_OF_CORES);
      LOG_SEND("\n| %-16s | %-10s | %-3s | %-4s | %-6s |\n", "Task name", "State", "Pri", "Core", "Core%");
      LOG_SEND("|------------------|------------|-----|------|--------|\n"); 
      // Match each task in start_array to those in the end_array
      for (int i = 0; i < statsArraySize; i++) {
        float percentage_time = ((float)statsArray[i].ulRunTimeCounter * 100.0) / runCounter;
        UBaseType_t coreId = statsArray[i].xCoreID;
        LOG_SEND("| %-16s | %-10s | %3u | %4c | %5.1f%% |\n", 
          statsArray[i].pcTaskName, getTaskStateString(statsArray[i].eCurrentState), (int)statsArray[i].uxCurrentPriority, coreId == tskNO_AFFINITY ? '*' : '0' + (int)coreId, percentage_time);
      }
      LOG_SEND("|------------------|------------|-----|------|--------|\n"); 
    } while (false);

    prevRunCounter = runCounter;
    free(statsArray);
    if (ret != ESP_OK) LOG_WRN("Failed to start task monitoring %s", espErrMsg(ret));
  } while (!onceOnly);
  vTaskDelete(NULL);
}

void runTaskStats(bool _onceOnly) {
  // invoke task stats monitoring
  static bool onceOnly = _onceOnly;
  // Allow other core to finish initialization
  vTaskDelay(pdMS_TO_TICKS(100));
  // Create and start stats task
  xTaskCreatePinnedToCore(statsTask, "statsTask", 4096, &onceOnly, STATS_TASK_PRIO, NULL, tskNO_AFFINITY);
}
#endif

void checkMemory(const char* source) {
  LOG_INF("%s Free: heap %lu, block: %lu, min: %lu, pSRAM %lu", strlen(source) ? source : "Setup", ESP.getFreeHeap(), ESP.getMaxAllocHeap(), ESP.getMinFreeHeap(), ESP.getFreePsram());
  if (ESP.getFreeHeap() < WARN_HEAP) LOG_WRN("Free heap only %lu, min %lu", ESP.getFreeHeap(), ESP.getMinFreeHeap());
  if (ESP.getMaxAllocHeap() < WARN_ALLOC) LOG_WRN("Max allocatable heap block is only %lu", ESP.getMaxAllocHeap());
  if (!strlen(source) && DEBUG_MEM) runTaskStats();
}

uint32_t checkStackUse(TaskHandle_t thisTask, int taskIdx) {
  // get minimum free stack size for task since started
  // taskIdx used to index minStack[] array
  static uint32_t minStack[20]; 
  uint32_t freeStack = 0;
  if (thisTask != NULL) {
    freeStack = (uint32_t)uxTaskGetStackHighWaterMark(thisTask);
    if (!minStack[taskIdx]) {
      minStack[taskIdx] = freeStack; // initialise
      LOG_INF("Task %s on core %d, initial stack space %lu", pcTaskGetTaskName(thisTask), xPortGetCoreID(), freeStack);
    }
    if (freeStack < minStack[taskIdx]) {
      minStack[taskIdx] = freeStack;
      if (freeStack < MIN_STACK_FREE) LOG_WRN("Task %s on core %d, stack space only: %lu", pcTaskGetTaskName(thisTask), xPortGetCoreID(), freeStack);
      else LOG_INF("Task %s on core %d, stack space reduced to %lu", pcTaskGetTaskName(thisTask), xPortGetCoreID(), freeStack);
    }
  }
  return freeStack;
}

/************************ system info **************************/

#include <driver/gpio.h>
#include "esp32-hal-periman.h"

static void printGpioInfo() {
  // from https://github.com/espressif/arduino-esp32/blob/master/cores/esp32/chip-debug-report.cpp
  LOG_SEND("Assigned GPIO Info:\n");
  for (uint8_t i = 0; i < SOC_GPIO_PIN_COUNT; i++) {
    if (!perimanPinIsValid(i)) continue;  //invalid pin
    peripheral_bus_type_t type = perimanGetPinBusType(i);
    if (type == ESP32_BUS_TYPE_INIT) continue;  //unused pin

    char gpioInf[100];
    char* p = gpioInf;
#if defined(BOARD_HAS_PIN_REMAP)
    int dpin = gpioNumberToDigitalPin(i);
    if (dpin < 0) continue;  //pin is not exported
    else p+= sprintf(p, "  D%-3d|%4u : ", dpin, i);
#else
    p+= sprintf(p, "  %4u : ", i);
#endif
    const char *extra_type = perimanGetPinBusExtraType(i);
    if (extra_type) p+= sprintf(p, "%s", extra_type);
    else p+= sprintf(p, "%s", perimanGetTypeName(type));
    int8_t bus_number = perimanGetPinBusNum(i);
    if (bus_number != -1) p+= sprintf(p, "[%u]", bus_number);

    int8_t bus_channel = perimanGetPinBusChannel(i);
    if (bus_channel != -1) p+= sprintf(p, "[%u]", bus_channel);
    *p = 0;
    LOG_SEND("%s\n", gpioInf);
  }
}

// display partition map
const char* partitionTypeToStr(uint8_t type) {
  // Map type to string
  switch (type) {
    case ESP_PARTITION_TYPE_APP: return "APP";
    case ESP_PARTITION_TYPE_DATA: return "DATA";
    default: return "UNKNOWN";
  }
}

const char* partitionSubtypeToStr(uint8_t type, uint8_t subtype) {
  // Map subtype to string based on type
  if (type == ESP_PARTITION_TYPE_APP) {
    switch (subtype) {
      case ESP_PARTITION_SUBTYPE_APP_FACTORY: return "Factory";
      case ESP_PARTITION_SUBTYPE_APP_OTA_0: return "OTA_0";
      case ESP_PARTITION_SUBTYPE_APP_OTA_1: return "OTA_1";
      case ESP_PARTITION_SUBTYPE_APP_OTA_2: return "OTA_2";
      case ESP_PARTITION_SUBTYPE_APP_OTA_3: return "OTA_3";
      case ESP_PARTITION_SUBTYPE_APP_OTA_4: return "OTA_4";
      case ESP_PARTITION_SUBTYPE_APP_OTA_5: return "OTA_5";
      default: return "App_Other";
    }
  } else if (type == ESP_PARTITION_TYPE_DATA) {
    switch (subtype) {
      case ESP_PARTITION_SUBTYPE_DATA_OTA: return "OTA_Data";
      case ESP_PARTITION_SUBTYPE_DATA_PHY: return "PHY";
      case ESP_PARTITION_SUBTYPE_DATA_NVS: return "NVS";
      case ESP_PARTITION_SUBTYPE_DATA_NVS_KEYS: return "NVS_Keys";
      case ESP_PARTITION_SUBTYPE_DATA_SPIFFS: return "SPIFFS";
      case ESP_PARTITION_SUBTYPE_DATA_FAT: return "FAT";
      default: return "Data_Other";
    }
  }
  return "Unknown";
}

static void printPartitionTable() {
  // print all partitions
  LOG_SEND("%-12s %-6s %-12s %-10s %-12s %-10s", "Partition", "Type", "Subtype", "Address", "Size", "Encrypted\n");
  LOG_SEND("-----------------------------------------------------------------\n");

  // Get iterator for all partitions
  esp_partition_iterator_t iter = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);

  if (iter == NULL) {
    LOG_ERR("No partitions found");
    return;
  }

  // Iterate through all partitions
  do {
    const esp_partition_t* part = esp_partition_get(iter);
    const char* typeStr = partitionTypeToStr(part->type);
    const char* subtypeStr = partitionSubtypeToStr(part->type, part->subtype);
    const char* label = part->label;
    LOG_SEND("%-12s %-6s %-12s 0x%08lX %-12s %-10s\n",
      label, typeStr, subtypeStr, part->address, fmtSize(part->size), part->encrypted ? "Yes" : "No");
    iter = esp_partition_next(iter);
  } while (iter != NULL);

  // Free iterator
  esp_partition_iterator_release(iter);
}

void showSys() {
  // output system details to web log
  logLine();
  boardInfo();
  logLine();
  LOG_SEND("%s v%s, arduino-esp32 v%s\n", APP_NAME, APP_VER, ESP_ARDUINO_VERSION_STR);
  logLine();
  printPartitionTable();
  logLine();
  printGpioInfo();
  logLine();
  runTaskStats(true);
  logLine();
  //gpio_dump_io_configuration(stdout, SOC_GPIO_VALID_GPIO_MASK);
}
