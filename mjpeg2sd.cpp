/*
  Capture ESP32 Cam JPEG images into a AVI file and store on SD
  matches file writes to the SD card sector size.
  AVI files stored on the SD card can also be selected and streamed to a browser.

  s60sc 2020, 2022, 2024, 2025
*/

#include "appGlobals.h"
#if INCLUDE_AF
#if __has_include("../libraries/OV5640_Auto_Focus_for_ESP32_Camera/src/ESP32_OV5640_AF.h") 
#include <ESP32_OV5640_AF.h>
#include <fcntl.h>  // playback reads the AVI through a POSIX fd, see readSD()
#include <unistd.h>
#include <esp_task_wdt.h>  // deleteTask() unsubscribes a task before deleting it
OV5640 ov5640AF = OV5640();
#else
#error "Need to install OV5640_Auto_Focus_for_ESP32_Camera library"
#endif
#endif

#define FB_CNT 4 // number of frame buffers

// user parameters set from web
bool useMotion = true; // whether to use camera for motion detection (with motionDetect.cpp)
bool forceRecord = false; // Recording enabled by rec button

// motion detection parameters
int moveStartChecks = 5; // checks per second for start motion
// While recording, a check only feeds the stop timer and the motion edges, so it can run
// slower: each check costs ~13ms of the capture task (SCCB reads), which at 5/s cost
// ~1.4fps of a Busy-100% recording. At 2/s the cost is ~0.5fps and re-asserting motion
// after a lull still takes only detectMotionFrames/2 = 1.5s of the moveStopSecs window
int moveStopChecks = 2; // checks per second while recording
// A motion triggered recording runs while movement continues and closes after moveStopSecs
// without it. Detection stays live through the recording - a zone check is ~20 SCCB reads
// with no frame decode, so there is nothing left that needs suspending. This also removes
// the stale-state false retrigger the old suspend-then-resume design produced (28 Aug:
// every motion recording was followed 0.4s later by a 15s clip of nothing)
int moveStopSecs = 10; // secs without motion before a triggered recording closes
int zoneCount = 2; // zones changed at once to signal motion (with zone delta from motionVal)
int zoneMask = 0xFFFF; // view-order bitmask of the 4x4 zones participating in detection
static bool zoneDetectOK = false; // sensor has the zone grid (OV5640) - set by prepCam()
// audio chunks are written every AUD_CHUNK_MIN bytes rather than every frame, so above
// 4fps the index needs well under 2 entries per frame. Overflowing it now closes the
// recording rather than rebooting, so this is a target rather than a hard ceiling
// Also the self-rescue bound. When the offered byte rate far exceeds the SD bus, HTTP can be
// unreachable for the whole recording, so a stop command may never arrive - the recording
// closing itself is then the only way back. 10000 frames meant up to ~20 minutes of that;
// 3600 caps it at 3 minutes at the 20fps default, ~6-7 under heavy shedding
int maxFrames = 3600; // maximum number of frames in video before auto close

// The sensor is tuned ABOVE the requested fps while the frame timer stays at the request:
// one frame is taken per timer tick off a GRAB_LATEST queue, so the surplus means every
// tick finds a fresh frame and the delivered rate is the timer's. Measured without it:
// recordings ran 0.7-1.3% under the request (ticks that found no frame when a zone check or
// SD write stalled the task). The surplus frames also feed the AEC/AWB.
//
// The surplus is bought out of the exposure ceiling, because max exposure is the frame
// period less a few blanking rows, and it was a flat 5% at every rate. What the surplus has
// to cover is scheduling jitter, which is an amount of TIME, not a fraction of the frame
// period - so a flat fraction over-provisions badly where the period is long. At the HD
// ceiling 5% is about 1ms of slack; at 11fps the same 5% is 4.5ms, and it is taken out of
// the exposure ceiling exactly where light is scarcest.
//
// So hold the slack at ~1ms instead: overdrive = 1 + fps/1000, clamped. That reproduces
// today's value at the top of the range, where it was tuned and where delivery accuracy
// matters most, and relaxes towards the floor as the period lengthens. Ceiling requests
// still get no effective overdrive (the VTS floor caps them) and run ~1% low by design.
#define FPS_OVERDRIVE_MAX 1.05f  // the historic flat value, retained at the top of the range
#define FPS_OVERDRIVE_MIN 1.01f  // never less than this: the measured deficit reached 1.3%
#define FPS_OVERDRIVE_SLACK_MS 1.0f

static float fpsOverdrive(uint8_t fps) {
  float od = 1.0f + FPS_OVERDRIVE_SLACK_MS * fps / 1000.0f;
  if (od > FPS_OVERDRIVE_MAX) od = FPS_OVERDRIVE_MAX;
  if (od < FPS_OVERDRIVE_MIN) od = FPS_OVERDRIVE_MIN;
  return od;
}

uint8_t fpsCeiling(framesize_t fs) {
  // the top of the fps slider: the tuned ceiling when tuning is on and the size has one,
  // else the driver-clock default rate
  return (tunedFps && frameData[fs].maxTunedFPS) ? frameData[fs].maxTunedFPS : frameData[fs].defaultFPS;
}

// status & control fields
uint8_t FPS = 0;
// The rate the user chose for the capture resolution. FPS itself now follows sensorFS, so
// without this a sensor switch would silently overwrite a manually set frame rate with the
// frame size default and never put it back
uint8_t captureFPS = 20;
bool nightTime = false;
uint8_t fsizePtr; // index to frameData[], the user's chosen capture resolution
bool doRecording = true; // whether to capture to SD or not
uint8_t xclkMhz = 20; // camera clock rate MHz
bool doKeepFrame = false;
static bool haveWav = false; // set once an audio chunk has been interleaved, drives the _S name suffix
char camModel[11];
static int siodGpio = SIOD_GPIO_NUM;
static int siocGpio = SIOC_GPIO_NUM;
size_t maxFrameBuffSize;
static int frameLimit;

// header and reporting info
static uint32_t vidSize; // total video size
static uint16_t frameCnt;
static uint32_t startTime; // total overall time
static uint32_t dTimeTot; // total frame decode/monitor time
// dTimeTot starts before esp_camera_fb_get(), so it bundles the wait for a frame in with
// the motion check and cannot be used to size detection cost - a stalled sensor or SD
// card shows up there as "monitoring time". These isolate the zoneMotion() call itself
static uint32_t mTimeTot;  // time actually spent in motion checks, in microseconds
static uint32_t mCheckCnt; // number of motion checks performed
static uint32_t badFrameCnt; // frames rejected as zero length or oversized
static uint32_t fTimeTot; // total frame buffering time
static uint32_t wTimeTot; // recording: total SD write time. Playback: consumer wait on the reader task
// playback device figures, kept apart from wTimeTot: the reader task's time inside File::read()
// overlaps the consumer's wait for that same read in wall-clock time, and summing the two into
// wTimeTot once reported 154ms of "SD read" per frame inside a 133ms frame period
static uint64_t sdReadUs; // reader task time inside File::read(), us
static uint32_t sdReadCnt; // clusters read
static uint32_t sdReadBytes; // bytes actually read, last cluster is partial
static uint32_t oTime; // file opening time
static uint32_t cTime; // file closing time
static uint32_t sTime; // file streaming time
static uint32_t frameInterval; // units of us between frames

// SD write budget governor. Holds the user's frame rate when frame bytes outgrow the SD
// write ceiling by stepping JPEG quality (more compression) for the rest of the clip,
// instead of letting FATFS backpressure shed frames silently - the dim-scene soak stored
// 0.7fps of a requested 6 when high-gain noise doubled QSXGA frames to 946KB. MJPEG
// frames are standalone so quality may change mid-clip; fps may not (the AVI header
// carries one rate). Boost is bounded, hysteretic, and restored when the clip closes.
// Quality NUMBER up = more compression on this driver.
#define GOV_MAX_BOOST 4   // quality steps above the user's setting
#define GOV_PUSH_PCT 90   // demand above this % of budget raises the boost. Must sit BELOW
                          // the saturation plateau: a backpressured card delivers ~94-98%
                          // of the nominal budget, so 95 made most ticks hover just under
                          // the trigger and the ramp took 15s (Test A, 28 Aug)
#define GOV_RELAX_PCT 60  // demand below this % for GOV_RELAX_TICKS lowers it. Must sit
                          // below PUSH/(1 + one step's effect): a quality step at QSXGA
                          // moves demand ~50%, and 75 made the governor flap q10<->q12
                          // every few seconds in the dim Test B (28 Aug) - easing at 74%
                          // landed straight back above 90% and re-boosted
#define GOV_RELAX_TICKS 2 // consecutive quiet ticks before easing off
uint8_t sdGovBoost = 0;    // current boost, 0 = user quality untouched (reported by updateFPS)
uint16_t sdGovFrameKB = 0; // last-second average frame KB while recording, else 0 (updateFPS)
static uint8_t govBaseQ;      // the user's quality, captured at clip open / user change
static uint8_t govMaxBoost;   // clip peak, for the closeAvi stats
static uint8_t govLowTicks;   // consecutive under-GOV_RELAX_PCT ticks
static uint32_t govWindowMs;  // start of the current 1s measurement window
static uint32_t govLastVidSize;
static uint16_t govLastFrameCnt;
// no-frame watchdog state (see noFrameRescue above processFrame)
static uint32_t lastGoodFrameMs = 0;
static uint16_t rescueClipCnt = 0; // rescues during the open recording, for closeAvi

// Supply sag state. On battery the rail decays gradually, and the brownout comparator
// armed at its warning level (utilsLog.cpp) is the only sensor this board has. Once it
// trips, the recording is landed while the SD card is still in spec and the device parks:
// no further recordings, no further SD writes, sensor to standby to stop our own draw
// dragging the rail down further. Deliberately one-way - a rail that has sagged this far
// is at end of life, and resuming 5MP capture into it is how cards get corrupted
bool supplyParked = false;
static uint32_t supplyParkedMs = 0;

// Battery monitor. The rail-side brownout comparator cannot trigger the landing on this
// board - the SGM6029 buck holds 3V3 regulated until the cell reaches ~3.3V and then
// passes through, so the rail says nothing useful until the collapse is already
// microseconds away (three battery runs, all reporting "no sag response recorded").
// A divider on the BATTERY side sees the decline while the rail is still perfectly
// regulated, which turns microseconds of warning into minutes. Hardware: 100k/100k from
// BAT+ to GND with 100nF at the tap, into an ADC1 pin (ADC2 is unreadable while wifi is up)
int battUse = 0;        // config: off by default - see the floor check in battMonitor()
int battPin = 1;        // config: D0 / GPIO1, ADC1 channel 0
int battScale = 2000;   // config: cell mV per 1000 ADC mV, ie 2000 = a 2:1 divider
int battWarnMv = 3400;  // config: land the clip at this cell voltage
uint16_t battMv = 0;    // last reading, 0 = not monitoring (reported in status)
// A cell below this cannot be running the board, so such a reading means the divider is
// absent, unwired or on the wrong pin - a floating ADC input reads anything, including
// near zero, and acting on that would park a perfectly healthy camera
#define BATT_FLOOR_MV 2500
#define BATT_CONFIRM 3 // consecutive sub-threshold samples before landing
static uint8_t battLowCount = 0;
static uint32_t battPollMs = 0;
static bool battAdcReady = false;
static bool battFloorWarned = false;
// a controlled restart during a recording must not orphan the file - set by appShutdown()
// and actioned by processFrame from the capture task's own context
static volatile bool shutdownPending = false;
static volatile bool capturePassBusy = false; // capture task is inside a processFrame burst
// FatFS only persists the directory entry and FAT chain on sync, so an un-synced file can
// read as empty however much data reached the card. Flushing bounds what an abrupt loss
// costs to this interval. Not free, and not purely a win: every flush is another FAT
// write, and a cut DURING a FAT write is the one failure that damages more than the
// current file. 2s is the middle of that trade, measured to cost nothing at HD/30/q6
#define AVI_FLUSH_SECS 2
static uint32_t lastFlushMs = 0;
// after a brownout boot, hold recording off briefly so a dying pack cannot drive a
// boot-record-brownout loop across the SD card
#define BROWNOUT_HOLDOFF_SECS 60
static bool holdoffActive = false;
static bool holdoffSavedRecording = false;

// SD card storage
// Playback double buffer, split by memory type: sdReadBuf is the DMA landing zone for
// File::read() and must be internal RAM (the sdmmc host bounces anything else through a
// per-sector temp buffer); iSDbuffer is the consumer's copy - CHUNK_HDR bytes of overlap
// then the cluster - and is only ever memcpy'd or handed to httpd, so it is allocated
// from PSRAM at startup. Splitting them is what lets RAMSIZE be 32KB without spending
// 64KB of internal RAM. The AVI capture path has its own write buffer.
uint8_t* iSDbuffer = NULL;
// Every playback read starts at AVI_HEADER_LEN + k * RAMSIZE, part way through a sector.
// FATFS copies the rest of that sector out of its window first, then DMA-reads the whole
// sectors that follow straight into our buffer at that offset (SD_READ_LEAD). The sdmmc
// driver only DMAs into a 4-byte aligned address; anything else it bounces through a temp
// buffer ONE SECTOR AT A TIME - 64 single-block commands per 32KB cluster at ~0.5ms each,
// the 1.0MB/s read rate that survived three other fixes while 32KB-aligned writes did
// 4.4MB/s. Skewing the read start by SD_READ_SKEW bytes puts that body on an aligned
// address, so it goes out as one multi-block command
#define SD_SECTOR 512
#define SD_READ_LEAD ((SD_SECTOR - AVI_HEADER_LEN % SD_SECTOR) % SD_SECTOR)
#define SD_READ_SKEW ((4 - SD_READ_LEAD % 4) % 4)
static uint8_t sdReadBuf[RAMSIZE + 4] __attribute__((aligned(4)));
static uint8_t sdWriteBuf[SD_WRITE_SIZE + CHUNK_HDR];
static size_t highPoint;
static File aviFile;
static char aviFileName[FILE_NAME_LEN];

// SD playback
static int playbackFd = -1; // POSIX fd on the AVI being played, see readSD()
static char partName[FILE_NAME_LEN];
static size_t readLen;
static uint8_t recFPS;
static uint32_t recDuration;
static uint8_t saveFPS = 99;
bool doPlayback = false; // controls playback

// task control
TaskHandle_t captureHandle = NULL;
TaskHandle_t playbackHandle = NULL;
static SemaphoreHandle_t readSemaphore;
static SemaphoreHandle_t playbackSemaphore;
SemaphoreHandle_t frameSemaphore[MAX_STREAMS] = {NULL};
SemaphoreHandle_t motionSemaphore = NULL;
SemaphoreHandle_t aviMutex = NULL;
static volatile bool isPlaying = false; // controls playback on app
bool isCapturing = false;
bool stopPlayback = false; // controls if playback allowed
int dashCamOn = 0; // whether to use / duration of dashcam style continuous recording

#ifndef AUXILIARY
framesize_t maxFS = FRAMESIZE_SVGA; // default, sizes the camera frame buffers
// AVI recording is capped here, but the frame size setting itself is not - there is only
// one global fsizePtr, and stills read the same live frame, so capping the setting would
// cap them too. Above this cap you still get full resolution stills, just no motion or
// forced AVI recording.
// QSXGA since the table 2-1 UI rework: 5MP video at the tuned ~7fps ceiling. The buffer
// arithmetic: maxFrameBuffSize is QSXGA WxH/5 = 983KB and a q6 5MP frame runs ~600KB, so
// the frame fits the buffer sized for it by prepCam() - verified, not assumed, in the sweep
framesize_t maxVideoFS = FRAMESIZE_QSXGA;
// what the sensor is set to right now. Initialised by prepCam() once fsizePtr is known
framesize_t sensorFS = FRAMESIZE_VGA; // placeholder until prepCam()/startSDtasks() pin it

/**************** timers & ISRs ************************/

static void IRAM_ATTR frameISR() {
  // interrupt at current frame rate
  BaseType_t xHigherPriorityTaskWoken = pdFALSE;
  if (isPlaying) xSemaphoreGiveFromISR (playbackSemaphore, &xHigherPriorityTaskWoken ); // notify playback to send frame
  if (captureHandle != NULL) vTaskNotifyGiveFromISR(captureHandle, &xHigherPriorityTaskWoken); // wake capture task to process frame
  portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

void controlFrameTimer(bool restartTimer) {
  // frame timer control
  static hw_timer_t* frameTimer = NULL;
  // stop current timer
  if (frameTimer) {
    timerDetachInterrupt(frameTimer);
    timerEnd(frameTimer);
    frameTimer = NULL;
  }
  if (restartTimer) {
    // (re)start timer interrupt for required framerate
    frameTimer = timerBegin(OneMHz);
    if (frameTimer) {
      frameInterval = OneMHz / FPS; // in units of us
      LOG_VRB("Frame timer interval %lums for FPS %u", frameInterval / 1000, FPS);
      timerAttachInterrupt(frameTimer, &frameISR);
      timerAlarm(frameTimer, frameInterval, true, 0); // micro seconds
    } else LOG_ERR("Failed to setup frameTimer");
  }
}

static void recordingCamMode(bool starting) {
  // Camera behaviour at recording boundaries: stop the two automatics whose mid-clip
  // adjustments read as artifacts, restore them after.
  //
  // AWB: freeze via 0x5196[5] (datasheet table 7-22) rather than switching to manual gains -
  // the AWB core keeps its state, colour just stops pumping for the duration of the clip.
  // Gain-driven UV shift (5.10) is a different mechanism and still applies; this only stops
  // the white point walking mid-recording.
  //
  // AF is deliberately left alone. The first version of this fired a single-shot acquisition
  // (CMD_MAIN 0x03) at recording start to stop continuous AF hunting mid-clip - and it wedged
  // the AF MCU in a permanent hunt instead: FW status read 0x00 (focusing) for the entire
  // recording and delivered frames fell from 30.0 to 21.5fps, reproducibly. The library's
  // header carries no documented pause/release command to do this properly, and the datasheet
  // does not cover the mailbox at all, so per the rule against inventing mailbox values the
  // continuous AF of the boot state stands. Measured, continuous AF at steady state is free -
  // 30.0fps recordings with it active - because a focused scene leaves the MCU idle (0x10).
  // The cost accepted is refocus breathing if the scene depth changes mid-clip.
  sensor_t* s = esp_camera_sensor_get();
  if (s == NULL) return;
  s->set_reg(s, 0x5196, 0x20, starting ? 0x20 : 0x00); // AWB freeze bit only
}

/**************** capture AVI  ************************/

uint16_t sdBudgetKBs() {
  // measured sustained SD write ceiling for the bus clock in force: 4458 KB/s at 53.3MHz
  // (sdBusDiv 3) and 3643 at the stock 40MHz, both from 20 s HD30 forced recordings on
  // 2 Sep 2026 with 32KB-boundary-aligned blocks (BOARD_TESTING 24); the 28 Aug figures
  // were 4420 / 3550. Single source for the UI badge (updateFPS) and the governor, so the
  // two can never disagree
  return (sdBusKHz() > 50000) ? 4458 : 3643;
}

uint16_t frameWindowKB(int fs) {
  // The sensor's JPEG frame-window cliff per size, in KB: the largest frame PROVEN to
  // deliver (random-pattern descend, 28 Aug - BOARD_TESTING 20). A frame past the
  // cliff is truncated in flight and the driver delivers NOTHING, so the badge warns
  // and the governor pre-arms at GOV_WINDOW_PCT of these. Measured constant per size
  // across fps AND clock (HD identical at 5/10/30fps, 21-80MHz), and NOT a readout
  // property: HD dies at ~295KB where 1280X960 survives 383KB on identical sensor
  // timing - it follows the output size. QVGA's cliff is unreachable (its frames
  // cannot physically get that big); unmeasured sizes return 0 = no prediction.
  // QSXGA's real cliff is masked by maxFrameBuffSize (983KB); 946 is the largest
  // frame the dim soak delivered there.
  switch (fs) {
    case FRAMESIZE_VGA: return 266;
    case FRAMESIZE_HD: return 291;
    case FRAMESIZE_FHD: return 443;
    case FRAMESIZE_QSXGA: return 946;
    default:
      if (fs == FS_1280X960) return 383;
      // INHERITED, not measured: the cliff follows the OUTPUT size (HD dies at ~295KB where
      // 1280X960 survives 383KB on identical sensor timing), and these emit the same
      // 1920x1080 as FHDNARROW. Replace with a measured figure if either is ever swept
      if (fs == FS_FHDMID || fs == FS_FHDFULL) return 443;
      // QHD deliberately stays 0. It was swept and recorded on 3 Sep 2026, but its cliff was
      // NOT measured - that needs the random-pattern descend of BOARD_TESTING 20, not a rate
      // sweep - and it cannot be inherited from anything: 2560x1440 sits between FHD's 443KB
      // and QSXGA's 946KB with no basis for interpolating. Lit q10 frames measured 372KB at
      // 9fps, and a dim scene roughly doubles frame size, so the headroom is genuinely unknown.
      // 0 means "no prediction", which leaves noFrameRescue() as the guard rather than a guess
      return 0;
  }
}

void govRebaseQuality(int q) {
  // a user quality change mid-recording re-bases the governor; any active boost
  // reapplies on top of the new base at the next tick
  govBaseQ = q;
}

static void sdGovernor() {
  // 1Hz while recording: compare the last second's write demand to the SD ceiling and
  // trade JPEG quality for frame delivery when frames outgrow it. A saturated card
  // delivers ~100% of budget by definition, so sustained saturation keeps raising the
  // boost until frames shrink below the ceiling - which is the recovery path
  uint32_t now = millis();
  if (now - govWindowMs < 1000) return;
  uint32_t winBytes = vidSize - govLastVidSize;
  uint16_t winFrames = frameCnt - govLastFrameCnt;
  uint32_t demandKBs = (uint32_t)(((uint64_t)winBytes * 1000) / (now - govWindowMs)) / 1024;
  sdGovFrameKB = winFrames ? winBytes / winFrames / 1024 : 0;
  govLastVidSize = vidSize;
  govLastFrameCnt = frameCnt;
  govWindowMs = now;
  // persist the directory entry and FAT chain so an abrupt loss costs at most this
  // interval rather than the whole clip (see AVI_FLUSH_SECS)
  if (now - lastFlushMs >= AVI_FLUSH_SECS * 1000) {
    lastFlushMs = now;
    aviFile.flush();
  }
  sensor_t* s = esp_camera_sensor_get();
  if (s == NULL) return;
  uint16_t budget = sdBudgetKBs();
  // pre-arm: frames approaching the sensor's JPEG frame window get compressed BEFORE
  // they cross it - past the cliff they stop arriving entirely and only the no-frame
  // watchdog can act. 80% of a proven-delivered figure leaves real margin
#define GOV_WINDOW_PCT 80
  uint16_t capKB = frameWindowKB(fsizePtr);
  bool windowNear = capKB && sdGovFrameKB > (uint32_t)capKB * GOV_WINDOW_PCT / 100;
  if (windowNear || demandKBs > (uint32_t)budget * GOV_PUSH_PCT / 100) {
    govLowTicks = 0;
    if (sdGovBoost < GOV_MAX_BOOST) {
      sdGovBoost++;
      if (sdGovBoost > govMaxBoost) govMaxBoost = sdGovBoost;
      int q = govBaseQ + sdGovBoost;
      if (q > 63) q = 63;
      s->set_quality(s, q);
      LOG_INF("SD governor: quality %d (boost %u) - demand %lu KB/s of %u", q, sdGovBoost, demandKBs, budget);
    }
  } else if (demandKBs < (uint32_t)budget * GOV_RELAX_PCT / 100 && sdGovBoost && !windowNear) {
    // !windowNear: at low fps the SD demand can be tiny while frames sit just under
    // the frame window - easing the boost there would grow them straight over it
    if (++govLowTicks >= GOV_RELAX_TICKS) {
      govLowTicks = 0;
      sdGovBoost--;
      int q = govBaseQ + sdGovBoost;
      if (q > 63) q = 63;
      s->set_quality(s, q);
      LOG_INF("SD governor: quality %d (boost %u) - demand %lu KB/s of %u", q, sdGovBoost, demandKBs, budget);
    }
  } else govLowTicks = 0;
}

// true from AVITEMP open until closeAvi() has fully landed the file. OTAprereq() waits on
// this rather than isCapturing, which processFrame clears at the decision point BEFORE the
// close branch runs - waiting on isCapturing returns with the index write and rename still
// to come, and the teardown then races the close (seen on the first OTA verification run:
// the clip went through boot recovery instead of landing)
static volatile bool aviBusy = false;

static void openAvi() {
  // derive filename from date & time, store in date folder
  // time to open a new file on SD increases with the number of files already present
  aviBusy = true;
  oTime = millis();
  dateFormat(partName, sizeof(partName), true);
  STORAGE.mkdir(partName); // make date folder if not present
  dateFormat(partName, sizeof(partName), false);
  // open avi file with temporary name
  aviFile = STORAGE.open(AVITEMP, FILE_WRITE);
  oTime = millis() - oTime;
  LOG_VRB("File opening time: %lums", oTime);
  recordingCamMode(true); // freeze AWB, single-shot AF for the clip
#if INCLUDE_AUDIO
  startAudioRecord();
#endif
  // initialisation of counters
  startTime = millis();
  frameCnt = fTimeTot = wTimeTot = dTimeTot = vidSize = 0;
  // governor starts each file from the user's quality; boost is re-earned per window.
  // status.quality is safe as the base here because closeAvi always restored it before
  // the previous file was renamed - a dashcam chain re-boosts within a tick or two
  sensor_t* govSensor = esp_camera_sensor_get();
  govBaseQ = (govSensor != NULL) ? govSensor->status.quality : 10;
  sdGovBoost = govMaxBoost = govLowTicks = 0;
  sdGovFrameKB = 0;
  govLastVidSize = govLastFrameCnt = 0;
  govWindowMs = millis();
  rescueClipCnt = 0; // no-frame watchdog steps within this clip, for the stats below
  haveWav = false;
  // Provisional header into the reserved space. Costs no extra write - this block is
  // written anyway - and buys two things: an interrupted file no longer opens with the
  // PREVIOUS recording's header sitting at offset 0 (sdWriteBuf is never cleared, which
  // made an orphan look like a valid file of the wrong geometry), and recoverAvi() can
  // read the real fps and frame size back out of it. Must precede prepAviIndex(), because
  // buildAviHdr() finishes by resetting moviSize/idxPtr/idxOffset
  buildAviHdr(FPS, fsizePtr, 0, 0);
  memcpy(sdWriteBuf, aviHeader, AVI_HEADER_LEN);
  highPoint = AVI_HEADER_LEN; // allot space for AVI header
  prepAviIndex();
  lastFlushMs = millis();
}

static inline bool doMonitor() {
  // pace the zone checks: 1 frame in N so they run at moveStartChecks per second while
  // watching for motion, dropping to moveStopChecks per second while a recording is open -
  // there a check only feeds the stop timer, and its ~13ms SCCB cost competes with saving
  // frames (measured 1.4fps at 5 checks/s, HD q6, Busy 100%)
  static uint16_t motionCnt = 0;
  uint16_t checkRate = FPS / (isCapturing ? moveStopChecks : moveStartChecks);
  if (!checkRate) checkRate = 1;
  if (++motionCnt / checkRate) motionCnt = 0; // time to check for motion
  return !(bool)motionCnt;
}

// geometry the sensor is being switched away from, so in flight frames can be recognised
static uint16_t staleWidth = 0, staleHeight = 0;
static uint8_t flushFrames = 0; // bounded, so an unexpected size cannot wedge the task
static uint32_t staleFrameCnt = 0; // reported by dumpMotionStats, so this stays visible

uint16_t jpegWidth(const uint8_t* buf, size_t len) {
  // Width the encoder actually wrote, read from the JPEG SOF header.
  // fb->width cannot be used for this: the driver fills the frame descriptor from its
  // current configuration, so immediately after set_framesize() it reports the NEW size
  // while the buffer still holds a frame captured at the OLD one. Measured on hardware -
  // a 9150 byte buffer described as 1920x1080, where a real FHD frame is ~82000 bytes.
  // Returns 0 if no SOF is found, which callers treat as "unknown, do not discard"
  for (size_t i = 2; i + 9 < len && i < 1024; ) {
    if (buf[i] != 0xFF) { i++; continue; }
    uint8_t m = buf[i + 1];
    if (m == 0xC0 || m == 0xC1 || m == 0xC2) return (buf[i + 7] << 8) | buf[i + 8];
    if (m == 0xD8 || m == 0xD9 || (m >= 0xD0 && m <= 0xD7)) { i += 2; continue; }
    i += 2 + ((buf[i + 2] << 8) | buf[i + 3]);
  }
  return 0;
}

static bool detectionActive() {
  // Zone detection runs in every state - recording, live view and NVR included - because a
  // check is a handful of SCCB reads with no frame decode and no resolution change. Only
  // dashcam mode and a sensor without the zone grid opt out. The sensor now always sits at
  // the user's capture resolution: the old VGA detection round-trip, its transition-frame
  // flushing on every recording, and the decode pixel cap are all gone with the decoder
  if (!useMotion || dashCamOn) return false;
  return zoneDetectOK;
}

static bool idleRateActive = false; // idle throttle holds the sensor at idleFps - idleThrottle()

static inline uint8_t desiredFPS(framesize_t forFS) {
  // the sensor always runs the capture size now, at whatever rate the user chose,
  // which is what captureFPS preserves across playback and boot - unless the idle
  // throttle holds it down while nothing needs frames
  return idleRateActive ? (uint8_t)idleFps : captureFPS;
}

// The lowest line length the sensor will honour. The driver programs HTS 2644 for XGA and HD
// but 2060 for VGA and SVGA on the same binned window, so 2644 is simply conservative rather
// than required. Measured: frame rate tracks HTS exactly down to 2060, then the sensor stops
// following - asking for 1900 came back as an effective 2069, and below about 1800 it produced
// no frames at all. The floor is per line, so it holds regardless of VTS: it back-solved to
// 2049 at VTS 984 and 2069 at VTS 744. The datasheet's 1296 for these modes is unreachable
#define HTS_FLOOR 2060

static void applyHtsFloor(sensor_t* s) {
  // Worth 19.2 -> 24.5fps at 1280x960 and 25.3 -> 31.9 at HD, for one register pair.
  //
  // Binned sizes only, and deliberately so. Full resolution has since been measured and is a
  // different case: at FHD the sensor reads 2624 real columns per line against HTS 2844, so
  // there is almost nothing spare. It works down to 2700 (5.8 -> 6.2fps) and is stone dead at
  // 2650 - not degraded frames, no frames at all, and no still either. A 6% gain sitting 1.9%
  // above a hard cliff, measured on one sensor at one temperature, is a bad trade, so the
  // full resolution sizes are left exactly as the driver set them. 2060 is safe here for the
  // opposite reason: it is the driver's own choice for VGA and SVGA on this same binned
  // window, already proven across the product, and XGA and HD were simply set high for no
  // reason. VTS has no slack at all at full resolution - dropping FHD from 1488 to 1340
  // stopped frame output entirely, and restoring it recovered immediately.
  //
  // DO NOT reduce VTS at full resolution to chase frame rate. It looks like a free doubling
  // and is not. FHD at VTS 1480 instead of 1488 reports 11.7fps instead of 5.8, and SXGA at
  // 1956 instead of 1968 reports 9.4 instead of 4.7 - both reproducible, both with valid
  // headers. The frames are corrupt: each carries only about half the horizontal pixels,
  // duplicated side by side with a seam down the middle. The rate halves because the sensor
  // is doing half the work, having failed to complete a full resolution line readout in the
  // shortened frame. Nothing automated catches this - ffmpeg decoded all 177 frames of a test
  // recording without error, every SOF read 1920x1080, header dimensions agreed at both
  // offsets, frameCnt agreed, and Bad frames discarded stayed 0. It was found by looking at a
  // frame. Any change to sensor timing registers needs a human to view an actual image
  //
  // The FHD lever turned out to be neither of these, and is now applyCropWindow() below: the
  // driver never crops, so FHD was reading 2624x1472 off the array and downscaling it to
  // 1920x1080. Cropping the window to 1984x1112 and turning the scaler off makes both HTS and
  // VTS legitimately shorter, and took it from 5.9 to 10.7fps with clean frames. Setting the
  // window and VTS alone kills output because the scaler is still enabled with nothing left to
  // scale - the missing register was 0x5001[5], not the 0x5600-0x5606 ratio block, which the
  // driver never writes at all because the hardware derives the ratio from the sizes
  if (s == NULL || s->set_reg == NULL || s->get_reg == NULL) return;
  int xInc = s->get_reg(s, 0x3814, 0xFF); // [7:4] odd increment, [3:0] even, ratio is the mean
  if (xInc < 0) return;
  if ((((xInc >> 4) & 0x0F) + (xInc & 0x0F)) / 2 < 2) return; // not binned
  int hts = (s->get_reg(s, 0x380C, 0xFF) << 8) | s->get_reg(s, 0x380D, 0xFF);
  if (hts <= HTS_FLOOR) return; // already there, which is the case for VGA and below
  s->set_reg(s, 0x380C, 0xFF, (HTS_FLOOR >> 8) & 0xFF);
  s->set_reg(s, 0x380D, 0xFF, HTS_FLOOR & 0xFF);
  int got = (s->get_reg(s, 0x380C, 0xFF) << 8) | s->get_reg(s, 0x380D, 0xFF);
  if (got != HTS_FLOOR) LOG_WRN("HTS floor wrote %d but reads back %d", HTS_FLOOR, got);
  else LOG_VRB("HTS %d -> %d", hts, HTS_FLOOR);
}

static int senReg16(sensor_t* s, int reg) {
  // big endian register pair straight off the sensor, -1 if either half fails
  int hi = s->get_reg(s, reg, 0xFF);
  int lo = s->get_reg(s, reg + 1, 0xFF);
  return (hi < 0 || lo < 0) ? -1 : ((hi << 8) | lo);
}

static bool senWrite16(sensor_t* s, int reg, int val) {
  // write a register pair and confirm it, since a rejected SCCB write is otherwise silent
  s->set_reg(s, reg, 0xFF, (val >> 8) & 0xFF);
  s->set_reg(s, reg + 1, 0xFF, val & 0xFF);
  return senReg16(s, reg) == val;
}

static int cropPreScaleW(framesize_t fs) {
  // The ISP INPUT width applyCropWindow() should aim the readout at, or 0 for "the output
  // width", which makes the scaler a no-op and the readout a straight 1:1 crop. Only the two
  // wide 1080p variants want an input larger than their output; every other size keeps the
  // original behaviour by returning 0 here.
  //
  // Both values are exact multiples of the 1920 output (1.2x and 1.333x) so the height derived
  // from them lands on a whole number and the scaler ratio is IDENTICAL in both axes. An
  // unequal ratio would come back as a subtly stretched image that every automated check
  // passes - the same class of failure as the split-frame trap, and just as invisible
  if (fs == FS_FHDMID) return 2304;   // -> 2304x1296 in, 1920x1080 out
  if (fs == FS_FHDFULL) return 2560;  // -> the driver's own pre-scale, so nothing is written
  return 0;
}

static void applyCropWindow(sensor_t* s, framesize_t forFS) {
  // The full resolution counterpart to applyHtsFloor(), and the answer to the FHD problem.
  //
  // Datasheet 4.2: frame rate follows the ISP input size - the pixels actually read off the
  // array - not the output size. "Typically, the larger the ISP input size is, the less
  // maximum frame rate can be reached". The esp32-camera driver never crops: set_framesize()
  // programs the window from a fixed ratio_table row and leaves the ISP scaler to shrink
  // whatever that reads. Every 16:9 size therefore reads 2624x1472 off the array, so FHD was
  // spending 2844x1488 clocks to emit 1920x1080. Table 2-1 defines 1080p the other way round,
  // as "cropping from full resolution", and cropping is what this does: read only the pixels
  // that are wanted, turn the scaler off, and shorten the frame to match.
  //
  // Measured on COM4, prediction first at every step: 5.9fps stock, 7.7 cropped at the stock
  // HTS against 7.8 predicted, then 8.5 / 9.5 / 10.7 at HTS 2600 / 2300 / 2060 against 8.5 /
  // 9.6 / 10.8 predicted. 0 bad frames throughout, every frame decoding, and frames extracted
  // and looked at after each change - which is not optional here, see the warning below.
  //
  // It costs field of view. FHD now reads 1984 of the array's 2624 columns, so framing is
  // about 1.29x tighter than it was. That is inherent to the method and is what the datasheet
  // mode does too. It also costs light: a 1.82x shorter frame is 1.82x less maximum exposure
  if (s == NULL || s->set_reg == NULL || s->get_reg == NULL) return;
  int xInc = s->get_reg(s, 0x3814, 0xFF);
  if (xInc < 0) return;
  if ((((xInc >> 4) & 0x0F) + (xInc & 0x0F)) / 2 >= 2) return; // binned - applyHtsFloor's job

  int xSt = senReg16(s, 0x3800), ySt = senReg16(s, 0x3802);
  int xEnd = senReg16(s, 0x3804), yEnd = senReg16(s, 0x3806);
  int xOff = senReg16(s, 0x3810), yOff = senReg16(s, 0x3812);
  if (xSt < 0 || ySt < 0 || xOff < 0 || yOff < 0 || xEnd <= xSt || yEnd <= ySt) return;
  int haveW = xEnd - xSt + 1, haveH = yEnd - ySt + 1;
  // datasheet figure 4-3: pre-scaling size is the input less twice the offset, so this is the
  // input the crop has to leave behind. cropPreScaleW() picks it: the output size for the 1:1
  // crop that makes the scaler a no-op, or a LARGER pre-scale for the wide 1080p variants,
  // which keep the scaler and buy field of view with frame rate
  int preW = cropPreScaleW(forFS), preH;
  if (preW > 0) preH = preW * frameData[forFS].frameHeight / frameData[forFS].frameWidth;
  else {
    preW = frameData[forFS].frameWidth;
    preH = frameData[forFS].frameHeight;
  }
  bool scalerNoOp = (preW == frameData[forFS].frameWidth && preH == frameData[forFS].frameHeight);
  int needW = preW + 2 * xOff;
  int needH = preH + 2 * yOff;
  // bigger than the window means there is nothing to crop, which is what leaves QSXGA, 5MP,
  // QHD and WQXGA alone - at those the output already is the whole array
  if (needW > haveW || needH > haveH) return;
  if (needW == haveW && needH == haveH) {
    // Nothing to crop. For QSXGA and friends that is because the output already IS the whole
    // array; for FHDFULL it is deliberate - it asks for the driver's own pre-scale, so the
    // window, HTS, VTS and scaler enable are all left exactly as set_framesize() wrote them.
    // Say so, because "the tuner wrote nothing" is otherwise indistinguishable from a bug
    LOG_VRB("Crop %s: readout %dx%d already matches the wanted pre-scale - driver geometry kept",
      frameData[forFS].frameSizeStr, haveW, haveH);
    return;
  }
  // keep the start even so the Bayer phase is unchanged, or the colours come out wrong
  int newXSt = (xSt + (haveW - needW) / 2) & ~1;
  int newYSt = (ySt + (haveH - needH) / 2) & ~1;

  if (!senWrite16(s, 0x3800, newXSt) || !senWrite16(s, 0x3802, newYSt)
   || !senWrite16(s, 0x3804, newXSt + needW - 1) || !senWrite16(s, 0x3806, newYSt + needH - 1)) {
    LOG_WRN("Crop window did not take for %s - sensor left as the driver set it", frameData[forFS].frameSizeStr);
    return;
  }
  // VTS is the rows read plus 16 lines of blanking at full resolution, measured across every
  // size. Shortening it without cropping first is the corruption trap warned about below
  if (!senWrite16(s, 0x380E, needH + 16)) LOG_WRN("Crop VTS %d did not take", needH + 16);
  // 0x5001[5] is the scale enable, and the only scaler register the driver ever writes. The
  // ratio itself is derived from window, offsets and output size, so 0x5600-0x5606 are not
  // involved - confirmed by datasheet 5.9 and table 5-1, which give 0x501E[6] as "scale ratio
  // manual enable" with a default of 0, meaning XSC/YSC are consulted only in manual mode.
  // With pre-scale now equal to the output, leaving the scaler on would be a 1:1 rescale.
  //
  // What this does NOT fix is LENC. Datasheet 5.2 divides the image into 5x5 blocks for the
  // BR channel and 6x6 for G, with the step held as a reciprocal in 0x5842-0x5849, and the
  // driver never writes them. That is correct for stock operation, where the pre-scale size is
  // constant per aspect ratio, and binned sizes are covered because 5.2 says LENC is subsample
  // aware. Cropping is the first thing that changes the pre-scale size - here from 2560x1440 to
  // 1920x1080 - so the correction grid lands at the wrong radial coordinates and the edges pick
  // up a colour and luminance error. Untested fix: scale each register by 2560/1920
  //
  // Only the 1:1 crop turns the scaler off. The wide 1080p variants deliberately leave it
  // enabled - a larger pre-scale downsized to 1920x1080 is the whole point of them - so their
  // LENC grid is closer to correct than the 1:1 crop's, not further from it: FHDFULL keeps the
  // driver's own pre-scale exactly, and FHDMID shifts it by 1.11x against FHDNARROW's 1.33x
  if (scalerNoOp) {
    int isp01 = s->get_reg(s, 0x5001, 0xFF);
    if (isp01 >= 0 && (isp01 & 0x20)) s->set_reg(s, 0x5001, 0xFF, isp01 & ~0x20);
  }
  // A narrower window needs fewer clocks per line, but full resolution has TWO floors and
  // the binding one is not the readout. HTS 2060 on a 1984 column crop reads out perfectly -
  // clean frames, exact rates - while the AEC goes blind: below ~2200 the sensor's Y-AVERAGE
  // statistics engine stops updating (YAVG pins near its last value while the zone grid at
  // 0x5691+ still tracks the scene), the AEC servos the pinned YAVG straight into its
  // deadband and holds exposure ~4.5x low. Found 27 Aug 2026 on a lit flat field: HD read
  // 4.61ms exposure, cropped FHD 1.29ms at the same gain, image mean 24/255. Bisected:
  // HTS 2200 clean, 2100 partially diverged, 2080/2064/2060 fully blind - every automated
  // frame check passed throughout, only the exposure showed it. 2200 keeps ~5% margin above
  // the 2100 cliff edge. Binned sizes are untouched (their stats run fine at 2060 - HD
  // agreed zones==YAVG on the same scene)
  //
  // The OTHER floor is the readout itself, and needW + 76 is where it sits. That 76 is not a
  // derivation, it is the measured full-width cliff read backwards: at a 2624 column readout
  // HTS 2700 worked and 2650 produced no frames at all, and 2700 - 2624 is 76. It is applied
  // at every width because it is the only number there is, but at any width OTHER than 2624 it
  // is an extrapolation. FHDMID lands on 2444 that way. If a wide variant ever delivers no
  // frames at all, raise HTS before suspecting anything else
  #define HTS_FLOOR_FULLRES 2200
  int htsFloor = needW + 76;
  if (htsFloor < HTS_FLOOR_FULLRES) htsFloor = HTS_FLOOR_FULLRES;
  int hts = senReg16(s, 0x380C);
  if (hts > htsFloor && !senWrite16(s, 0x380C, htsFloor)) LOG_WRN("Crop HTS %d did not take", htsFloor);
  LOG_VRB("Cropped %s to %dx%d at %d,%d, VTS %d, HTS %d, scaler %s", frameData[forFS].frameSizeStr,
    needW, needH, newXSt, newYSt, needH + 16, (hts > htsFloor) ? htsFloor : hts,
    scalerNoOp ? "off (1:1)" : "on (downsizing)");
}

static void setOutputSize(sensor_t* s, uint16_t w, uint16_t h) {
  // Retarget the ISP scaler output without touching the window, binning or line timing.
  // 0x3808/0x3809 and 0x380A/0x380B are the DVP output size; everything upstream of them is
  // left as the base size set it, which is the point - frame rate follows HTS and VTS, so
  // this changes what comes out without changing how long a frame takes to produce
  if (s == NULL || s->set_reg == NULL) {
    LOG_WRN("Custom frame size needs set_reg, which this sensor does not provide");
    return;
  }
  s->set_reg(s, 0x3808, 0xFF, (w >> 8) & 0x0F);
  s->set_reg(s, 0x3809, 0xFF, w & 0xFF);
  s->set_reg(s, 0x380A, 0xFF, (h >> 8) & 0x0F);
  s->set_reg(s, 0x380B, 0xFF, h & 0xFF);
  // read back rather than trust the writes - a rejected SCCB write is silent, and the
  // symptom downstream would be frames of an unexpected size with no clue why
  int gotW = (s->get_reg(s, 0x3808, 0xFF) << 8) | s->get_reg(s, 0x3809, 0xFF);
  int gotH = (s->get_reg(s, 0x380A, 0xFF) << 8) | s->get_reg(s, 0x380B, 0xFF);
  if (gotW != w || gotH != h) LOG_WRN("Output size override wrote %ux%u but reads %dx%d", w, h, gotW, gotH);
  else LOG_VRB("Output size overridden to %ux%u", w, h);
}

// defined further down, next to camClocks() which it needs for the line time
static void applyAecLimits(sensor_t* s);
static int senLineFactor(sensor_t* s);

int tunedFps = 0; // config: fps choices drive the sensor's own timing on the in-spec PLL
volatile bool retimePending = false; // fps changed - capture task retimes on the next frame
// a size or rate chosen MID-RECORDING is deferred here rather than applied, so one AVI keeps
// one geometry and one timing throughout - its header carries a single WxH and fps, and
// settleSensor() used to follow fsizePtr on the next frame regardless of what was consuming
// the stream. Applied by settleSensor() from a quiet state when the recording closes
volatile int pendingFS = -1;
volatile int pendingFPS = -1;

static bool pickPll(float targetMHz, int* mulOut, int* sysDivOut) {
  // mul/sys_div pair for the closest PIXCLK to targetMHz on the pre_div 3 tree at XCLK
  // 20MHz: PIXCLK MHz = mul x 2/3 / sys_div, which is the full chain
  // (XCLK/pre_div x mul / sys_div x 2/5) / 4 simplified for pre_div 3 - it is only valid on
  // that tree, and applyTunedTiming always programs it. Selection window: mul 75-150 keeps
  // VCO in the datasheet's 500-1000 best range (even values only above 127 per section
  // 7.1), PIXCLK capped at the verified 80MHz (image cliff 88-92). The window spans a full 2x, so
  // consecutive sys_div steps can never leave an unreachable gap; smallest workable
  // sys_div = lowest in-range VCO. Shared by applyScalerClock and the exposure-first
  // low-fps regime of applyTunedTiming
  for (int sd = 1; sd <= 15; sd++) {
    int m = (int)(targetMHz * 1.5f * sd + 0.5f);
    if (m > 127) m &= ~1; // even only above 127
    if (m < 75 || m > 150) continue; // VCO outside 500-1000
    if (m * 2.0f / 3 / sd > 80.05f) continue; // above the verified PIXCLK ceiling
    *mulOut = m;
    *sysDivOut = sd;
    return true;
  }
  return false;
}

static void applyTunedTiming(sensor_t* s, framesize_t fs) {
  // The generalisation of the retired hdProfile/fhdProfile pair: at every video size, the
  // fps setting IS the sensor's frame timing. Two regimes: high fps computes VTS on the
  // 80MHz PLL; low fps (where that VTS would cross the 1964-row AEC engine cap) holds VTS
  // at the cap and slows the clock instead, so the exposure ceiling keeps tracking the
  // frame period (exposure-first, 28 Aug). Replaces "the timer asks and the sensor
  // delivers whatever the driver's clock tree happens to produce" with timing that agrees
  // by construction.
  //
  // PLL: pre_div 3 / sys_div 1 / mul 120 at XCLK 20MHz -> REFIN 6.67MHz, VCO 800MHz (the
  // section 2.5 maximum, exactly in spec against the driver's nominal 2000) and PIXCLK 80MHz
  // (table 8-5 typ 48 max 96, measured image cliff 88-92). Hardware-verified at HD (30.0fps
  // dead-on) and FHD (15.004 counted); the per-size sweep extends that proof.
  //
  // VTS = PIXCLK / (HTS x lineFactor x fps), the hardware-proven model - lineFactor is 2 at
  // full resolution where a line costs 2 x HTS clocks. Clamped UPWARD only: never below the
  // VTS already in force (the driver's own value binned, the crop's rows+16 full res),
  // because raising VTS is blanking and safe while lowering it is the proven split-frame
  // trap - seamed half-width frames that decode cleanly and pass every automated check.
  // applyAecLimits() runs after this and derives banding steps and the exposure ceiling from
  // whatever landed, so they stay consistent by construction. The 40MHz in-spec-everywhere
  // variant (retired hdProfile 2) remains reproducible at runtime via camPll if the zombie
  // investigation ever needs it again - see BOARD_TESTING.md.
  //
  // The sensor is tuned fpsOverdrive() above the request while the frame timer stays at the
  // request itself: the capture task takes one frame per timer tick off a GRAB_LATEST
  // queue, so with a surplus every tick finds a fresh frame and the delivered rate IS the
  // timer rate. The measured 0.7-1.3% delivery deficit came from ticks that found no frame
  // when sensor and timer ran identical rates. The surplus also feeds the AEC/AWB more frames
  // than get recorded; the cost is 5% off the maximum exposure. Ceiling requests get no
  // overdrive (the VTS floor caps them) and deliver ~1% low, by design
  uint8_t fps = desiredFPS(fs);
  if (fps < 1) fps = 1;
  int hts = senReg16(s, 0x380C);
  int vtsNow = senReg16(s, 0x380E);
  int lf = senLineFactor(s);
  if (hts < 1 || vtsNow < 8) {
    LOG_WRN("Tuned timing: HTS %d / VTS %d readback implausible - not retimed", hts, vtsNow);
    return;
  }
  // The VTS floor cannot be "whatever VTS is there now" - a previous tuned choice may have
  // raised it, and clamping against it would make rates monotonically decreasing (found on
  // hardware: after 10fps at FHD set VTS 1941, a 15fps request was wrongly refused). The
  // floor is derived from the window registers instead, which the tuning never touches:
  // rows read + 16 blanking lines at full resolution, rows/2 + 8 binned - matching every
  // measured driver value (744 HD, 984 VGA/XGA, 1128 cropped FHD = 1112 rows + 16, 1968 QSXGA)
  int ySt = senReg16(s, 0x3802), yEnd = senReg16(s, 0x3806);
  int vtsFloor = vtsNow; // fallback: never lower what is there if the window is unreadable
  if (ySt >= 0 && yEnd > ySt) {
    int rows = yEnd - ySt + 1;
    vtsFloor = (lf == 2) ? rows + 16 : rows / 2 + 8;
  }
// VTS-4 == the 1964-row AEC engine cap (applyAecLimits): rows above this are blanking
// the AEC can never convert to exposure
#define AEC_VTS_CEIL 1968
  int vts = (int)(80e6f / ((float)hts * lf * fps * fpsOverdrive(fps)));
  int mul = 120, sysDiv = 1; // PIXCLK 80MHz, the high-fps default
  if (vts > AEC_VTS_CEIL) {
    // Exposure-first low-fps regime. VTS-only stretching froze the exposure ceiling at
    // 1964 x tROW once VTS crossed the engine cap (HD: 50.6ms for everything under
    // ~19fps). Hold VTS at the cap instead and put the rest of the frame period into
    // line TIME by slowing the clock: min(VTS-4, 1964) x tROW then tracks ~the whole
    // frame period at any rate (95ms at HD 10fps, ~400ms at 1fps).
    // HTS is left strictly alone: the binned line cost flips from 1x to 2x HTS above
    // ~2300 (VSYNC-counted 28 Aug: exact 80MHz at HTS 2277, exactly-half 40MHz at 2644+,
    // chaotic fractional rates 2340-2500), so programmed HTS above 2277 is never valid
    // on a binned size. The clock path has no such cliff - scaler sizes run it 10-80MHz
    // and HD ran the retired 40MHz profile
    int targetVts = (AEC_VTS_CEIL > vtsFloor) ? AEC_VTS_CEIL : vtsFloor;
    float targetMHz = (float)fps * fpsOverdrive(fps) * hts * lf * targetVts / 1e6f;
    if (targetMHz >= 10.0f) {
      // pickPll cannot fail for 10-80 (the mul window spans 2x per sys_div step), but
      // if it ever does the mul 120 default stands and the naive VTS keeps the rate
      // correct with the old capped-exposure behavior
      if (pickPll(targetMHz, &mul, &sysDiv)) vts = targetVts;
    } else {
      // below the verified 10MHz PIXCLK floor (fps 1-2 at most sizes): floor the clock
      // and grow VTS past the cap. The exposure ceiling is capped at 1964 rows, but
      // tROW is now so long the ceiling still lands in the hundreds of ms
      mul = 76; sysDiv = 5; // 10.13MHz, the verified floor combo (see applyScalerClock)
      vts = (int)((mul * 2e6f / 3 / sysDiv) / ((float)hts * lf * fps * fpsOverdrive(fps)));
    }
  }
  if (vts < vtsFloor) {
    float ceilFps = 80e6f / ((float)hts * lf * vtsFloor);
    // an overdrive-only clamp is expected near the ceiling; only a request the sensor
    // itself cannot reach is worth a warning
    if ((float)fps > ceilFps) LOG_WRN("Tuned timing: %ufps is beyond the %s ceiling %.1f - delivering the ceiling",
      fps, frameData[fs].frameSizeStr, ceilFps);
    vts = vtsFloor;
  }
  if (vts > 0xFFFF) vts = 0xFFFF; // 16 bit register; ~0.02fps floor at HD, never plausible
  s->set_reg(s, 0x3037, 0xFF, 0x03); // pre_div 3, root_2x 0
  s->set_reg(s, 0x3035, 0xFF, (sysDiv << 4) | 0x01); // [7:4] sys_div, [3:0] MIPI div stays 1
  s->set_reg(s, 0x3036, 0xFF, mul);
  delay(50); // let the PLL relock, same settle setCamPll() uses
  int gotMul = s->get_reg(s, 0x3036, 0xFF);
  if (gotMul != mul) {
    LOG_WRN("Tuned timing: PLL mul wrote %d but reads %d - VTS left alone", mul, gotMul);
    return;
  }
  float pixClkMHz = mul * 2.0f / 3 / sysDiv;
  if (!senWrite16(s, 0x380E, vts)) LOG_WRN("Tuned timing: VTS %d did not read back", vts);
  else {
    int expLines = vts - 4;
    if (expLines > 1964) expLines = 1964; // the AEC engine cap, applyAecLimits
    LOG_INF("Tuned timing %s: PIXCLK %.2fMHz, HTS %d x%d, VTS %d -> sensor %.2ffps for request %u, max exposure %.0fms",
      frameData[fs].frameSizeStr, pixClkMHz, hts, lf, vts, pixClkMHz * 1e6f / ((float)hts * lf * vts), fps,
      (float)expLines * hts * lf * 1000.0f / (pixClkMHz * 1e6f));
  }
}

static bool scalerClockSize(framesize_t fs) {
  // the sizes whose tuned fps is set by moving the CLOCK at the driver's own VTS, because
  // raising VTS on a real ISP-scaler size halves delivery (at any clock, threshold
  // state-dependent - measured 27/28 Aug, see BOARD_TESTING.md "Per-size clock assessment")
  // while fps-via-PLL at driver VTS ran exact at five rates over a 20-61 MHz sweep.
  //
  // VGA and QVGA are the two measured members of the OV5640 UI set; the remaining stock scaler
  // sizes stay untuned. FHDMID and FHDFULL join them because turning the scaler on is exactly
  // what puts a size into this class - and the clock path costs them nothing, since
  // applyTunedTiming clamps VTS upward only (same ceiling either way) while a slower clock
  // lengthens every row, where VTS past 1968 buys no exposure at all. FHDNARROW stays on the
  // VTS path: its scaler is off, so the halving rule never applied to it
  return fs == FRAMESIZE_VGA || fs == FRAMESIZE_QVGA || fs == FS_FHDMID || fs == FS_FHDFULL;
}

static void applyScalerClock(sensor_t* s, framesize_t fs) {
  // The scaler-size counterpart of applyTunedTiming(): the fps setting is hit by choosing
  // the PLL multiplier and system divider while HTS and VTS stay at the driver's values
  // (2060 x 984 for VGA/QVGA). PIXCLK MHz = mul x 2/3 / sys_div on the pre_div 3 tree at
  // XCLK 20MHz, so fps = PIXCLK / (HTS x lf x VTS). Bench-verified: 29.925/24.990/19.894/
  // 14.965/9.949 counted against five computed targets (0.3% worst error), stills clean,
  // auto AEC healthy, QVGA 39.47 at the mul 120 ceiling.
  // Selection window: mul 75-150 keeps VCO in the datasheet's 500-1000 best range (even
  // values only above 127 per section 7.1), and PIXCLK is capped at the verified 80MHz -
  // the image cliff sits at 88-92. The window spans a full 2x, so consecutive sys_div
  // steps (worst ratio 2, at 1->2) can never leave an unreachable fps gap.
  uint8_t fps = desiredFPS(fs);
  if (fps < 1) fps = 1;
  int hts = senReg16(s, 0x380C);
  int vts = senReg16(s, 0x380E);
  int lf = senLineFactor(s);
  if (hts < 1 || vts < 8) {
    LOG_WRN("Scaler clock: HTS %d / VTS %d readback implausible - clock left alone", hts, vts);
    return;
  }
  // the sensor runs fpsOverdrive() above the request; the frame timer paces delivery at
  // the request itself (see applyTunedTiming - same scheme, clock instead of VTS)
  float plainMHz = (float)fps * hts * lf * vts / 1e6f; // the PIXCLK that matches the request exactly
  float targetMHz = plainMHz * fpsOverdrive(fps);
  // both ends are measured, not derived: 80 is the verified ceiling (image cliff 88-92);
  // below ~10 the sensor degrades - 8.1MHz ran grainy with a 2.5% rate sag and 6.1MHz
  // produced a solid false-colour frame with no scene content at all
  const float PIXCLK_FLOOR_MHZ = 10.0f;
  int mul = 0, sysDiv = 0;
  if (targetMHz > 80.0f || targetMHz < PIXCLK_FLOOR_MHZ) {
    // outside the achievable range: clamp, like the tuned path's ceiling clamp.
    // The floor combo is the verified 5fps point (PIXCLK 10.13, counted 4.961). Below it
    // the frame TIMER still paces the requested 1-4fps exactly - the sensor just free-runs
    // at ~5. An overdrive-only ceiling bust is expected and silent; only a request the
    // sensor itself cannot reach warns
    bool tooFast = targetMHz > 80.0f;
    mul = tooFast ? 120 : 76;
    sysDiv = tooFast ? 1 : 5;
    if (tooFast && plainMHz > 80.0f) LOG_WRN("Scaler clock: %ufps is beyond the %s range - delivering %.2ffps", fps,
      frameData[fs].frameSizeStr, (mul * 2e6f / 3 / sysDiv) / ((float)hts * lf * vts));
  } else if (!pickPll(targetMHz, &mul, &sysDiv)) {
    // unreachable given the 2x-wide mul window, but never leave the PLL unprogrammed
    mul = 120;
    sysDiv = 1;
    LOG_WRN("Scaler clock: no divider fit for %ufps - delivering the ceiling", fps);
  }
  s->set_reg(s, 0x3037, 0xFF, 0x03); // pre_div 3, root_2x 0
  s->set_reg(s, 0x3035, 0xFF, (sysDiv << 4) | 0x01); // [7:4] sys_div, [3:0] MIPI div stays 1
  s->set_reg(s, 0x3036, 0xFF, mul);
  delay(50); // let the PLL relock, same settle applyTunedTiming uses
  int gotMul = s->get_reg(s, 0x3036, 0xFF);
  if (gotMul != mul) {
    LOG_WRN("Scaler clock: PLL mul wrote %d but reads %d", mul, gotMul);
    return;
  }
  float pixClkMHz = mul * 2.0f / 3 / sysDiv;
  LOG_INF("Scaler clock %s: PIXCLK %.2fMHz (mul %d sys_div %d), HTS %d x%d, VTS %d -> sensor %.2ffps for request %u",
    frameData[fs].frameSizeStr, pixClkMHz, mul, sysDiv, hts, lf, vts,
    pixClkMHz * 1e6f / ((float)hts * lf * vts), fps);
}

static void applySensorTuning(sensor_t* s, framesize_t fs) {
  // Everything set_framesize() does not do for us. It reloads the whole register block, so all
  // of this is lost on every call and has to follow every one of them - which is the reason it
  // is gathered here rather than left inline: there are two callers, and the boot one silently
  // did none of it. A board that came up already on its configured size never went through
  // setSensorSize() at all, so it ran on the driver's own geometry until the size was changed
  // and changed back. Measured: HD booted at HTS 2644 and 25.2fps rather than 2060 and 32.3
  if (s == NULL) return;
  if (fs == FS_1280X960) setOutputSize(s, frameData[fs].frameWidth, frameData[fs].frameHeight);
  // mutually exclusive by design - the crop handles full resolution sizes and the HTS floor
  // the binned ones, each returning immediately when handed the other's case
  applyCropWindow(s, fs);
  applyHtsFloor(s);
  // the retiming must precede applyAecLimits(), which derives banding and the exposure
  // ceiling from whatever clock and VTS are in force by the time it reads them back - a
  // scaler clock change would otherwise leave stale banding steps (visible stripes,
  // measured at the E2 10fps step). maxTunedFPS == 0 still opts a size out entirely.
  // Two tuning mechanisms, split by how the size fails: real ISP-scaler sizes halve
  // delivery when VTS rises above the driver's value (at any clock - 27/28 Aug), so their
  // fps moves the CLOCK at fixed VTS; every other tuned size holds the 80MHz tree and
  // moves VTS. 1280X960's scaler pass is 1:1 and takes the VTS path by measurement
  if (tunedFps && videoSizeAllowed(fs) && frameData[fs].maxTunedFPS) {
    if (scalerClockSize(fs)) applyScalerClock(s, fs);
    else applyTunedTiming(s, fs);
  }
  // last, because it reads back the HTS, VTS and clock the steps above have settled
  applyAecLimits(s);
}

static framesize_t hwFrameSize(framesize_t fs) {
  // custom sizes have no framesize_t of their own, so the driver is given the base size and
  // applySensorTuning() rewrites the registers that differ
  if (fs == FS_1280X960) return FS_1280X960_BASE;
  if (fs == FS_FHDMID || fs == FS_FHDFULL) return FS_FHD_BASE;
  return fs;
}

void setSensorSize(framesize_t newFS) {
  // Switch the sensor between the detection size and the user's capture size.
  // Deliberately does not go through setFPS(): that writes saveFPS, which playback uses to
  // restore the browser's rate, so routing every transition through it would leave a
  // playback started while idle restoring the detection rate instead of the user's.
  // Must only be called from the capture task with no frame checked out - it hands every
  // queued buffer back to the driver
  if (newFS == sensorFS) return;
  sensor_t* s = esp_camera_sensor_get();
  if (s == NULL) return;
  uint32_t sTime = millis();
  // Remember the geometry we are leaving. Draining the queue is not enough on its own: a
  // frame already in flight in DMA still completes at the old size and gets queued after
  // the reconfigure, so the first frame out can be stale. Confirmed on hardware - frame 0
  // of an FHD recording was 640x480. processFrame() discards anything still matching this
  staleWidth = frameData[sensorFS].frameWidth;
  staleHeight = frameData[sensorFS].frameHeight;
  flushFrames = FB_CNT * 2; // every buffer, plus margin for one in flight per buffer
  // drain what the driver already holds at the old size before reconfiguring. Returning a
  // queued frame is immediate because it is already captured, so this costs far less than
  // reconfiguring first and then burning a frame interval per stale buffer
  esp_camera_return_all();
  if (s->set_framesize(s, hwFrameSize(newFS)) != ESP_OK) {
    LOG_WRN("Failed to switch sensor to %s", frameData[newFS].frameSizeStr);
    return;
  }
  applySensorTuning(s, newFS);
  sensorFS = newFS;
  FPS = desiredFPS(newFS);
  controlFrameTimer(true);
  LOG_VRB("Sensor -> %s @ %u fps in %lums", frameData[newFS].frameSizeStr, FPS, millis() - sTime);
}

static void settleSensor() {
  // keep the sensor, and the frame timer, matched to whatever state the device is in.
  // Skipped while playback is actually running, because playback borrows the global FPS for
  // its own pacing (playbackFPS() sets it, setFPS(saveFPS) restores it) and retuning the
  // timer underneath it would break that.
  // Must be isPlaying, not doPlayback. doPlayback means "a playable file is selected" - it
  // is set just by picking a file in the browser, and stopPlaying() only clears it inside
  // if (isPlaying), so a selection that never plays leaves it true indefinitely. Guarding on
  // it froze sensorFS for the rest of the session: the sensor stopped following fsizePtr
  if (isPlaying) return;
  if (!isCapturing && (pendingFS >= 0 || pendingFPS >= 0)) {
    // choices made mid-recording, applied now that the clip has closed. Size first - the
    // user's rate survives the switch, clamped to the new size's ceiling - then an
    // explicit rate choice on top of it
    if (pendingFS >= 0) {
      fsizePtr = pendingFS;
      if (captureFPS > fpsCeiling((framesize_t)fsizePtr)) captureFPS = fpsCeiling((framesize_t)fsizePtr);
      pendingFS = -1;
    }
    if (pendingFPS >= 0) {
      captureFPS = pendingFPS;
      pendingFPS = -1;
      if (tunedFps) retimePending = true;
    }
  }
  framesize_t want = (framesize_t)fsizePtr; // the sensor always follows the capture size
  if (want != sensorFS) {
    setSensorSize(want); // runs applySensorTuning, so any pending retime is covered
    retimePending = false;
  } else if (retimePending && !isCapturing) {
    // fps changed at a constant size. Only this task may retime the sensor (the web task
    // cannot - the capture may hold a frame mid-flight), which is why the handler just
    // raises the flag. On VTS-tuned sizes this is a blanking-only change; on the
    // clock-tuned scaler sizes it is a PLL write with a 50ms relock - the AEC-band gate
    // in zoneMotion absorbs the blip. Deferred while capturing so a recording keeps one timing
    retimePending = false;
    applySensorTuning(esp_camera_sensor_get(), (framesize_t)sensorFS);
    if (FPS != desiredFPS(sensorFS)) {
      FPS = desiredFPS(sensorFS);
      controlFrameTimer(true);
    }
  } else if (FPS != desiredFPS(want)) {
    // size is already right but the rate is not - happens after a playback restores the
    // browser's rate while the sensor is sitting at the detection size
    FPS = desiredFPS(want);
    controlFrameTimer(true);
  }
}

const char* sensorStateStr() {
  // what the sensor is doing right now, for the web UI - it changes size on its own now,
  // and nothing else reports that
  static char stateBuf[32];
  const char* fsStr = frameData[sensorFS].frameSizeStr;
  if (isCapturing) snprintf(stateBuf, sizeof(stateBuf), "%s (%s)", dashCamOn ? "DashCam" : "Recording", fsStr);
  else if (nvrActive()) snprintf(stateBuf, sizeof(stateBuf), "NVR - recording paused (%s)", fsStr);
  else if (viewerActive()) snprintf(stateBuf, sizeof(stateBuf), "Live view (%s)", fsStr);
  else if (useMotion) snprintf(stateBuf, sizeof(stateBuf), "Detecting (%s)", fsStr);
  else snprintf(stateBuf, sizeof(stateBuf), "Idle (%s)", fsStr);
  return stateBuf;
}

bool videoSizeAllowed(uint8_t fsize) {
  // framesize_t is not ordered by pixel count - P_HD (0.92MP), P_3MP (1.33MP) and P_FHD
  // (2.07MP) all have higher enum values than FHD yet are no larger than it. Comparing
  // enum indexes would wrongly block all three, so compare pixels
  return (uint32_t)frameData[fsize].frameWidth * frameData[fsize].frameHeight
      <= (uint32_t)frameData[maxVideoFS].frameWidth * frameData[maxVideoFS].frameHeight;
}

// Zone detector shared state - file scope so dumpMotionStats() and zoneStatsJson() can
// report it alongside the capture task that owns it
static uint8_t zref[16]; // reference zone grid from the last clean check
static bool zrefValid = false;
static uint32_t zconsecCnt = 0; // consecutive tripped checks, reported by zoneStatsJson
static int zonePeakDelta = 0; // largest in-mask per-zone delta since the last stats dump
static int zonePeakMoved = 0; // most zones moved in one check since the last stats dump
static bool haveMotion = false; // detector output, refreshed at the check rate

static inline int zoneThresh() {
  // the Motion Sensitivity slider (1-10) maps onto the per-zone luminance delta that counts
  // as movement. Default motionVal 8 -> 4, near the 5 the tier-1 pre-filter proved (5x the
  // measured +-1 zone noise floor on a static scene)
  int t = 12 - (int)motionVal;
  return (t < 2) ? 2 : t;
}

static int zoneViewToPhys(int i) {
  // zoneMask is VIEW order - the grid the user clicks matches the image they see. The zone
  // grid follows the readout, so hmirror/vflip are undone here from the live sensor state
  // and a flip never silently moves the masked region. Bench-verified polarity: if the
  // corner-wave test shows the stats window is pre-flip instead, swap this to identity
  sensor_t* s = esp_camera_sensor_get();
  int r = i / 4, c = i % 4;
  if (s != NULL) {
    if (s->status.hmirror) c = 3 - c;
    if (s->status.vflip) r = 3 - r;
  }
  return r * 4 + c;
}

static uint16_t zoneMaskPhys() {
  // the view-order mask translated to the sensor's zone order
  uint16_t m = 0;
  for (int v = 0; v < 16; v++)
    if ((zoneMask >> v) & 1) m |= 1 << zoneViewToPhys(v);
  return m;
}

void dumpMotionStats() {
  // on demand; the measurement window resets on each dump
  uint16_t checkRate = FPS / moveStartChecks;
  if (!checkRate) checkRate = 1;
  float perSec = (float)FPS / checkRate;
  LOG_INF("******** Motion detection stats ********");
  LOG_INF("Zone detection %s, sensor %s (%ux%u), app FPS %u", zoneDetectOK ? "available" : "UNAVAILABLE - needs the OV5640 zone grid",
    frameData[sensorFS].frameSizeStr, frameData[sensorFS].frameWidth, frameData[sensorFS].frameHeight, FPS);
  LOG_INF("Capture size: %s, state: %s", frameData[fsizePtr].frameSizeStr, sensorStateStr());
  LOG_INF("moveStartChecks %d -> check every %u frame(s) = %0.1f checks/sec (%d/sec while recording)",
    moveStartChecks, checkRate, perSec, moveStopChecks);
  if (mCheckCnt) {
    float perCheck = (float)mTimeTot / mCheckCnt / 1000.0f; // us -> ms
    LOG_INF("%lu checks, %lu ms total, %0.2f ms per check", mCheckCnt, mTimeTot / 1000, perCheck);
    LOG_INF("Detection duty cycle: %0.1f%% of wall clock", perCheck * perSec / 10.0f);
  } else LOG_WRN("No checks recorded - motion off or night gated");
  // for tuning: the peaks are the closest any check came to tripping, so peaks well under
  // the thresholds mean the sensitivity is too low to ever fire on that scene
  LOG_INF("Thresholds: zone delta >= %d (motionVal %.0f), zones >= %d, %d consecutive checks, mask 0x%04X",
    zoneThresh(), motionVal, zoneCount, detectMotionFrames, zoneMask);
  LOG_INF("Peak seen: zone delta %d, zones moved %d", zonePeakDelta, zonePeakMoved);
  LOG_INF("Light level %u, night %s (lswitch %u)%s", lightLevel, nightTime ? "yes" : "no",
    nightSwitch, nightTime ? " - DETECTION SUSPENDED" : "");
  // any nonzero count here used to leak a frame buffer permanently, FB_CNT of them
  // being enough to wedge the capture task
  LOG_INF("Bad frames discarded: %lu (of %d buffers)", badFrameCnt, FB_CNT);
  // frames caught at the old size after a sensor switch. Nonzero is normal and healthy -
  // it means the guard is keeping them out of recordings and streams
  LOG_INF("Stale frames flushed after switch: %lu", staleFrameCnt);
  // settleSensor() should have reconciled these on the last frame, so a lasting mismatch
  // means it is not running - which is how a stuck guard silently froze the sensor once
  // already. Every symptom of that bug was "sensorFS is not what the state implies", and
  // nothing asserted it
  if (!isPlaying && sensorFS != (framesize_t)fsizePtr) LOG_WRN("Sensor is %s but capture size is %s - settleSensor not reconciling",
    frameData[sensorFS].frameSizeStr, frameData[fsizePtr].frameSizeStr);
  LOG_INF("***************************************");
  mTimeTot = mCheckCnt = badFrameCnt = staleFrameCnt = 0; // reset the measurement window
  zonePeakDelta = zonePeakMoved = 0;
}

void keepFrame(camera_fb_t* fb) {
  // keep required frame for external server alert
  if (fb->len < maxAlertBuffSize && alertBuffer != NULL) {
    memcpy(alertBuffer, fb->buf, fb->len);
    alertBufferSize = fb->len;
  }
}

static const uint8_t zeroFiller[3] = {0, 0, 0}; // DWORD alignment padding for AVI chunks

static void bufferedAviWrite(const uint8_t* data, size_t len) {
  // append to the SD write buffer, flushing whenever a full SD_WRITE_SIZE block is ready.
  // every aviFile.write() is therefore SD_WRITE_SIZE and lands on a SD_WRITE_SIZE boundary,
  // as openAvi() reserves the AVI header space inside the first buffer
  while (len >= SD_WRITE_SIZE - highPoint) {
    size_t take = SD_WRITE_SIZE - highPoint;
    memcpy(sdWriteBuf + highPoint, data, take);
    aviFile.write(sdWriteBuf, SD_WRITE_SIZE);
    data += take;
    len -= take;
    highPoint = 0;
  }
  memcpy(sdWriteBuf + highPoint, data, len);
  highPoint += len;
}

#if INCLUDE_AUDIO
static uint32_t writeAudioChunk(size_t minLen) {
  // append accumulated audio as an 01wb chunk once there is at least minLen of it, and
  // return the ms spent writing it so the caller can include it in the SD storage total.
  // Interleaving here replaces the temporary WAV file that used to be read back off
  // SD and rewritten into the AVI when the recording closed
  uint8_t* audBuf = NULL;
  size_t audLen = getAudioChunk(&audBuf, minLen);
  if (!audLen) return 0;
  haveWav = true;
  // align end of chunk on 4 byte boundary for AVI
  uint16_t filler = (4 - (audLen & 0x00000003)) & 0x00000003;
  size_t chunkLen = audLen + filler;
  uint8_t hdrBuf[CHUNK_HDR];
  memcpy(hdrBuf, wbBuf, 4);
  memcpy(hdrBuf + 4, &chunkLen, 4);
  uint32_t aTime = millis();
  bufferedAviWrite(hdrBuf, CHUNK_HDR);
  bufferedAviWrite(audBuf, audLen);
  if (filler) bufferedAviWrite(zeroFiller, filler);
  aTime = millis() - aTime;
  buildAviIdx(chunkLen, false); // save avi index for audio chunk
  vidSize += chunkLen + CHUNK_HDR;
  return aTime;
}
#endif

static void saveFrame(camera_fb_t* fb) {
  // save frame on SD card
  uint32_t fTime = millis();
  // align end of jpeg on 4 byte boundary for AVI
  uint16_t filler = (4 - (fb->len & 0x00000003)) & 0x00000003;
  size_t jpegSize = fb->len + filler;
  // add avi frame header
  uint8_t hdrBuf[CHUNK_HDR];
  memcpy(hdrBuf, dcBuf, 4);
  memcpy(hdrBuf + 4, &jpegSize, 4);
  bufferedAviWrite(hdrBuf, CHUNK_HDR);
  // add frame content
  uint32_t wTime = millis();
  bufferedAviWrite(fb->buf, fb->len);
  if (filler) bufferedAviWrite(zeroFiller, filler);
  wTime = millis() - wTime;

  buildAviIdx(jpegSize); // index the video frame before the audio chunk that follows it
  vidSize += jpegSize + CHUNK_HDR;
  frameCnt++;
#if INCLUDE_AUDIO
  wTime += writeAudioChunk(AUD_CHUNK_MIN); // audio is an SD write too, count it as storage time
#endif
  wTimeTot += wTime;
  LOG_VRB("SD storage time %lu ms", wTime);
  fTime = millis() - fTime - wTime;
  fTimeTot += fTime;
  LOG_VRB("Frame processing time %lu ms", fTime);
  LOG_VRB("============================");
}

bool recoverAvi() {
  // Rebuild an AVI that lost power before closeAvi() could run. The recorded stream is
  // self describing - saveFrame() writes '00dc' + 4 byte length + JPEG (+ pad), audio
  // writes '01wb' - so the frames can be walked and everything closeAvi() would have
  // written can be reconstructed: index, header, final name.
  // MUST run before startSDtasks(), because openAvi() opens AVITEMP FILE_WRITE and would
  // truncate the evidence on the first recording of the new boot
  if (!STORAGE.exists(AVITEMP)) return false;
  File rf = STORAGE.open(AVITEMP, FILE_READ);
  if (!rf) {
    LOG_WRN("Interrupted recording %s present but cannot be opened", AVITEMP);
    return false;
  }
  size_t fileSize = rf.size();
  uint8_t hdr[AVI_HEADER_LEN];
  uint8_t recFPSv = FPS ? FPS : 20;
  uint8_t recFS = fsizePtr;
  bool haveHdr = false;
  if (fileSize > AVI_HEADER_LEN && rf.read(hdr, AVI_HEADER_LEN) == AVI_HEADER_LEN
      && !memcmp(hdr, "RIFF", 4) && !memcmp(hdr + 8, "AVI ", 4)) {
    // provisional header written by openAvi(): recover the true geometry and rate
    uint16_t w = 0, h = 0;
    uint32_t dwScale = 0, dwRate = 0;
    memcpy(&w, hdr + 0x40, 2);
    memcpy(&h, hdr + 0x44, 2);
    memcpy(&dwScale, hdr + 0x80, 4);
    memcpy(&dwRate, hdr + 0x84, 4);
    // every row, custom sizes included - this used to stop at FS_1280X960 on the assumption
    // that it was the last one, which stopped being true when the FHD ladder was added.
    // Three rows now share 1920x1080, so a recovered clip from any of them is named for the
    // first. Only the name is ambiguous: the geometry it is being recovered for is identical
    for (int i = 0; i < (int)(sizeof(frameData) / sizeof(frameData[0])); i++) {
      if (frameData[i].frameWidth == w && frameData[i].frameHeight == h) { recFS = i; haveHdr = true; break; }
    }
    if (dwScale && dwRate) {
      uint32_t r = dwRate / dwScale;
      if (r >= 1 && r <= 120) recFPSv = (uint8_t)r;
    }
  }
  if (!haveHdr) LOG_WRN("Interrupted recording has no usable header - assuming %s @ %ufps",
    frameData[recFS].frameSizeStr, recFPSv);
  // walk the chunk stream from the end of the reserved header space
  prepAviIndex();
  uint16_t frames = 0;
  uint32_t audChunks = 0;
  size_t pos = AVI_HEADER_LEN;
  uint8_t chunkHdr[CHUNK_HDR];
  rf.seek(pos, SeekSet);
  while (pos + CHUNK_HDR <= fileSize) {
    if (rf.read(chunkHdr, CHUNK_HDR) != CHUNK_HDR) break;
    bool isVid = !memcmp(chunkHdr, dcBuf, 4);
    bool isAud = !memcmp(chunkHdr, wbBuf, 4);
    if (!isVid && !isAud) break; // end of valid data - the rest is stale buffer content
    uint32_t len = 0;
    memcpy(&len, chunkHdr + 4, 4);
    // Strictness is what stops the tail of the last 32KB block being read as frames:
    // a plausible length is not enough, a video chunk must also start with a JPEG SOI
    if (!len || len > maxFrameBuffSize || pos + CHUNK_HDR + len > fileSize) break;
    if (isVid) {
      uint8_t soi[2];
      if (rf.read(soi, 2) != 2 || soi[0] != 0xFF || soi[1] != 0xD8) break;
    }
    buildAviIdx(len, isVid);
    if (isVid) frames++; else audChunks++;
    if (frames && aviIndexNearFull()) {
      LOG_WRN("Recovery stopped at %u frames - index buffer full", frames);
      break;
    }
    pos += CHUNK_HDR + len;
    rf.seek(pos, SeekSet);
  }
  rf.close();
  if (!frames) {
    STORAGE.remove(AVITEMP);
    LOG_ALT("Interrupted recording held no complete frames - discarded");
    return false;
  }
  // reopen read/write and append everything closeAvi() would have written
  File wf = STORAGE.open(AVITEMP, "r+");
  if (!wf) {
    LOG_WRN("Cannot reopen %s to repair it - left in place", AVITEMP);
    return false;
  }
  uint32_t vidDuration = (uint32_t)(((uint64_t)frames * 1000) / (recFPSv ? recFPSv : 1));
  finalizeAviIndex(frames);
  wf.seek(pos, SeekSet); // index goes immediately after the last valid chunk
  size_t readLen = 0;
  do {
    readLen = writeAviIndex(sdReadBuf, RAMSIZE); // idle DRAM buffer: playback and capture exclude each other
    if (readLen) wf.write(sdReadBuf, readLen);
  } while (readLen > 0);
  buildAviHdr(recFPSv, recFS, frames, vidDuration);
  wf.seek(0, SeekSet);
  wf.write(aviHeader, AVI_HEADER_LEN);
  wf.close();
  // Anything after the index is stale buffer content that Arduino's File cannot truncate
  // away. Harmless: players honour the RIFF size just stamped into the header
  char recovered[FILE_NAME_LEN];
  dateFormat(partName, sizeof(partName), true);
  STORAGE.mkdir(partName);
  dateFormat(partName, sizeof(partName), false);
  // Recovery runs seconds into boot, usually before NTP has synced, so on a board that
  // power cycles repeatedly the timestamp is the same 1970 value every time and the names
  // collide. A failed rename would leave AVITEMP in place for the next recording to
  // overwrite - losing exactly the clip this function just rescued - so uniquify
  for (int attempt = 0; attempt < 100; attempt++) {
    if (attempt) snprintf(recovered, FILE_NAME_LEN - 1, "%s_%s_%u_%lu_R%d.%s", partName,
      frameData[recFS].frameSizeStr, recFPSv, (unsigned long)(vidDuration / 1000), attempt, AVI_EXT);
    else snprintf(recovered, FILE_NAME_LEN - 1, "%s_%s_%u_%lu_R.%s", partName,
      frameData[recFS].frameSizeStr, recFPSv, (unsigned long)(vidDuration / 1000), AVI_EXT);
    if (!STORAGE.exists(recovered)) break;
  }
  if (STORAGE.rename(AVITEMP, recovered))
    LOG_ALT("Recovered interrupted recording: %s (%u frames, %lus, %s)", recovered, frames,
      (unsigned long)(vidDuration / 1000), fmtSize(pos));
  else LOG_WRN("Recovered the recording but could not rename %s", AVITEMP);
  return true;
}

static bool closeAvi() {
  // closes the recorded file
  recordingCamMode(false); // unfreeze AWB, restore continuous AF
  if (sdGovBoost) {
    // hand the sensor back at the user's quality; the boost only ever lives inside a clip
    sensor_t* govSensor = esp_camera_sensor_get();
    if (govSensor != NULL) govSensor->set_quality(govSensor, govBaseQ);
  }
  sdGovBoost = 0;
  sdGovFrameKB = 0;
  uint32_t vidDuration = millis() - startTime;
  uint32_t vidDurationSecs = lround(vidDuration / 1000.0);
  logLine();
  LOG_VRB("Capture time %lu secs, %u frames", vidDurationSecs, frameCnt);

  cTime = millis();
#if INCLUDE_AUDIO
  // stop the mic, then append whatever audio is still pending as a final 01wb chunk.
  // Must happen before the write buffer is flushed below
  finishAudioRecord(true);
  writeAudioChunk(0); // 0 forces out the last partial chunk
#endif
  // write remaining frame content to SD
  aviFile.write(sdWriteBuf, highPoint);
  size_t readLen = 0;
  // save avi index
  finalizeAviIndex(frameCnt);
  do {
    readLen = writeAviIndex(sdReadBuf, RAMSIZE); // idle DRAM buffer: playback and capture exclude each other
    if (readLen) aviFile.write(sdReadBuf, readLen);
  } while (readLen > 0);
  // save avi header at start of file
  float actualFPS = (1000.0f * (float)frameCnt) / ((float)vidDuration);
  uint8_t actualFPSint = (uint8_t)(lround(actualFPS));
  xSemaphoreTake(aviMutex, portMAX_DELAY);
  // pass the measured duration so the header carries the exact rate, not the rounded one
  buildAviHdr(actualFPSint, fsizePtr, frameCnt, vidDuration);
  xSemaphoreGive(aviMutex);
  aviFile.seek(0, SeekSet); // start of file
  aviFile.write(aviHeader, AVI_HEADER_LEN);
  aviFile.close();
  LOG_VRB("Final SD storage time %lu ms", millis() - cTime);
  uint32_t hTime = millis();
#if INCLUDE_MQTT
  if (mqtt_active) {
    sprintf(jsonBuff, "{\"RECORD\":\"OFF\", \"TIME\":\"%s\"}", esp_log_system_timestamp());
    mqttPublish(jsonBuff);
    mqttPublishPath("record", "off");
  }
#endif
  // a motion capture runs at least until the stop timer expires, so there is no short
  // recording to discard - the only file worth throwing away is one with nothing in it
  if (frameCnt) {
    // name file to include actual dateTime, FPS, duration, and frame count
    int alen = snprintf(aviFileName, FILE_NAME_LEN - 1, "%s_%s_%u_%lu%s%s.%s",
                        partName, frameData[fsizePtr].frameSizeStr, actualFPSint, vidDurationSecs,
                        haveWav ? "_S" : "", dashCamOn ? "_C" : "", AVI_EXT);
    if (alen > FILE_NAME_LEN - 1) LOG_WRN("file name truncated");
    STORAGE.rename(AVITEMP, aviFileName);
    LOG_VRB("AVI close time %lu ms", millis() - hTime);
    cTime = millis() - cTime;
    if (dashCamOn) forceRecord = true; // restart continuous recording
    else {
      // AVI stats
      LOG_INF("******** AVI recording stats ********");
      LOG_ALT("Recorded %s", aviFileName);
      LOG_INF("AVI duration: %lu secs", vidDurationSecs);
      LOG_INF("Number of frames: %u", frameCnt);
      LOG_INF("Required FPS: %u", FPS);
      LOG_INF("Actual FPS: %0.1f", actualFPS);
      LOG_INF("File size: %s", fmtSize(vidSize));
      if (frameCnt) {
        LOG_INF("Average frame length: %lu bytes", vidSize / frameCnt);
        LOG_INF("Average frame monitoring time: %lu ms", dTimeTot / frameCnt);
        LOG_INF("Average frame buffering time: %lu ms", fTimeTot / frameCnt);
        LOG_INF("Average frame storage time: %lu ms", wTimeTot / frameCnt);
      }
      LOG_INF("Average SD write speed: %lu kB/s", ((vidSize / wTimeTot) * 1000) / 1024);
      if (govMaxBoost) LOG_INF("SD governor: max quality boost %u (quality %u of base %u)", govMaxBoost, govBaseQ + govMaxBoost, govBaseQ);
      if (rescueClipCnt) LOG_WRN("No-frame rescue fired %u times during this clip - frames exceeded the sensor's JPEG window", rescueClipCnt);
      LOG_INF("File open / completion times: %lu ms / %lu ms", oTime, cTime);
      LOG_INF("Busy: %lu%%", std::min(100 * (wTimeTot + fTimeTot + dTimeTot + oTime + cTime) / vidDuration, (uint32_t)100));
      checkMemory();
      LOG_INF("*************************************");
      // send out notification of motion if requested
#if INCLUDE_SMTP
      if (smtpUse) {
        // send email with movement image
        char subjectMsg[50];
        snprintf(subjectMsg, sizeof(subjectMsg) - 1, "from %s, in %s", hostName, aviFileName);
        emailAlert("Motion Alert", subjectMsg);
      } 
#endif
#if INCLUDE_TGRAM
      tgramAlert(aviFileName, "");
#endif
#if INCLUDE_FTP_HFS
      if (autoUpload) {
        if (deleteAfter) {
          // issue #380 - in case other files failed to transfer, do whole parent folder
          dateFormat(partName, sizeof(partName), true);
          fsStartTransfer(partName);
        } else fsStartTransfer(aviFileName); // transfer this file to remote ftp server
      }
#endif
    }
    if (!checkFreeStorage()) doRecording = forceRecord = false;
    aviBusy = false;
    return true;
  } else {
    // nothing was captured, so there is no usable file to keep
    STORAGE.remove(AVITEMP);
    LOG_WRN("Discarded empty recording after %lu secs", vidDurationSecs);
    aviBusy = false;
    return false;
  }
}

static void buildZoneOverlay(uint8_t* z, bool refOk, int zoneT, uint16_t maskPhys) {
  // Show Motion view: a 96x96 image of 16 blocks in VIEW order - gray = zone luminance,
  // red = zone moved this check, dimmed = masked out of detection. Encoded into the same
  // motionJpeg buffer the stream task already consumes via motionSemaphore
  if (motionJpeg == NULL) motionJpeg = (uint8_t*)ps_malloc(32 * 1024);
  static uint8_t* ovBuf = NULL;
  if (ovBuf == NULL) ovBuf = (uint8_t*)ps_malloc(96 * 96 * 3);
  if (motionJpeg == NULL || ovBuf == NULL) return;
  if (motionJpegLen) return; // previous overlay not yet taken by the stream task
  for (int v = 0; v < 16; v++) {
    int p = zoneViewToPhys(v);
    int d = refOk ? abs((int)z[p] - (int)zref[p]) : 0;
    bool moved = refOk && d >= zoneT && ((maskPhys >> p) & 1);
    bool active = (maskPhys >> p) & 1;
    uint8_t g = active ? z[p] : z[p] / 3;
    uint8_t rC = moved ? 255 : g, gC = moved ? g / 2 : g, bC = moved ? g / 2 : g;
    int row0 = (v / 4) * 24, col0 = (v % 4) * 24;
    for (int y = row0; y < row0 + 24; y++) {
      uint8_t* px = ovBuf + (y * 96 + col0) * 3;
      for (int x = 0; x < 24; x++) { *px++ = rC; *px++ = gC; *px++ = bC; }
    }
  }
  uint8_t* jpg = NULL;
  size_t jpgLen = 0;
  if (fmt2jpg(ovBuf, 96 * 96 * 3, 96, 96, PIXFORMAT_RGB888, 80, &jpg, &jpgLen)) {
    if (jpgLen && jpgLen < 32 * 1024) {
      memcpy(motionJpeg, jpg, jpgLen);
      motionJpegLen = jpgLen;
      xSemaphoreGive(motionSemaphore);
    }
    free(jpg);
  }
}

static bool zoneMotion(bool motionStatus) {
  // The whole detector: the sensor's own 4x4 zone luminance grid (section 7.28,
  // 0x5691-0x56A0) compared against the previous check. ~20 SCCB reads (~2-3ms), no frame
  // decode, so it works at every frame size and keeps running through recordings and live
  // views. Replaces the JPEG-decode detector - see BOARD_TESTING.md for the decode hang
  // that capped detection at HD and the stale-state retrigger the suspend design produced.
  //
  // Two gates make the zones trustworthy, both measured:
  //  - AEC deadband (section 4.5): while YAVG sits inside {0x3A1E..0x3A1B} the AEC holds
  //    exposure, and on a static scene the 16 zones are stable to +-1 count. Outside the
  //    band the AEC is re-exposing and every zone moves at once - an optical event, not
  //    motion. That includes the re-exposure a person entering causes, so a trigger can
  //    lag the entry by the AEC settle time (~0.5-1s worst case).
  //  - AF state: a focus hunt also moves every zone at once. Codes from the AF library's
  //    app-note derived header: 0x70 idle, 0x10 focused, anything else is a hunt.
  // A failed gate is indeterminate: the reference is dropped and the existing motion state
  // is returned unchanged - no trip, no un-trip, never a false edge. Crucially it also
  // PAUSES the consecutive-trip streak rather than resetting it: real movement perturbs
  // the AEC continuously, so checks alternate between tripped and out-of-band - measured
  // on the first walk test, where 14 zones moved at delta 52 yet the trigger never fired
  // because every re-exposure zeroed the streak. False-trigger immunity is unharmed: after
  // a gate failure the reference must rebuild before any comparison can trip, so a global
  // optical event (lights, AF) still produces no edge.
  static uint8_t refFS = 255; // reference is meaningless across a size change
  sensor_t* s = esp_camera_sensor_get();
  if (s == NULL || s->get_reg == NULL) return motionStatus;
  int yavg = s->get_reg(s, 0x56A1, 0xFF);
  int bandHi = s->get_reg(s, 0x3A1B, 0xFF);
  int bandLo = s->get_reg(s, 0x3A1E, 0xFF);
  // YAVG is pre-gamma, so it reads ~3x lower than the decoded-image mean the old detector
  // used (measured: YAVG 33 on a scene the old path called light 33-45%). Scale against 96
  // rather than 255 so lswitch keeps its existing calibration; clamp at 100. The
  // AEC-normalised caveat is unchanged: this measures exposure output, not room brightness
  if (yavg >= 0) lightLevel = (yavg >= 96) ? 100 : (yavg * 100) / 96;
  nightTime = isNight(nightSwitch);
  // the night gate blocks detection outright, and lightLevel measures auto exposure output
  // rather than room brightness, so it can latch in ordinary indoor light. Say so once per
  // transition - otherwise detection silently does nothing and looks like a sensitivity bug
  static bool warnedNight = false;
  if (nightTime && !warnedNight) {
    LOG_WRN("Motion detection suspended - night mode (light %u%% below lswitch %u)", lightLevel, nightSwitch);
    warnedNight = true;
  } else if (!nightTime && warnedNight) {
    LOG_INF("Motion detection resumed - daylight (light %u%%)", lightLevel);
    warnedNight = false;
  }
  if (nightTime) {
    zrefValid = false;
    zconsecCnt = 0;
    return false;
  }
  bool gatesOk = (yavg >= 0 && yavg >= bandLo && yavg <= bandHi);
#if INCLUDE_AF
  if (gatesOk) {
    uint8_t af = ov5640AF.getFWStatus();
    gatesOk = (af == FW_STATUS_S_IDLE || af == FW_STATUS_S_FOCUSED);
  }
#endif
  if (!gatesOk) {
    // reference dropped, streak PAUSED - see the header comment for why it must not reset
    zrefValid = false;
    return motionStatus;
  }
  uint8_t z[16];
  for (int i = 0; i < 16; i++) {
    int v = s->get_reg(s, 0x5691 + i, 0xFF);
    if (v < 0) { zrefValid = false; return motionStatus; } // SCCB glitch
    z[i] = (uint8_t)v;
  }
  if (sensorFS != refFS) {
    refFS = sensorFS;
    zrefValid = false; // zones now describe a different framing - no false trip on a size change
    zconsecCnt = 0;
  }
  int zoneT = zoneThresh();
  uint16_t maskPhys = zoneMaskPhys();
  bool tripped = false;
  if (zrefValid) {
    int moved = 0, maxDelta = 0;
    for (int i = 0; i < 16; i++) {
      if (!((maskPhys >> i) & 1)) continue; // masked out of the region of interest
      int d = abs((int)z[i] - (int)zref[i]);
      if (d > maxDelta) maxDelta = d;
      if (d >= zoneT) moved++;
    }
    tripped = (moved >= zoneCount);
    if (maxDelta > zonePeakDelta) zonePeakDelta = maxDelta;
    if (moved > zonePeakMoved) zonePeakMoved = moved;
    if (tripped) {
      zconsecCnt++;
      // need a minimum sequence of tripped checks to signal valid movement
      if (!motionStatus && zconsecCnt >= (uint32_t)detectMotionFrames) {
        LOG_INF("Motion detected: %d zones moved >= %d (zoneCount %d, %lu consecutive), light %u%%",
          moved, zoneT, zoneCount, zconsecCnt, lightLevel);
        motionStatus = true;
#if INCLUDE_MQTT
        if (mqtt_active) {
          sprintf(jsonBuff, "{\"MOTION\":\"ON\", \"TIME\":\"%s\"}", esp_log_system_timestamp());
          mqttPublish(jsonBuff);
          mqttPublishPath("motion", "on");
        }
#endif
      }
    } else {
      zconsecCnt = 0;
      if (motionStatus) {
        LOG_INF("Motion ended: %d zones moved, below zoneCount %d", moved, zoneCount);
        motionStatus = false;
#if INCLUDE_MQTT
        if (mqtt_active) {
          sprintf(jsonBuff, "{\"MOTION\":\"OFF\", \"TIME\":\"%s\"}", esp_log_system_timestamp());
          mqttPublish(jsonBuff);
          mqttPublishPath("motion", "off");
        }
#endif
      }
    }
  }
  if (dbgMotion) buildZoneOverlay(z, zrefValid, zoneT, maskPhys); // before the reference update, so deltas show
  memcpy(zref, z, sizeof(zref));
  zrefValid = true;
  return motionStatus;
}

void zoneStatsJson(char* buff, size_t buffLen) {
  // JSON snapshot for threshold tuning: live zones, deltas against the detector's current
  // reference, the trust gates, thresholds in force, and the detector state. Safe from the
  // web task - SCCB reads only, same as camRegRd
  sensor_t* s = esp_camera_sensor_get();
  if (s == NULL || s->get_reg == NULL) {
    snprintf(buff, buffLen, "{\"err\":\"no sensor\"}");
    return;
  }
  uint8_t z[16];
  for (int i = 0; i < 16; i++) {
    int v = s->get_reg(s, 0x5691 + i, 0xFF);
    z[i] = (v < 0) ? 0 : (uint8_t)v;
  }
  int yavg = s->get_reg(s, 0x56A1, 0xFF);
  int bandHi = s->get_reg(s, 0x3A1B, 0xFF);
  int bandLo = s->get_reg(s, 0x3A1E, 0xFF);
  bool aecStable = (yavg >= 0 && yavg >= bandLo && yavg <= bandHi);
  int afStat = -1;
#if INCLUDE_AF
  afStat = ov5640AF.getFWStatus();
#endif
  uint16_t maskPhys = zoneMaskPhys();
  char* p = buff;
  p += sprintf(p, "{\"zones\":[");
  for (int i = 0; i < 16; i++) p += sprintf(p, "%u%s", z[i], (i < 15) ? "," : "");
  p += sprintf(p, "],\"deltas\":[");
  for (int i = 0; i < 16; i++)
    p += sprintf(p, "%d%s", zrefValid ? abs((int)z[i] - (int)zref[i]) : 0, (i < 15) ? "," : "");
  p += sprintf(p, "],\"yavg\":%d,\"bandLo\":%d,\"bandHi\":%d,\"aecStable\":%d,\"afStat\":%d,", yavg, bandLo, bandHi, aecStable ? 1 : 0, afStat);
  p += sprintf(p, "\"zoneThresh\":%d,\"zoneCount\":%d,\"consecNeeded\":%d,", zoneThresh(), zoneCount, detectMotionFrames);
  p += sprintf(p, "\"zoneMask\":%d,\"maskPhys\":%u,\"refValid\":%d,\"consec\":%lu,", zoneMask, maskPhys, zrefValid ? 1 : 0, zconsecCnt);
  p += sprintf(p, "\"motion\":%d,\"capturing\":%d,\"light\":%u,\"night\":%d,\"detectOK\":%d}",
    haveMotion ? 1 : 0, isCapturing ? 1 : 0, lightLevel, nightTime ? 1 : 0, zoneDetectOK ? 1 : 0);
}

// No-frame watchdog. An oversized JPEG is truncated inside the sensor's frame window
// and the driver then delivers NOTHING - not degraded frames (see BOARD_TESTING 19-20).
// Nothing else can recover from that state: settleSensor runs at the END of
// processFrame so pending retimes never apply, and the SD governor accounts bytes that
// never arrive. When frames stop, step JPEG quality UP (more compression) until they
// return. The rescue is STICKY by design: the frame window is a hard cliff, so
// stepping back down would re-kill frames - the rescued quality stands until the user
// changes quality/size/fps themselves. WRN-logged per step, counted per clip.
// (State lives with the governor block near the top of the file.)
static void sagShutdown() {
  // Stage 1 response, in task context: the ISR only set a flag, so there is a stack,
  // flash access and time here. Called from processFrame with the recording still open,
  // so the normal close path finalises the file immediately after this returns
  supplyParked = true;
  supplyParkedMs = millis();
  markSagStage1(); // survives the reset, so the next boot can say this ran
  LOG_ALT("SUPPLY SAG at brownout level %u (trip %lu) - landing recording and parking",
    brownoutArmedLevel(), sagTripCount);
  doRecording = false; // block new recordings, including motion triggered ones
  // Sensor to software standby: the 5MP array plus its PSRAM traffic is the dominant
  // draw, and shedding it stops us pulling the rail down any further. Reversible, but
  // nothing here re-enables it - recovery is a power cycle onto a charged pack
  sensor_t* s = esp_camera_sensor_get();
  if (s != NULL && s->set_reg != NULL) s->set_reg(s, 0x3008, 0xFF, 0x42); // standby
  // Deliberately NOT re-arming a terminal stage, though an earlier version did.
  // The comparator readback (bodDump) showed both configurable reset paths already
  // disabled - RST_ENA 0, ANA_RST_EN 0 - through all three battery runs, and the chip
  // still reset with ESP_RST_BROWNOUT every time: the S3's own protection handles the
  // final collapse whatever this detector is told to do. A terminal arm therefore buys
  // no protection, while its flash power-down demonstrably stops the chip executing when
  // a trip occurs (hung the board twice on 28 Aug, needing a physical power cycle) and
  // its RF power-down would drop the only interface a battery board has. Leave the
  // comparator exactly where it is and let the hardware do the last step.
  // Straight fsync, not flush_log(false): that sleeps a second, which is a poor thing to
  // do in the capture task while the rail is on its way down
  logSyncSD();
}

#include "esp_ota_ops.h"

// Rollback verification. CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE is set, so a freshly
// flashed image boots as ESP_OTA_IMG_PENDING_VERIFY and must be confirmed with
// esp_ota_mark_app_valid_cancel_rollback() or the next reboot reverts to the previous
// image. The Arduino core confirms it inside initArduino() - before wifi, camera or
// storage have been tried - which means an image that boots but cannot be reached is
// declared healthy and there is no way back. On a headless board that is the difference
// between a retry and a lost deployment.
// Overriding the core's weak verifyRollbackLater() defers the decision to us instead.
// extern "C" is load-bearing: the core defines the weak symbol in a .c file and declares
// it in no header, so a plain C++ definition here mangles to a different symbol and the
// core's default (return false, confirm immediately) silently wins - which is exactly
// what happened on the first verification run: otaConfirm() found the image already
// VALID and the whole ladder was dead code
extern "C" bool verifyRollbackLater() {
  return true;
}

static void otaConfirm() {
  // Confirm only once the app has actually proven itself. The uptime fallback is what
  // keeps this safe: any image that simply stays up gets confirmed, so a bug here cannot
  // put us in a rollback loop that eats every new build
  static bool settled = false;
  if (settled) return;
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t state;
  if (running == NULL || esp_ota_get_state_partition(running, &state) != ESP_OK) {
    settled = true;
    return;
  }
  if (state != ESP_OTA_IMG_PENDING_VERIFY) {
    settled = true; // already valid, or not an OTA slot - nothing to do
    return;
  }
  bool camOk = esp_camera_sensor_get() != NULL;
  bool storageOk = (STORAGE.totalBytes() > 0); // mounted; not "an SD card is present"
  bool reachable = (WiFi.STA.status() == WL_CONNECTED) || (millis() > 5 * 60 * 1000);
  if (!camOk || !storageOk || !reachable) return;
  if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK)
    LOG_ALT("OTA image confirmed valid (camera, storage, %s) - rollback cancelled",
      WiFi.STA.status() == WL_CONNECTED ? "wifi" : "uptime");
  else LOG_WRN("Failed to mark OTA image valid - it will roll back on the next reboot");
  settled = true;
}

static void battMonitor() {
  // 1Hz from processFrame, so it runs whether or not a recording is open - unlike the
  // governor tick, which only runs while recording
  if (!battUse || battPin < 1 || supplyParked) return;
  uint32_t now = millis();
  if (now - battPollMs < 1000) return;
  battPollMs = now;
  if (!battAdcReady) {
    // setupADC() sets attenuation and resolution, and is otherwise only called from
    // peripherals.cpp - which is compiled out on this board, so without this the ADC sits
    // at core defaults and every reading scaled against MAX_ADC is silently wrong
    setupADC();
    battAdcReady = true;
  }
  // smoothAnalog averages ADC_SAMPLES reads; scale to the tap voltage, then through the
  // divider ratio to the cell
  uint32_t tapMv = ((uint32_t)smoothAnalog(battPin) * 3300) / MAX_ADC;
  battMv = (uint16_t)((tapMv * battScale) / 1000);
  if (battMv < BATT_FLOOR_MV) {
    // no plausible cell here - almost certainly no divider fitted, or the wrong pin
    if (!battFloorWarned) {
      battFloorWarned = true;
      LOG_WRN("Battery monitor reads %umV on pin %d, below the %dmV floor - no divider fitted? Not acting on it",
        battMv, battPin, BATT_FLOOR_MV);
    }
    battLowCount = 0;
    return;
  }
  battFloorWarned = false;
  if (battMv > battWarnMv) {
    battLowCount = 0;
    return;
  }
  // the cell sags under camera load bursts, so one dip must not park the device
  if (++battLowCount < BATT_CONFIRM) return;
  LOG_ALT("Battery at %umV, below the %dmV warning level on %d consecutive samples",
    battMv, battWarnMv, BATT_CONFIRM);
  supplySagging = true; // actioned by the sag handler in processFrame, same as a comparator trip
}

// Sensor idle throttle (Tier 2 power saving). Zone detection reads sensor registers at
// most 5 times a second whatever the frame rate, so streaming 30fps around the clock
// while idle buys nothing - and the sensor plus its DVP/PSRAM traffic is the dominant
// idle load (bench: 360mA idle baseline with the radio already sleeping). Throttle the
// sensor to idleFps after idleSecs of inactivity; restore the user's rate the moment
// anything needs frames. The restore runs in the same processFrame pass that starts a
// capture, before the file opens - the first frames arrive at the ramping rate and the
// AVI header carries the measured fps, so clips stay valid
int idleFps = 0;   // 0 = feature off (stock behaviour)
int idleSecs = 10;

static void idleSetRate(uint8_t fps, bool throttled) {
  // The frame timer AND the sensor: setFPS() alone only changes how often frames are
  // fetched, while the sensor keeps streaming at its tuned rate into the driver's DMA
  // and burns the same power - measured, 360mA flat with the timer at 5fps. The sensor's
  // fps-derived timing flows through desiredFPS(), so raise the flag first, then re-run
  // applySensorTuning() (crop, HTS floor, tuned VTS/PIXCLK, AEC limits) to make it real
  idleRateActive = throttled;
  setFPS(fps);
  sensor_t* s = esp_camera_sensor_get();
  if (s != NULL) applySensorTuning(s, (framesize_t)fsizePtr);
}

static void idleThrottle() {
  static uint32_t lastActiveMs = 0;
  if (!idleFps) {
    // feature off - if disabled mid-throttle, hand the sensor back
    if (idleRateActive) idleSetRate(captureFPS, false);
    return;
  }
  bool wantFull = isCapturing || forceRecord || doPlayback || doKeepFrame || streamsBusy();
  uint32_t now = millis();
  if (wantFull) lastActiveMs = now;
  if (idleRateActive) {
    if (wantFull) {
      idleSetRate(captureFPS, false);
      LOG_INF("Idle throttle released - sensor back at %ufps", captureFPS);
    }
  } else if (!wantFull && FPS == captureFPS && idleFps < captureFPS
      && now - lastActiveMs > (uint32_t)idleSecs * 1000) {
    // the FPS == captureFPS guard means we never take over a rate someone else set -
    // playback owns FPS while it runs, and a mid-change state is left alone
    idleSetRate(idleFps, true);
    LOG_INF("Idle throttle - sensor at %ufps until something needs frames", idleFps);
  }
}

static void supervisors() {
  // periodic housekeeping that must run in every state, so it lives in the capture task
  // rather than the governor tick (which only runs while recording)
  static uint32_t lastMs = 0;
  uint32_t now = millis();
  if (now - lastMs < 10000) return;
  lastMs = now;
  otaConfirm();
  wifiSupervise();
}

static void noFrameRescue() {
  uint32_t now = millis();
  if (supplyParked) return; // parked: no frames expected, nothing to rescue
  if (!lastGoodFrameMs) { lastGoodFrameMs = now; return; } // arm on first miss after boot
  // threshold rides out retime/PLL transients (~1s) and the frame gaps of low rates
  uint32_t thresh = FPS ? 4000u / FPS : 2000;
  if (thresh < 2000) thresh = 2000;
  if (now - lastGoodFrameMs < thresh) return;
  sensor_t* s = esp_camera_sensor_get();
  if (s == NULL) return;
  int q = s->status.quality + 2;
  if (q > 63) q = 63;
  if (q == s->status.quality) { lastGoodFrameMs = now; return; } // already at the cap
  s->set_quality(s, q);
  govRebaseQuality(q); // sticky: the rescue is the governor's new base, closeAvi keeps it
  if (isCapturing) rescueClipCnt++;
  LOG_WRN("No frames for %lums - quality %d rescue%s", now - lastGoodFrameMs, q,
    isCapturing ? " (mid-recording)" : "");
  lastGoodFrameMs = now; // pace one step per threshold
}

static boolean processFrame() {
  // get camera frame
  // whether the open recording was started by motion, and so closes on the extending
  // timer (moveStopSecs after the last motion) rather than when the record button is let go
  static bool motionTriggered = false;
  static uint32_t lastMotionMs = 0; // refreshed by every check that reports motion
  bool res = true;
  uint32_t dTime = millis();

  camera_fb_t* fb = esp_camera_fb_get();
  if (fb == NULL) {
    noFrameRescue();
    return false;
  }
  if (!fb->len || fb->len > maxFrameBuffSize) {
    // counts as a miss for the watchdog too: a stream of oversized frames starves a
    // recording exactly like absent ones, and the same rescue (smaller frames) fixes both
    noFrameRescue();
    // Must hand the buffer back even when rejecting the frame. There are only FB_CNT of
    // them and the driver never reclaims one that is still checked out, so a handful of
    // bad frames leaves esp_camera_fb_get() with nothing to return and the capture task
    // wedges for good. Bad frames get more likely at large frame sizes, where a slow
    // motion check holds a buffer for longer than the sensor's frame interval
    badFrameCnt++;
    LOG_VRB("Discarded bad frame, len %u", fb->len);
    esp_camera_fb_return(fb);
    return false;
  }
  lastGoodFrameMs = millis(); // feeds the no-frame watchdog; stale-size flushes below
  // still count - the sensor-to-driver path is provably moving

  // Drop frames left over from before a sensor switch. Tested against the geometry we
  // switched away from rather than against the size we expect, because frameData and the
  // sensor disagree for P_FHD (1080 vs 1088) - matching the old size exactly avoids
  // discarding perfectly good frames on that one. Bounded by FB_CNT so an unexpected size
  // cannot stall the capture task. Placed before the stream copy and keepFrame so a stale
  // frame reaches neither a viewer nor a recording
  if (flushFrames) {
    // Tested against the encoded JPEG, not fb->width - see jpegWidth(). Compared with the
    // width we switched away from rather than the one we expect, so P_FHD's frameData /
    // sensor disagreement (1080 vs 1088) cannot cause good frames to be dropped
    flushFrames--;
    if (jpegWidth(fb->buf, fb->len) == staleWidth) {
      staleFrameCnt++;
      esp_camera_fb_return(fb);
      return false;
    }
    flushFrames = 0; // buffer content is genuinely at the new size now
  }

  for (int i = 0; i < vidStreams; i++) {
    // hand off to the two-deep ring; a refusal means every slot is still awaiting
    // send, which is the transport genuinely falling behind rather than a handshake gap
    if (!streamOfferFrame(i, fb->buf, fb->len) && streamSlotActive(i)) streamSkipped[i]++;
  }
  if (doKeepFrame) {
    keepFrame(fb);
    doKeepFrame = false;
  }

  // Zone detection runs in every state - a check is ~20 SCCB reads, no frame decode, so
  // recording, live view and NVR carry it for free. The old decode detector, its VGA
  // round-trip and its large-frame hang are gone with the decoder (see BOARD_TESTING.md)
  int reasonId = 0;
  bool prevMotion = haveMotion;
  if (detectionActive()) {
    if (doMonitor()) {
      uint32_t mTime = micros(); // micros, as a check is only a few ms
      // The argument is the PREVIOUS motion state, which zoneMotion() uses to find the
      // start and stop edges - passing a literal false would make every check look like
      // a fresh start and repeat the motion log and MQTT on every check
      if (zoneMotion(haveMotion)) reasonId = 1; // check 1 in N frames
      mTimeTot += micros() - mTime;
      mCheckCnt++;
#if INCLUDE_PERIPH
      if (pirUse && getPIRval()) reasonId = 2;
#endif
      haveMotion = (reasonId) ? true : false;
    }
  } else haveMotion = false; // disarmed mid-event: a stale true must not hold a recording open

  // process motion status
  if (haveMotion) lastMotionMs = millis(); // feeds the extending stop timer below
  if (haveMotion && !prevMotion) {
    // start of movement: the frame in hand is at the capture resolution now, so the alert
    // image is full size. The still handler reports the real size rather than assuming
    keepFrame(fb);
#if INCLUDE_PERIPH
    buzzerAlert(true); // sound buzzer if enabled
    if (lampAuto && nightTime) setLamp(lampLevel);  // switch on lamp if requested
#endif
  }
  if (!haveMotion) {
#if INCLUDE_PERIPH
    if (lampAuto) setLamp(0); // switch off lamp
    buzzerAlert(false); // switch off buzzer if still on
#endif
  }

  // Recording status. A motion triggered capture records while movement continues: every
  // check reporting motion refreshes lastMotionMs, and the recording closes once
  // moveStopSecs pass without it - detection keeps running throughout, which is what makes
  // this possible (and removes the old suspend-then-resume stale-state retrigger). The
  // frame limit and AVI index remain as hard caps; a cap-close with motion still present
  // chains straight into a new file on the next pass. A forced capture (record button or
  // dashcam) still runs for as long as it is held on.
  // doRecording is the Save Capture toggle, and is also cleared when the SD card fills and
  // when there is no card at all. The record button bypasses it, being an explicit user action
  bool prevCapture = isCapturing;
  if (!isCapturing) {
    // supplyParked also gates the record button, which otherwise bypasses doRecording -
    // an explicit user action cannot make a failing supply safe to write to
    if (supplyParked) forceRecord = false;
    else if ((haveMotion && doRecording) || forceRecord) {
      isCapturing = true;
      motionTriggered = haveMotion && !forceRecord;
    }
  } else if (motionTriggered) {
    if (!forceRecord && millis() - lastMotionMs > (uint32_t)moveStopSecs * 1000) isCapturing = false;
  } else if (!forceRecord) isCapturing = false;

  if (isCapturing && !videoSizeAllowed(fsizePtr)) {
    // refuse the recording, but leave stills running at this size.
    // If a recording was already open this closes it cleanly via the branch below
    if (!prevCapture) LOG_WRN("Video recording unavailable at %s, above the %s cap - stills are unaffected",
      frameData[fsizePtr].frameSizeStr, frameData[maxVideoFS].frameSizeStr);
    isCapturing = forceRecord = motionTriggered = false;
  }
  battMonitor(); // may set supplySagging, actioned immediately below
  supervisors();
  // after the capture decision above, before the open branch below: a wake triggered by
  // this pass's motion restores the user's rate before the file opens
  idleThrottle();
  // Supply sag (Stage 1). Placed here deliberately: after prevCapture has been taken, so
  // the close path below finalises the open file properly - a sag must never leave the
  // orphan that boot recovery then has to repair - and before the start branch, so a
  // recording cannot begin on a rail that is already failing
  if (supplySagging) {
    supplySagging = false;
    sagShutdown();
    isCapturing = forceRecord = motionTriggered = false;
  }
  // a controlled restart (web reset, OTA, mqtt) asked for the recording to be landed
  if (shutdownPending) isCapturing = forceRecord = motionTriggered = false;
  // brownout holdoff expiry - millis() is uptime, so this is simply "60s since boot"
  if (holdoffActive && millis() > (uint32_t)BROWNOUT_HOLDOFF_SECS * 1000) {
    holdoffActive = false;
    doRecording = holdoffSavedRecording;
    LOG_ALT("Brownout holdoff expired - recording re-armed");
  }
  if (isCapturing && !prevCapture) {
    // New movement has occurred or record button pressed. The sensor is already at the
    // capture resolution, so this pass just opens the file; the frame in hand is handed
    // back and the next notify delivers the first saved frame
    esp_camera_fb_return(fb);
    stopPlaying(); // terminate any playback
    stopPlayback = true; // stop any subsequent playback
    if (!dashCamOn) LOG_ALT("Capture started by %s%s%s%s", reasonId == 0 ? "Button" : "", reasonId == 1 ? "Camera " : "", reasonId == 2 ? "PIR" : "", reasonId == 3 ? "Accelerometer" : "");
#if INCLUDE_MQTT
    if (mqtt_active) {
      sprintf(jsonBuff, "{\"RECORD\":\"ON\", \"TIME\":\"%s\"}", esp_log_system_timestamp());
      mqttPublish(jsonBuff);
      mqttPublishPath("record", "on");
    }
#endif
    wsAsyncSendJson("ustatus", "\"showRecord\":1");
    setSensorSize((framesize_t)fsizePtr); // defensive no-op - settleSensor keeps the sensor there
    // the extending timer governs a motion recording's length; maxFrames (and the AVI
    // index) stay as the hard cap, which at 30fps is 120s per file before chaining
    if (!dashCamOn) frameLimit = maxFrames;
    openAvi();
    return true;
  }

  if (isCapturing) {
    // capture is ongoing
    showProgress();
    if (frameCnt < frameLimit) {
      dTimeTot += millis() - dTime;
      saveFrame(fb);
      sdGovernor();
      // the index buffer is the real ceiling - below 4fps audio needs an entry per frame,
      // so it can fill before frameLimit. Closing here yields a valid file, where
      // overflowing it used to reboot the device in the middle of a recording
      bool indexFull = aviIndexNearFull();
      if (frameCnt >= frameLimit || indexFull) {
        // stop saving frames for this avi as limit reached
        isCapturing = forceRecord = motionTriggered = false;
        if (!dashCamOn) {
          logLine();
          if (indexFull) LOG_WRN("Auto closed recording after %u frames - AVI index full", frameCnt);
          else LOG_WRN("Auto closed recording after %u frames", frameLimit);
        }
      }
    }
#if INCLUDE_PERIPH
    if (buzzerUse && frameCnt / FPS >= buzzerDuration) buzzerAlert(false); // switch off after given period
#endif
  }

  esp_camera_fb_return(fb);
  if (!isCapturing && prevCapture) {
    // finish recording (normal or forced)
    closeAvi();
    wsAsyncSendJson("ustatus", "\"showRecord\":0");
    stopPlayback = false; // allow for playbacks
  }

  // settle the sensor for whatever state we are now in. No-op when already correct, and
  // safe here because the frame buffer has been handed back above. (Viewers no longer
  // suspend motion recording - zone detection runs through them)
  settleSensor();
  return res;
}

static void captureTask(void* parameter) {
  // woken by frame timer when time to capture frame
  uint32_t ulNotifiedValue;
  while (true) {
    ulNotifiedValue = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    // busy marker brackets the whole burst: OTAprereq() waits on it so vTaskDelete only
    // ever lands here at the notify wait, where no SCCB, FATFS or frame buffer is held
    capturePassBusy = true;
    // Enrol this task in the watchdog and pet it once per wake. Nothing watched it before, and
    // it is the task most able to block: saveFrame() writes to the card synchronously with no
    // timeout anywhere below it, and FATFS is reentrant so it holds the volume mutex while it
    // does. A card that stops responding therefore took the web server and everything else that
    // touches storage down with it, while the ping task carried on petting the only watchdog
    // there was - which is why the board answered pings for ever and never recovered on its own.
    // The frame timer runs whether or not a recording is in progress, so this task wakes at the
    // frame rate regardless and a missed pet means genuinely stuck, not merely idle. The one
    // path that legitimately stops the timer is OTAprereq(), which now stops the watchdog first
    resetWatchDog(1, wifiTimeoutSecs * 1000 * 2);
    if (ulNotifiedValue > FB_CNT) ulNotifiedValue = FB_CNT; // prevent too big queue if FPS excessive
    // may be more than one isr outstanding if the task delayed by SD write or jpeg decode
    while (ulNotifiedValue-- > 0) processFrame();
    capturePassBusy = false;
  }
  vTaskDelete(NULL);
}

uint8_t setFPS(uint8_t val) {
  // change or retrieve FPS value
  if (val) {
    FPS = val;
    // change frame timer which drives the task
    controlFrameTimer(true);
    saveFPS = FPS; // used to reset FPS after playback
  }
  return FPS;
}

uint8_t setFPSlookup(uint8_t val) {
  // set FPS from framesize lookup - the user's rate survives, clamped to the new ceiling
  fsizePtr = val;
  if (captureFPS > fpsCeiling((framesize_t)fsizePtr)) captureFPS = fpsCeiling((framesize_t)fsizePtr);
  return setFPS(captureFPS);
}

/********************** plackback AVI as MJPEG ***********************/

static fnameStruct extractMeta(const char* fname) {
  // extract FPS, duration, and frame count from avi filename
  fnameStruct fnameMeta = {0, 0, 0};
  char fnameStr[FILE_NAME_LEN];
  strcpy(fnameStr, fname);
  // replace all '_' with space for sscanf
  replaceChar(fnameStr, '_', ' ');
  int items = sscanf(fnameStr, "%*s %*s %*s %hhu %lu", &fnameMeta.recFPS, &fnameMeta.recDuration);
  if (items != 2) LOG_ERR("failed to parse %s, items %u", fname, items);
  return fnameMeta;
}

static void playbackFPS(const char* fname) {
  // extract meta data from filename to commence playback
  fnameStruct fnameMeta = extractMeta(fname);
  recFPS = fnameMeta.recFPS;
  if (recFPS < 1) recFPS = 1;
  recDuration = fnameMeta.recDuration;
  // temp change framerate to recorded framerate
  FPS = recFPS;
  controlFrameTimer(true); // set frametimer
}

static void readSD() {
  // read next cluster from SD for playback
  // One POSIX read() per cluster, straight into the DRAM landing buffer (File::read() is
  // stdio fread() with a 4KB setvbuf buffer, so it reaches FATFS as 4KB refills - measured
  // identical at 1.0MB/s, the limit was never stdio). The skew is what matters: see
  // SD_READ_SKEW at the buffer's declaration
  readLen = 0;
  if (!stopPlayback && playbackFd >= 0) {
    uint32_t rTime = micros();
    ssize_t bytesRead = read(playbackFd, sdReadBuf + SD_READ_SKEW, RAMSIZE);
    uint32_t rUs = micros() - rTime;
    readLen = (bytesRead < 0 || bytesRead > (ssize_t)RAMSIZE) ? 0 : bytesRead; // never hand memcpy an error
    // the device figure - deliberately not added to wTimeTot, see its declaration
    sdReadUs += rUs;
    sdReadCnt++;
    sdReadBytes += readLen;
    LOG_VRB("SD read time %lu us", rUs);
  }
  xSemaphoreGive(readSemaphore); // signal that ready
  // No delay after the give. Upstream slept 10ms here, which serialised every 8KB cluster
  // to 10ms + read time regardless of how fast the consumer drained it: measured as 101ms
  // per frame of consumer wait against 55ms of actual reading, and 7.5fps playback of a
  // 30fps clip. The task blocks on its notify straight after this, so nothing needs the yield
}


void openSDfile(const char* streamFile) {
  // open selected file on SD for streaming
  if (stopPlayback) LOG_WRN("Playback refused - capture in progress");
  else {
    stopPlaying(); // in case already running
    if (iSDbuffer == NULL) {
      LOG_WRN("Playback refused - no PSRAM playback buffer");
      return;
    }
    strcpy(aviFileName, streamFile);
    LOG_INF("Playing %s", aviFileName);
    if (playbackFd >= 0) close(playbackFd); // a force-closed playback can leave one open
    char vfsPath[FILE_NAME_LEN + 8];
    snprintf(vfsPath, sizeof(vfsPath), "/sdcard%s", aviFileName); // SD_MMC mount point, as utilsLog.cpp
    playbackFd = open(vfsPath, O_RDONLY);
    if (playbackFd < 0) {
      // eg a folder shortcut or deleted file was selected. Must bail out here, or
      // readSD() reports a cluster it never read
      LOG_WRN("Playback refused - cannot open %s", vfsPath);
      return;
    }
    lseek(playbackFd, AVI_HEADER_LEN, SEEK_SET); // skip over header
    playbackFPS(aviFileName);
    isPlaying = true; //playback status
    doPlayback = true; // control playback
    sdReadUs = sdReadCnt = sdReadBytes = 0; // before the prime read, which getNextFrame(true) would miss
    readSD(); // prime playback task
  }
}

mjpegStruct getNextFrame(bool firstCall) {
  // get next cluster on demand when ready for opened avi
  mjpegStruct mjpegData = {0, 0, 0};
  static bool remainingBuff;
  static bool completedPlayback; // indicates that playback completed 
  static size_t buffOffset;
  static uint32_t hTimeTot;
  static uint32_t tTimeTot;
  static uint32_t hTime;
  static size_t remainingFrame;
  static size_t buffLen;
  static bool skippingAudio; // stepping over an interleaved audio chunk
  const uint32_t dcVal = 0x63643030; // value of 00dc marker
  const uint32_t wbVal = 0x62773130; // value of 01wb marker
  if (firstCall) {
    sTime = millis();
    hTime = millis();
    remainingBuff = completedPlayback = skippingAudio = false;
    frameCnt = remainingFrame = vidSize = buffOffset = 0;
    wTimeTot = fTimeTot = hTimeTot = tTimeTot = 1; // avoid divide by 0
  }
  LOG_VRB("http send time %lu ms", millis() - hTime);
  hTimeTot += millis() - hTime;
  uint32_t mTime = millis();
  if (!stopPlayback) {
    // continue sending out frames
    if (!remainingBuff) {
      // load more data from SD
      mTime = millis();
      // move final bytes to buffer start in case jpeg marker at end of buffer
      memcpy(iSDbuffer, iSDbuffer + RAMSIZE, CHUNK_HDR);
      xSemaphoreTake(readSemaphore, portMAX_DELAY); // wait for read from SD card completed
      buffLen = readLen;
      LOG_VRB("SD wait time %lu ms", millis() - mTime);
      wTimeTot += millis() - mTime;
      mTime = millis();
      // overlap buffer by CHUNK_HDR to prevent jpeg marker being split between buffers
      memcpy(iSDbuffer + CHUNK_HDR, sdReadBuf + SD_READ_SKEW, buffLen); // load new cluster from the DRAM landing buffer
      LOG_VRB("memcpy took %lu ms for %u bytes", millis() - mTime, buffLen);
      fTimeTot += millis() - mTime;
      remainingBuff = true;
      if (buffOffset > RAMSIZE) buffOffset = 4; // special case, marker overlaps end of buffer
      else buffOffset = frameCnt ? 0 : CHUNK_HDR; // only before 1st frame
      xTaskNotifyGive(playbackHandle); // wake up task to get next cluster - sets readLen
    }
    mTime = millis();
    if (!remainingFrame) {
      // at start of a chunk marker - either a video frame or an interleaved audio chunk
      uint32_t inVal;
      memcpy(&inVal, iSDbuffer + buffOffset, 4);
      if (inVal != dcVal && inVal != wbVal) {
        // reached end of chunks to stream
        mjpegData.buffLen = buffOffset; // remainder of final jpeg
        mjpegData.buffOffset = 0; // from start of buff
        mjpegData.jpegSize = 0;
        stopPlayback = completedPlayback = true;
        return mjpegData;
      } else {
        // get chunk size
        uint32_t chunkSize;
        memcpy(&chunkSize, iSDbuffer + buffOffset + 4, 4);
        remainingFrame = chunkSize;
        vidSize += chunkSize; // all bytes read off SD, for the read speed stat
        buffOffset += CHUNK_HDR; // skip over marker
        skippingAudio = (inVal == wbVal);
        if (skippingAudio) mjpegData.jpegSize = 0; // audio is stepped over, never sent on
        else {
          mjpegData.jpegSize = chunkSize; // signal start of jpeg to webServer
          mTime = millis();
          // wait on playbackSemaphore for rate control. Video frames only - letting an
          // audio chunk take a frame's worth of delay would halve the playback rate
          xSemaphoreTake(playbackSemaphore, portMAX_DELAY);
          LOG_VRB("frame timer wait %lu ms", millis() - mTime);
          tTimeTot += millis() - mTime;
          frameCnt++;
          showProgress();
        }
      }
    } else mjpegData.jpegSize = 0; // within frame,
    // determine amount of data to send to webServer
    size_t chunkPart;
    if (buffOffset > RAMSIZE) chunkPart = 0; // special case
    else chunkPart = (remainingFrame > buffLen - buffOffset) ? buffLen - buffOffset : remainingFrame;
    mjpegData.buffOffset = buffOffset; // from here
    remainingFrame -= chunkPart;
    buffOffset += chunkPart;
    mjpegData.buffLen = skippingAudio ? 0 : chunkPart;
    if (skippingAudio) {
      // showPlayback() ends playback on (buffLen == 0 && buffOffset == 0), and a cluster
      // loaded part way through an audio chunk sets buffOffset to 0 - so substitute a non
      // zero value, which the caller never dereferences while buffLen is 0
      if (!mjpegData.buffOffset) mjpegData.buffOffset = CHUNK_HDR;
      if (!remainingFrame) skippingAudio = false; // whole chunk stepped over
    }
    if (buffOffset >= buffLen) remainingBuff = false;
  } else {
    // finished, close SD file used for streaming
    if (playbackFd >= 0) close(playbackFd);
    playbackFd = -1;
    logLine();
    if (!completedPlayback) LOG_INF("Force close playback");
    uint32_t playDuration = (millis() - sTime) / 1000;
    uint32_t totBusy = wTimeTot + fTimeTot + hTimeTot;
    LOG_INF("******** AVI playback stats ********");
    LOG_INF("Playback %s", aviFileName);
    LOG_INF("Recorded FPS %u, duration %lu secs", recFPS, recDuration);
    LOG_INF("Playback FPS %0.1f, duration %lu secs", (float)frameCnt / playDuration, playDuration);
    LOG_INF("Number of frames: %u", frameCnt);
    if (frameCnt) {
      // read speed / read time are the reader task inside File::read() - what the card does.
      // wait time is how long the consumer sat on readSemaphore - what the pipeline does
      uint64_t readUs = sdReadUs ? sdReadUs : 1;
      uint32_t readCnt = sdReadCnt ? sdReadCnt : 1;
      LOG_INF("Average SD read speed: %lu kB/s (%lu clusters of %u bytes, %lu us each)",
        (uint32_t)((uint64_t)sdReadBytes * 1000000ULL / 1024ULL / readUs), sdReadCnt, (unsigned)RAMSIZE,
        (uint32_t)(readUs / readCnt));
      LOG_INF("Average frame SD read time: %lu ms", (uint32_t)(readUs / 1000ULL / frameCnt));
      LOG_INF("Average frame SD wait time: %lu ms", wTimeTot / frameCnt);
      LOG_INF("Average frame processing time: %lu ms", fTimeTot / frameCnt);
      LOG_INF("Average frame delay time: %lu ms", tTimeTot / frameCnt);
      LOG_INF("Average http send time: %lu ms", hTimeTot / frameCnt);
      LOG_INF("Busy: %lu%%", min(100 * totBusy / (totBusy + tTimeTot), (uint32_t)100));
    }
    checkMemory();
    LOG_INF("*************************************\n");
    setFPS(saveFPS); // realign with browser
    stopPlayback = isPlaying = false;
    mjpegData.buffLen = mjpegData.buffOffset = 0; // signal end of jpeg
  }
  hTime = millis();
  delay(1);
  return mjpegData;
}

void stopPlaying() {
  if (isPlaying) {
    // force stop any currently running playback
    stopPlayback = true;
    // wait till stopped cleanly, but prevent infinite loop
    uint32_t timeOut = millis();
    while (doPlayback && millis() - timeOut < MAX_FRAME_WAIT) delay(10);
    if (doPlayback) {
      // not yet closed, so force close
      logLine();
      LOG_WRN("Force closed playback");
      doPlayback = false; // stop webserver playback
      setFPS(saveFPS);
      xSemaphoreGive(playbackSemaphore);
      xSemaphoreGive(readSemaphore);
      delay(200);
    }
    stopPlayback = false;
    isPlaying = false;
  }
}

static void playbackTask(void* parameter) {
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    readSD();
  }
  vTaskDelete(NULL);
}

/******************* Startup ********************/

static bool startSDtasks() {
  // tasks to manage SD card operation
  xTaskCreateWithCaps(&playbackTask, "playbackTask", PLAYBACK_STACK_SIZE, NULL, PLAY_PRI, &playbackHandle, STACK_MEM);
  xTaskCreate(&captureTask, "captureTask", CAPTURE_STACK_SIZE, NULL, CAPTURE_PRI, &captureHandle);
  if (captureHandle == NULL) {
    // Usually insufficient memory
    OTAprereq();
    return false;
  }
  // set initial camera framesize and FPS from configs. sensorFS tracks what the sensor is
  // actually set to, and it stays at the capture size in every state now
  sensor_t * s = esp_camera_sensor_get();
  framesize_t bootFS = (framesize_t)fsizePtr;
  s->set_framesize(s, hwFrameSize(bootFS));
  applySensorTuning(s, bootFS); // or the board runs on the driver's geometry until a size change
  sensorFS = bootFS;
  captureFPS = FPS; // whatever the config loaded is the user's capture rate
  setFPS(FPS);
  debugMemory("startSDtasks");
  return true;
}

bool prepRecording() {
  // initialisation & prep for AVI capture
  readSemaphore = xSemaphoreCreateBinary();
  playbackSemaphore = xSemaphoreCreateBinary();
  aviMutex = xSemaphoreCreateMutex();
  motionSemaphore = xSemaphoreCreateBinary();
  for (int i = 0; i < vidStreams; i++) frameSemaphore[i] = xSemaphoreCreateBinary();
  // consumer side of the playback double buffer - PSRAM, see the declaration. Playback
  // is refused if this fails; recording does not depend on it
  if (iSDbuffer == NULL) iSDbuffer = (uint8_t*)ps_malloc(RAMSIZE + CHUNK_HDR);
  if (iSDbuffer == NULL) LOG_ERR("Failed to allocate %u byte playback buffer from PSRAM", RAMSIZE + CHUNK_HDR);
  reloadConfigs(); // apply camera config
  if (!startSDtasks()) return false;
  // printResetReason() runs before the SD log opens, and the RTC ring that holds its output
  // is overwritten by boot chatter within a minute or two - a lockup found hours later by
  // the reset button would lose its breadcrumb. Say it again here, where the SD log is open
  uint8_t prevStage = previousRestartStage();
  if (prevStage == RESTART_IN_ESP_RESTART) LOG_INF("Previous controlled restart completed every stage");
  else if (prevStage) LOG_WRN("Previous controlled restart HUNG after stage %u (%s) - see doRestart()", prevStage, restartStageName(prevStage));
  if (hadBrownout()) {
    // The supply failed on the previous boot. Recording straight back into a sagging
    // battery is how a boot-record-brownout loop grinds the SD card, so hold off briefly
    // and let the pack settle - long enough to break the loop, short enough that a camera
    // which sagged once is not dead in the field
    holdoffSavedRecording = doRecording;
    holdoffActive = true;
    doRecording = false;
    LOG_ALT("Previous boot ended in a brownout - recording held off for %ds", BROWNOUT_HOLDOFF_SECS);
  }

  if ((fs::LittleFSFS*)&STORAGE == &LittleFS) {
    // prevent recording
    sdFreeSpaceMode = 0;
    sdMinCardFreeSpace = 0;
    doRecording = false;
    sdLog = false;
    useMotion = false;
    LOG_WRN("Recording disabled as no SD card");
  } else {
    LOG_DIA("To record new AVI, do one of:");
    LOG_DIA("- press Start Recording on web page");
#if INCLUDE_PERIPH
    if (pirUse) {
      LOG_DIA("- attach PIR to pin %u", pirPin);
      LOG_DIA("- raise pin %u to 3.3V", pirPin);
    }
#endif
    if (useMotion) LOG_DIA("- move in front of camera");
  }
  logLine();
  debugMemory("prepRecording");
  return true;
}

void appShutdown() {
  // A recording open at restart time used to be orphaned: this function did nothing about
  // it and processFrame never got another pass, so a plain web reset or an OTA left an
  // unplayable AVITEMP behind with no power failure involved at all. Ask the capture task
  // to land it and wait briefly - closing from here would race the task that owns the
  // file, and an orphan is recoverable at next boot anyway (recoverAvi)
  if (isCapturing) {
    shutdownPending = true;
    doRecording = false;
    uint32_t waitStart = millis();
    while (isCapturing && millis() - waitStart < 1500) delay(50);
    if (isCapturing) LOG_WRN("Recording still open at shutdown - leaving it for boot recovery");
    else LOG_INF("Recording closed cleanly for shutdown");
  }
  // nothing else to flush on shutdown - motion recordings are closed by processFrame().
  // The sensor, however, must not carry tuned state across a soft restart: the ESP resets but
  // the OV5640 keeps power, and with the PLL retimed by the HD profile the next boot's camera
  // probe can intermittently read a garbage PID and fail 0x106 ESP_ERR_NOT_SUPPORTED (observed
  // after an OTA with hdProfile active; the retry boot recovered). 0x3008[7] is the sensor's
  // software reset - all registers return to power-on defaults, so the probe always sees a
  // factory-state part. Harmless when the state was stock
  sensor_t* s = esp_camera_sensor_get();
  if (s != NULL && s->set_reg != NULL) {
    s->set_reg(s, 0x3008, 0xFF, 0x82); // software reset, then normal run state
    delay(10);
  }
}

static void deleteTask(TaskHandle_t& thisTaskHandle) {
  // hangs if try deleting null thisTaskHandle
  if (thisTaskHandle != NULL) {
    // Unsubscribe from the task watchdog first. vTaskDelete does not (the FreeRTOS
    // pre-deletion hook is off in this core), so captureTask's entry outlived the task
    // through every OTA and, never fed again, tripped the 60s watchdog on any transfer
    // that outlasted it - the two slow-link OTAs of 2 Sep died ~60s after OTAprereq, task
    // WDT with IDLE0 running. Fast transfers finished first and never saw it.
    // ESP_ERR_NOT_FOUND for tasks that never enrolled is the normal case, ignore it
    esp_task_wdt_delete(thisTaskHandle);
    vTaskDelete(thisTaskHandle);
  }
  thisTaskHandle = NULL;
}

void endTasks() {
  for (int i = 0; i < numStreams; i++) deleteTask(sustainHandle[i]);
  deleteTask(captureHandle);
  deleteTask(playbackHandle);
#if INCLUDE_PERIPH
  deleteTask(DS18B20handle);
  deleteTask(servoHandle);
  deleteTask(stickHandle);
#endif
#if INCLUDE_SMTP
  deleteTask(emailHandle);
#endif
#if INCLUDE_FTP_HFS
  deleteTask(fsHandle);
#endif
#if INCLUDE_TGRAM
  deleteTask(telegramHandle);
#endif
#if INCLUDE_AUDIO
  deleteTask(audioHandle);
#endif
}

void OTAprereq() {
  // stop timer isrs, and free up heap space, or crashes esp32
  // watchdog first: this deliberately stops the frame timer and the ping task, so every enrolled
  // task stops petting and the panic would otherwise fire in the middle of an update
  stopWatchDog();
  // full-attention radio for the transfer: modem sleep makes a 12s upload take minutes
  // and invites the stall watchdog (seen on the bench, 31 Aug). The post-OTA reboot
  // restores the configured sleep setting
  WiFi.setSleep(false);
  doPlayback = forceRecord = false;
  // Close any active streams and let their tasks park at the notify wait before
  // endTasks() deletes them - a sustain task killed mid httpd send dies holding
  // httpd/lwip state, the same hazard class as the capture kill below
  for (int i = 0; i < numStreams; i++) stopSustainTask(i);
  uint32_t quiesceStart = millis();
  while (streamsBusy() && millis() - quiesceStart < 2000) delay(50);
  if (streamsBusy()) LOG_WRN("Stream still active after 2s - proceeding with OTA teardown");
  // Land any open recording BEFORE endTasks() deletes the capture task. That task writes
  // to SD synchronously in saveFrame(), and vTaskDelete() on a task inside a FATFS write
  // orphans the volume mutex - every later storage call then blocks for ever, including
  // doRestart()'s flush_log(), which is why an OTA could be accepted and then never
  // restart. Clearing forceRecord above only stops the NEXT recording, not one in flight,
  // so wait for the capture task to close the file from its own context - the only safe
  // place to do it. The wait is on aviBusy, not isCapturing: isCapturing clears at the
  // decision point in processFrame while the flush, index write and rename are still to
  // come. Bounded: an orphaned AVITEMP is repaired at next boot by recoverAvi(), a wedged
  // board is not
  if (isCapturing || aviBusy) {
    shutdownPending = true;
    doRecording = false;
    uint32_t waitStart = millis();
    while ((isCapturing || aviBusy) && millis() - waitStart < 5000) delay(50);
    if (isCapturing || aviBusy) LOG_WRN("Recording still open after 5s - proceeding, boot recovery will repair it");
    else LOG_INF("Recording landed cleanly before OTA teardown");
  }
  controlFrameTimer(false);
  // The timer is stopped, but a capture pass may be in flight and one latched notify can
  // still start another. vTaskDelete() lands wherever the task happens to be - mid SCCB
  // transaction (motion check, settleSensor) or FATFS call - and the orphaned lock then
  // blocks esp_camera_deinit() or doRestart() for ever: ping alive, HTTP dead, the
  // guaranteed post-OTA restart never runs (wedged COM4 exactly this way, 31 Aug). Wait
  // for the task to park at its notify wait, where it holds nothing, before killing it
  delay(60); // a latched notify starts its burst within a tick of the timer stopping
  uint32_t parkStart = millis();
  while (capturePassBusy && millis() - parkStart < 3000) delay(20);
  bool capParked = !capturePassBusy;
  if (!capParked) LOG_WRN("Capture pass still running after 3s - proceeding, camera deinit skipped");
#if INCLUDE_PERIPH
  setStickTimer(false);
#endif
  stopPing();
  endTasks();
  // an unparked capture task may have died holding the SCCB lock - deinit would then
  // deadlock on it, and the post-OTA reboot re-probes the sensor from scratch anyway
  if (capParked) esp_camera_deinit();
  delay(100);
}

static bool camPower() {
  // dummy - power management not required for XIAO ESP32S3 Sense
  return true;
}

static esp_err_t changeXCLK(camera_config_t config) {
  //since the original setup doesnt create over 20MHz clock, we do it forcefully
  if (config.xclk_freq_hz <= 20 * OneMHz) return ESP_OK;
  esp_err_t res = ESP_OK;
  // Deinitialize the existing LEDC configuration
  ledc_stop(LEDC_LOW_SPEED_MODE, config.ledc_channel, 0);
  delay(5);
  // Configure the LEDC timer
  ledc_timer_config_t ledc_timer = {
    .speed_mode = LEDC_LOW_SPEED_MODE,
    .duty_resolution = LEDC_TIMER_1_BIT,
    .timer_num = config.ledc_timer,
    .freq_hz = (uint32_t)config.xclk_freq_hz,
    .clk_cfg = LEDC_AUTO_CLK
  };
  res = ledc_timer_config(&ledc_timer);
  if (res != ESP_OK) {
    LOG_ERR("Failed to configure timer %s", espErrMsg(res));
    return res;
  }
  // Configure the LEDC channel
  ledc_channel_config_t ledc_channel = {
    .gpio_num = XCLK_GPIO_NUM,
    .speed_mode = LEDC_LOW_SPEED_MODE,
    .channel = config.ledc_channel,
    .intr_type = LEDC_INTR_DISABLE,
    .timer_sel = config.ledc_timer,
    .duty = 1,  // 50% duty cycle for 1-bit resolution
    .hpoint = 0
  };
  res = ledc_channel_config(&ledc_channel);
  if (res != ESP_OK) {
    LOG_ERR("Failed to configure channel %s", espErrMsg(res));
    return res;
  }
  delay(200); // base on datasheet, it needs < 300 ms for configuration to settle in. we just put 200ms. it doesnt hurt.
  return res;
}

/************** OV5640 clock tree diagnostics **************/
// The esp32-camera driver is shipped prebuilt with CONFIG_LOG_MAXIMUM_LEVEL=1, so its own
// "Calculated XVCLK ... PCLK" and "Set PLL" ESP_LOGI lines are compiled out and cannot be
// re-enabled at runtime. Reading the registers back over SCCB is the only way to see what
// the sensor is actually doing. ov5640_regs.h is a private driver header and is not shipped
// with the Arduino core, so the addresses are repeated here.

#define OV5640_SC_PLL_CTRL0  0x3034 // [3:0] MIPI bit mode, 0x1A = 10 bit
#define OV5640_SC_PLL_CTRL1  0x3035 // [7:4] system clock divider
#define OV5640_SC_PLL_CTRL2  0x3036 // [7:0] PLL multiplier
#define OV5640_SC_PLL_CTRL3  0x3037 // [3:0] pre-divider, [4] root divider (2x)
#define OV5640_SC_PLL_CTRL5  0x3039 // [7] PLL bypass
#define OV5640_SC_PLLS_CTRL0 0x3108 // [5:4] PCLK root divider
#define OV5640_SYSREM_RESET  0x3103 // clock source select
#define OV5640_PCLK_DIV      0x3824 // [4:0] DVP PCLK divider
#define OV5640_PCLK_MANUAL   0x460C // 0x22 = manual PCLK divider, 0x20 = auto
#define OV5640_X_ADDR_ST     0x3800 // .. 0x3803 sensor window start
#define OV5640_X_ADDR_END    0x3804 // .. 0x3807 sensor window end
#define OV5640_X_OUTPUT_SIZE 0x3808 // .. 0x380b DVP output size
#define OV5640_X_TOTAL_SIZE  0x380c // .. 0x380f HTS / VTS
#define OV5640_X_OFFSET      0x3810 // .. 0x3813 ISP offset
#define OV5640_X_INCREMENT   0x3814 // horizontal subsample increment
#define OV5640_Y_INCREMENT   0x3815 // vertical subsample increment
#define OV5640_TIMING_TC_R20 0x3820 // vflip / vertical binning
#define OV5640_TIMING_TC_R21 0x3821 // hmirror / horizontal binning
#define OV5640_AEC_PK_VTS    0x350c // .. 0x350d extra frame lines added by AEC
#define OV5640_VFIFO_CTRL00  0x4600 // [5] JPEG fixed height enable (mode 2 only)
#define OV5640_VFIFO_HSIZE   0x4602 // .. 0x4603 JPEG output width
#define OV5640_VFIFO_VSIZE   0x4604 // .. 0x4605 JPEG output height
#define OV5640_JPG_MODE_SEL  0x4713 // [2:0] JPEG mode 1..6, reset default 0x02
// [7] encoder input format, 0 = YUV420, 1 = YUV422. Reported but NOT a usable lever, though not
// for the reason first recorded here: the sensor CAN emit 4:2:0. Datasheet 7.15 gives the
// formatter output format as 0x4300[7:4], where 0x3 is YUV422 (what the driver writes) and 0x4
// is YUV420. Clearing this bit alone only changes what the ENCODER expects while the formatter
// still sends 422, which is a mismatch of our own making.
// Setting both does not work either. With 0x4300=0x4x and 0x4400=0x01, across all eight output
// sequences 0x40-0x46, frames came back ~68KB against a 35.5KB 422 control, every one banded
// with black horizontal lines and visibly stretched vertically. The SOF read 1280x720 with
// Ysamp 0x22 - structurally correct 4:2:0 - so rows are missing from the buffer that arrived,
// not misread chroma. That points at the host: 4:2:0 alternates luma-only and chroma lines, so
// the line structure on the wire changes, and the precompiled driver writes 0x4300 back to 0x30
// at init and lays its DVP capture out for 4:2:2 mode-2 framing. Matching the VFIFO sizes at
// 0x4602-0x4605 to the real output stopped frame delivery entirely.
// So this is a host limit, not a sensor limit, and it is not fixable without driver source.
// Measured at HD in daylight when only this bit was cleared: frames grew from 41.5KB to
// 181-223KB, the rate fell from 32.6fps to 11-13, and ffmpeg reported "overread". It looked
// like a 19.5% saving when first tried against a near-black scene, because an all-zero chroma
// plane decodes the same either way. Restoring 0x81 recovers exactly, same frame and same rate
#define OV5640_JPEG_CTRL00   0x4400
#define OV5640_JFIFO_OVERFLOW 0x4417 // [0] JPEG FIFO overflow, read only - see below
#define OV5640_ISP_CONTROL01 0x5001 // [5] ISP scale enable - decides crop vs downscale
#define OV5640_AEC_PK_EXPOSURE 0x3500 // .. 0x3502 exposure, [19:4] lines and [3:0] fraction
#define OV5640_AEC_PK_REAL_GAIN 0x350a // .. 0x350b AGC gain actually applied, 0x350b[7:0] x/16
#define OV5640_AEC_CTRL00    0x3a00 // [2] night mode enable - extends the frame in the dark
#define OV5640_AEC_GAIN_CEIL 0x3a18 // .. 0x3a19 AGC ceiling, real gain format
// AEC limits that have to track the frame timing - see applyAecLimits(). All are driver reset
// table values that nothing recomputes when HTS or VTS move
#define OV5640_AEC_MAX_EXPO  0x3a02 // .. 0x3a03 max exposure in lines, must fit inside VTS
#define OV5640_AEC_B50_STEP  0x3a08 // .. 0x3a09 lines per 50Hz half cycle, [1:0] and [7:0]
#define OV5640_AEC_B60_STEP  0x3a0a // .. 0x3a0b lines per 60Hz half cycle
// NOTE the band-max pair is REVERSED relative to the step pair above: datasheet table 7-9
// puts the 60Hz count at 0x3A0D and the 50Hz count at 0x3A0E. This was shipped swapped for a
// while - harmless only because both counts compute equal at every stock timing
#define OV5640_AEC_MAX_B60   0x3a0d // [5:0] how many B60 steps fit in the frame
#define OV5640_AEC_MAX_B50   0x3a0e // [5:0] how many B50 steps fit in the frame
#define OV5640_AEC_MAX_EXPO_50 0x3a14 // .. 0x3a15 max exposure under the 50Hz filter

static int camReg(sensor_t* s, int reg) {
  // single 8 bit register, -1 on SCCB failure
  return (s == NULL || s->get_reg == NULL) ? -1 : s->get_reg(s, reg, 0xFF);
}

static int camReg16(sensor_t* s, int reg) {
  // big endian register pair, eg HTS in 0x380c/0x380d
  int hi = camReg(s, reg);
  int lo = camReg(s, reg + 1);
  return (hi < 0 || lo < 0) ? -1 : ((hi << 8) | lo);
}

static int senLineFactor(sensor_t* s) {
  // Pixel clocks per line, as a multiple of HTS. A binned readout completes a line in HTS
  // clocks; a full resolution readout takes 2 x HTS. Proven by hardware count on 26 Aug 2026:
  // at FHD with HTS 2060, VTS 1294 and PIXCLK 80MHz the VSYNC pin delivered exactly 15.0fps -
  // half the plain HTS x VTS arithmetic - which pins the full resolution line time at
  // 2 x HTS / PIXCLK (51.5us there). The frameData model always carried this /2 for fps;
  // everything converting between LINES and TIME needs the same factor or it reports 2x off
  // at full resolution: banding steps, exposure milliseconds, frame period
  if (s == NULL || s->get_reg == NULL) return 1;
  int xInc = s->get_reg(s, 0x3814, 0xFF); // [7:4] odd + [3:0] even increment, ratio is the mean
  if (xInc < 0) return 1; // unreadable: assume binned, which leaves the verified HD path alone
  return ((((xInc >> 4) & 0x0F) + (xInc & 0x0F)) / 2 >= 2) ? 1 : 2;
}

// The decoded PLL tree, gathered so that everything needing the pixel clock reads the same
// number. It used to be computed inline inside dumpCamRegs(), which was fine while only the
// instrument wanted it - the banding filter now wants it too, and two copies of this arithmetic
// would eventually disagree about the clock while both looked plausible
typedef struct {
  bool valid;                                   // false if the SCCB reads failed - check first
  int r34, r35, mul, r37, r39, r08;             // raw PLL registers, for reporting
  int pclkDiv, pclkMan;
  int sysDiv, preDiv, pclkRoot, pclkRootDiv;
  bool root2x, bypass, pclkManual;
  float preDivVal;
  uint32_t xclk, refin, vco, pllClk, pixClk, pclkDriver;
} camClocks_t;

static camClocks_t camClocks(sensor_t* s) {
  camClocks_t c = {};
  if (s == NULL) return c;
  c.r34 = camReg(s, OV5640_SC_PLL_CTRL0);
  c.r35 = camReg(s, OV5640_SC_PLL_CTRL1);
  c.mul = camReg(s, OV5640_SC_PLL_CTRL2);
  c.r37 = camReg(s, OV5640_SC_PLL_CTRL3);
  c.r39 = camReg(s, OV5640_SC_PLL_CTRL5);
  c.r08 = camReg(s, OV5640_SC_PLLS_CTRL0);
  c.pclkDiv = camReg(s, OV5640_PCLK_DIV);
  c.pclkMan = camReg(s, OV5640_PCLK_MANUAL);
  if (c.mul < 0 || c.r35 < 0 || c.r37 < 0) return c; // valid stays false

  // decode, then recompute the clocks using the driver's own arithmetic (calc_sysclk)
  c.sysDiv = (c.r35 >> 4) & 0x0F;
  if (!c.sysDiv) c.sysDiv = 1;
  c.preDiv = c.r37 & 0x0F;
  c.root2x = (c.r37 & 0x10) ? true : false;
  c.pclkRoot = (c.r08 >> 4) & 0x03;
  c.bypass = (c.r39 & 0x80) ? true : false;
  c.pclkManual = (c.pclkMan == 0x22);
  static const float preDivMap[] = {1, 1, 2, 3, 4, 1.5, 6, 2.5, 8};
  static const int pclkRootMap[] = {1, 2, 4, 8};
  c.preDivVal = preDivMap[c.preDiv > 8 ? 0 : c.preDiv];
  c.pclkRootDiv = pclkRootMap[c.pclkRoot];

  c.xclk = xclkMhz * OneMHz;
  c.refin = (uint32_t)(c.xclk / c.preDivVal);
  c.vco = c.refin * c.mul / (c.root2x ? 2 : 1);
  // Provenance matters here, because the naming used to be wrong and the wrong name misled the
  // whole investigation. This arithmetic is the esp32-camera driver's calc_sysclk() copied
  // verbatim, including the 2/5 and the /4. NEITHER constant comes from the datasheet -
  // section 2.5 gives only the 6-27MHz input range and the 800MHz VCO ceiling, and section 7
  // gives the divider fields without ever chaining them into an equation.
  //
  // The driver calls pllClk/4 "SYSCLK", and this code used to as well. That is a real
  // datasheet term but the wrong one: section 2.5 says "SysClk is for the internal clock of
  // the Image Signal Processing (ISP) block", and nothing ties it to HTS or VTS. What HTS and
  // VTS count is the PIXEL clock - section 6.6 specifies the DVP VSYNC width in 0x470A/0x470B
  // in "PCLK unit" - so the frame period is HTS x VTS pixel clocks and fps = PCLK/(HTS x VTS).
  //
  // So pllClk/4 is named pixClk here because that is what it behaves as, not because the
  // datasheet derives it that way: it computes 50MHz at stock, measurement back-solves 49.7,
  // and table 8-5 puts fPCLK typical at 48MHz and maximum at 96. The driver's own PCLK formula
  // on the next line computes 12.5MHz, four times low, which is why its figure was never
  // usable. Treat pixClk as an identification supported by agreement with table 8-5 and with
  // every measurement taken, not as a proven decode - the divider chain is still unverified
  c.pllClk = c.bypass ? c.xclk : (c.vco / c.sysDiv * 2 / 5); // 2/5 is 10 bit mode per 0x3034[3:0]
  c.pixClk = c.pllClk / 4;
  c.pclkDriver = c.pllClk / c.pclkRootDiv / ((c.pclkManual && c.pclkDiv) ? c.pclkDiv : 2);
  c.valid = true;
  return c;
}

static void applyAecLimits(sensor_t* s) {
  // The AEC's limits are fixed values from the driver's reset table and nothing recomputes them
  // when the timing changes. set_framesize() writes only 0x38xx, and applyHtsFloor() and
  // applyCropWindow() move HTS and VTS underneath them, so they end up describing a frame this
  // board does not run.
  //
  // Two of them are geometry, not preference:
  //  - The banding steps at 0x3A08/0x3A0A are line counts spanning one mains half cycle, so they
  //    are a function of the line time, HTS/pixclk. The driver ships 295 and 246, which are right
  //    for a line time we do not use. At HTS 2060 and 45MHz the correct values are 218 and 182,
  //    ~35% lower. 0x3A00[5] has the banding filter ENABLED, so these are actively wrong rather
  //    than dormant, and exposure quantises to multiples of them.
  //  - The exposure ceiling at 0x3A02 must fit inside the frame. The driver leaves it at 984
  //    lines for every size. At HD, VTS is 744, so the AEC was choosing 885 lines - 36.46ms of a
  //    30.65ms frame - and the sensor clipped it. Measured 0x350C/0x350D stayed zero, so this was
  //    costing the AEC's control loop accuracy rather than costing frame rate; do not expect fps
  //    from this, expect the AEC to stop asking for exposure the frame cannot hold.
  if (s == NULL || s->get_reg == NULL || s->set_reg == NULL) return;
  camClocks_t c = camClocks(s);
  int hts = senReg16(s, OV5640_X_TOTAL_SIZE);
  int vts = senReg16(s, OV5640_X_TOTAL_SIZE + 2);
  // bail rather than guess. A banding step computed from a bad clock is worse than a stale one,
  // because it is wrong in a way that looks deliberate
  if (!c.valid || c.pixClk == 0 || hts < 1 || vts < 8) {
    LOG_WRN("AEC limits not applied: clock %s, HTS %d, VTS %d", c.valid ? "ok" : "undecodable", hts, vts);
    return;
  }
  // line time is HTS clocks binned, 2 x HTS at full resolution - see senLineFactor()
  int lf = senLineFactor(s);
  int b50 = c.pixClk / (100 * hts * lf);    // lines per 50Hz half cycle
  int b60 = c.pixClk / (120 * hts * lf);    // lines per 60Hz half cycle
  // The ceiling is min(the frame, the AEC engine's own range). The engine limit is real and
  // its failure mode is nasty: hand it a max exposure it cannot reach and it stops stepping
  // exposure in band multiples ALTOGETHER - in the dark it parks exposure sub-band and runs
  // gain at 27-30x of the 32x ceiling (column FPN, washed-out color), in the light it picks
  // freeform values that are no multiple of the mains half cycle (rolling flicker bands, the
  // "vertical striping" of 27 Aug 2026). Bisected on hardware at HD/VTS 3698 by moving ONLY
  // 0x3A02/0x3A14 and forcing a re-seek: 1228/1552/1940 all step in exact band multiples at
  // low gain; 2328 collapsed to 280 lines at 15x in a lit scene; 3694 chose 1444, no band
  // multiple. The bracket 1940-2328 agrees with the datasheet features table, "maximum
  // exposure interval: 1964 x tROW", so the spec figure is used. Enabling night mode
  // (0x3A00[2]) instead "fixes" only the dark case, by abandoning band quantisation - which
  // trades the gain-parking for the flicker bands, and is how the first fix went wrong.
  //
  // Two subtleties cost a day between them. The AEC never re-seeks from a stable point - any
  // exposure it holds survives limit writes, mode bits and aec toggles until the scene or a
  // manual perturbation moves yavg out of the stable band, so a bad exposure can outlive the
  // config that caused it and a good test must force a re-seek. And the band-max pair really
  // is reversed relative to the step pair (table 7-9: 60Hz count at 0x3A0D, 50Hz at 0x3A0E) -
  // a swapped count over-promises bands beyond the ceiling, which is the same unsatisfiable
  // config and parked the AEC at fps 19 even though its 1942-line ceiling was in range
  #define AEC_ENGINE_MAX_ROWS 1964
  int maxExp = vts - 4;                     // datasheet 4.6.2: exposure must leave 4 lines
  if (maxExp > AEC_ENGINE_MAX_ROWS) maxExp = AEC_ENGINE_MAX_ROWS;
  // 10 bit fields, and a zero step would make the AEC divide by it
  if (b50 < 1 || b60 < 1 || b50 > 0x3FF || b60 > 0x3FF || maxExp < 1 || maxExp > 0xFFFF) {
    LOG_WRN("AEC limits not applied: B50 %d, B60 %d, max exposure %d out of range", b50, b60, maxExp);
    return;
  }
  bool ok = senWrite16(s, OV5640_AEC_B50_STEP, b50);
  ok &= senWrite16(s, OV5640_AEC_B60_STEP, b60);
  ok &= senWrite16(s, OV5640_AEC_MAX_EXPO, maxExp);
  ok &= senWrite16(s, OV5640_AEC_MAX_EXPO_50, maxExp);
  // read modify write the two count registers, which are [5:0] with the rest reserved.
  // floor(maxExp/step) guarantees count x step <= maxExp, keeping the config satisfiable
  int c50 = s->get_reg(s, OV5640_AEC_MAX_B50, 0xFF);
  int c60 = s->get_reg(s, OV5640_AEC_MAX_B60, 0xFF);
  if (c50 >= 0) s->set_reg(s, OV5640_AEC_MAX_B50, 0x3F, (maxExp / b50) & 0x3F);
  if (c60 >= 0) s->set_reg(s, OV5640_AEC_MAX_B60, 0x3F, (maxExp / b60) & 0x3F);
  // The AEC never re-seeks from a stable point, so an exposure chosen under the PREVIOUS
  // steps outlives every correction above - measured on boot, where convergence starts under
  // the driver's reset-table steps before this runs: it held 885 lines, exactly 3 of the
  // driver's 295-line bands and no multiple of the real 388, which is mild mains flicker held
  // indefinitely on a static scene. If the held exposure is off the current grid, snap it to
  // the nearest band multiple through a brief manual pulse (datasheet 4.6.2: exposure writes
  // need 0x3503[0]); the AEC resumes from the snapped value and trims gain, not exposure,
  // because a band multiple is a point its own stepping can hold
  int e0 = camReg(s, OV5640_AEC_PK_EXPOSURE), e1 = camReg(s, OV5640_AEC_PK_EXPOSURE + 1);
  int e2 = camReg(s, OV5640_AEC_PK_EXPOSURE + 2), bandSel = camReg(s, 0x3C0C);
  if (e0 >= 0 && e1 >= 0 && e2 >= 0 && bandSel >= 0) {
    int expo = ((e0 & 0x0F) << 12) | (e1 << 4) | (e2 >> 4); // [19:4] lines, [3:0] fraction
    int step = (bandSel & 1) ? b50 : b60; // 0x3C0C[0]: the band the engine is quantising to
    // below one band the engine's own auto-band-off applies and freeform is by design
    if (expo > step) {
      int n = (expo + step / 2) / step;
      if (n * step > maxExp) n = maxExp / step;
      int snapped = n * step;
      if (n >= 1 && abs(snapped - expo) > 3) {
        s->set_reg(s, 0x3503, 0x01, 0x01);
        s->set_reg(s, OV5640_AEC_PK_EXPOSURE, 0xFF, (snapped >> 12) & 0x0F);
        s->set_reg(s, OV5640_AEC_PK_EXPOSURE + 1, 0xFF, (snapped >> 4) & 0xFF);
        s->set_reg(s, OV5640_AEC_PK_EXPOSURE + 2, 0xFF, (snapped << 4) & 0xF0);
        s->set_reg(s, 0x3503, 0x01, 0x00);
        LOG_INF("AEC exposure snapped %d -> %d lines (%d x %d line band)", expo, snapped, n, step);
      }
    }
  }
  if (!ok) LOG_WRN("AEC limits did not read back: B50 %d B60 %d maxExp %d", b50, b60, maxExp);
  else LOG_VRB("AEC limits for HTS %d VTS %d: B50 %d, B60 %d, max exposure %d (%d/%d steps)",
    hts, vts, b50, b60, maxExp, maxExp / b50, maxExp / b60);
}

void dumpCamRegs() {
  // report the OV5640 clock tree and frame timing as actually programmed
  sensor_t* s = esp_camera_sensor_get();
  if (s == NULL) {
    LOG_WRN("dumpCam: no camera sensor");
    return;
  }
  camClocks_t c = camClocks(s);
  if (!c.valid) {
    LOG_WRN("dumpCam: SCCB read failed - camera may be unresponsive");
    return;
  }
  // local aliases, so the log statements below read as they always did
  int r34 = c.r34, r35 = c.r35, mul = c.mul, r37 = c.r37, r39 = c.r39;
  int r08 = c.r08, pclkDiv = c.pclkDiv, pclkMan = c.pclkMan;
  int sysDiv = c.sysDiv, preDiv = c.preDiv, pclkRoot = c.pclkRoot;
  bool root2x = c.root2x, bypass = c.bypass, pclkManual = c.pclkManual;
  float preDivVal = c.preDivVal;
  uint32_t refin = c.refin, vco = c.vco, pllClk = c.pllClk;
  uint32_t pixClk = c.pixClk, pclkDriver = c.pclkDriver;

  int hts = camReg16(s, OV5640_X_TOTAL_SIZE);
  int vts = camReg16(s, OV5640_X_TOTAL_SIZE + 2);
  // AEC extends the frame past VTS to buy exposure time, and in auto mode it moves on its
  // own - datasheet 4.6.2 puts the maximum exposure at {0x380E,0x380F} + {0x350C,0x350D}.
  // Reading VTS alone therefore understates the frame period exactly when the light is poor,
  // which is when the rate actually drops, so the ceiling is derived from the sum
  int vtsExtra = camReg16(s, OV5640_AEC_PK_VTS);
  if (vtsExtra < 0) vtsExtra = 0;
  int vtsEff = vts + vtsExtra;
  // subsample increment is [7:4] odd and [3:0] even, the ratio being their mean: 0x11 is a
  // full readout, 0x31 is 2x. Binned rows sustain twice what a full resolution readout does at
  // the same pixel clock, so the ratio is reported next to the rate rather than folded into
  // it - picking one here would bake an unproven theory into the instrument
  int xInc = camReg(s, OV5640_X_INCREMENT);
  int yInc = camReg(s, OV5640_Y_INCREMENT);
  float xBin = (xInc < 0) ? 0.0f : (float)(((xInc >> 4) & 0x0F) + (xInc & 0x0F)) / 2.0f;
  float yBin = (yInc < 0) ? 0.0f : (float)(((yInc >> 4) & 0x0F) + (yInc & 0x0F)) / 2.0f;
  // a full resolution line costs 2 x HTS clocks (senLineFactor() - hardware-proven via the
  // VSYNC count), so the frame clock count, the fps ceiling and the exposure milliseconds
  // below all carry the factor; without it every full resolution figure here read 2x optimistic
  int lineFactor = senLineFactor(s);
  float frameClks = (float)hts * vtsEff * lineFactor;
  bool haveTiming = (hts > 0 && vtsEff > 0);
  float fpsPix = haveTiming ? (float)pixClk / frameClks : 0.0;
  float fpsDriverPclk = haveTiming ? (float)pclkDriver / frameClks : 0.0;
  int jpgMode = camReg(s, OV5640_JPG_MODE_SEL);
  int vfifo00 = camReg(s, OV5640_VFIFO_CTRL00);

  LOG_DIA("******** OV5640 clock tree ********");
  LOG_DIA("Frame size: %s, XCLK %uMHz (fixed)", frameData[sensorFS].frameSizeStr, xclkMhz);
  LOG_DIA("PLL regs: 0x3034=0x%02X 0x3035=0x%02X 0x3036=%d 0x3037=0x%02X 0x3039=0x%02X",
    r34, r35, mul, r37, r39);
  LOG_DIA("PCLK regs: 0x3108=0x%02X 0x3824=%d 0x460C=0x%02X 0x3103=0x%02X",
    r08, pclkDiv, pclkMan, camReg(s, OV5640_SYSREM_RESET));
  LOG_DIA("Decoded: mul=%d sys_div=%d pre_div=%d(/%.1f) root_2x=%d pclk_root=%d(/%d) pclk_manual=%d pclk_div=%d bypass=%d",
    mul, sysDiv, preDiv, preDivVal, root2x, pclkRoot, c.pclkRootDiv, pclkManual, pclkDiv, bypass);
  LOG_DIA("Clocks: REFIN %.2fMHz, VCO %.1fMHz, PLL_CLK %.2fMHz, PIXCLK %.2fMHz (table 8-5: typ 48, max 96), driver PCLK %.2fMHz (4x low, do not use)",
    refin / 1000000.0, vco / 1000000.0, pllClk / 1000000.0, pixClk / 1000000.0, pclkDriver / 1000000.0);
  // table 8-5 caps the parallel port pixel clock at 96MHz. The measured cliff on this part sits
  // between 88 and 92MHz on the pixClk basis, which is close enough to that limit to be the
  // likely cause rather than a coincidence - worth checking before blaming the pixel array
  if (pixClk > 96 * OneMHz) LOG_WRN("PIXCLK %.1fMHz exceeds the 96MHz maximum in table 8-5 - expect a corrupt image",
    pixClk / 1000000.0);
  // datasheet 2.5 caps the VCO at 800MHz for a 6-27MHz input. Computing more than that means
  // this decode is wrong somewhere, and every clock derived below it inherits the error
  if (vco > 800 * OneMHz) LOG_WRN("Computed VCO %.0fMHz exceeds the 800MHz datasheet maximum - clock decode is suspect",
    vco / 1000000.0);
  // section 6.6 specifies DVP sync widths in "PCLK unit", so HTS x VTS is a count of pixel
  // clocks and the frame period follows from the pixel clock, not from the ISP's SysClk
  LOG_DIA("Timing: HTS %d x%d clocks/line, VTS %d + %d AEC extra = %d effective, %.0f pixel clocks/frame",
    hts, lineFactor, vts, vtsExtra, vtsEff, frameClks);
  LOG_DIA("Ceiling: %.1f fps on PIXCLK basis, %.1f on the driver's PCLK figure (app FPS %u)",
    fpsPix, fpsDriverPclk, FPS);
  // Exposure and gain belong next to the frame timing because they are the same trade: the
  // frame period is the hard ceiling on integration time, so pushing the rate up buys darkness
  // that only gain can pay for. Exposure is in units of line/16 (datasheet 4.6.2), gain is in
  // real gain format where the value is 16x the actual multiplier
  int expRaw = (camReg(s, OV5640_AEC_PK_EXPOSURE) << 16) | camReg16(s, OV5640_AEC_PK_EXPOSURE + 1);
  int gainRaw = camReg16(s, OV5640_AEC_PK_REAL_GAIN) & 0x3FF;
  int gainCeil = camReg16(s, OV5640_AEC_GAIN_CEIL) & 0x3FF;
  int aec00 = camReg(s, OV5640_AEC_CTRL00);
  float expLines = (expRaw < 0) ? 0.0f : (float)(expRaw & 0xFFFFF) / 16.0f;
  float expMs = (pixClk > 0 && hts > 0) ? expLines * hts * lineFactor * 1000.0f / pixClk : 0.0f;
  LOG_DIA("Exposure: %.1f lines = %.2fms of the %.2fms frame, gain %.2fx (ceiling %.2fx), night mode %s",
    expLines, expMs, (fpsPix > 0) ? 1000.0f / fpsPix : 0.0f, gainRaw / 16.0f, gainCeil / 16.0f,
    (aec00 < 0) ? "?" : ((aec00 & 0x04) ? "ON - frame may extend" : "off"));
  // 0x4417[0] is the JPEG FIFO overflow flag, and it is here because it separates two
  // explanations of the same symptom. When the image breaks up at a high pixel clock, either
  // the clock has passed the 96MHz maximum in table 8-5, or the JPEG engine could not keep up
  // and its FIFO overran. Those want opposite responses, and without this flag they look
  // identical from outside.
  //
  // 0x4400[7] is the encoder's input format, and it is reported because it must stay at YUV422.
  // 4:2:0 would carry 1.5 bytes per pixel against 422's 2, which is worth wanting when the SD
  // card is the binding constraint. The sensor does support it - see the note at the define -
  // but the host cannot receive it, so the bit has to stay where it is. Clearing it alone tells
  // the encoder to expect a format the FORMATTER is not sending, because the formatter's own
  // output format lives at 0x4300[7:4] and the driver holds it at YUV422.
  // Measured on a real scene with only this bit cleared: frames went from 35.1KB to 69.8KB,
  // twice the size rather than smaller, because corrupt high frequency content compresses
  // badly. JFIFO overflow stayed clear throughout, so the flag above correctly rules the
  // encoder out and points at the format mismatch instead.
  //
  // Worth recording how this nearly passed. On a near black scene the same setting measured
  // 19.5% SMALLER and looked like a win: the corruption is invisible when every line is black,
  // and the frame really does carry half as many chroma blocks. Every automated check agreed -
  // the JPEG SOF read 0x22/0x11/0x11 which is genuine 4:2:0, all 530 frames decoded, headers
  // matched at both offsets, zero bad frames. It took a lit scene and a person looking at it
  int jpegCtrl = camReg(s, OV5640_JPEG_CTRL00);
  int jfifoOvf = camReg(s, OV5640_JFIFO_OVERFLOW);
  LOG_DIA("JPEG: mode %d (0x4713=0x%02X), 0x4600=0x%02X fixed height %s, VFIFO output %dx%d, input %s, JFIFO overflow %s",
    jpgMode < 0 ? -1 : jpgMode & 0x07, jpgMode, vfifo00, (vfifo00 > 0 && (vfifo00 & 0x20)) ? "on" : "off",
    camReg16(s, OV5640_VFIFO_HSIZE), camReg16(s, OV5640_VFIFO_VSIZE),
    (jpegCtrl < 0) ? "?" : ((jpegCtrl & 0x80) ? "YUV422" : "YUV420"),
    (jfifoOvf < 0) ? "?" : ((jfifoOvf & 0x01) ? "YES - encoder could not keep up" : "no"));
  int xSt = camReg16(s, OV5640_X_ADDR_ST), ySt = camReg16(s, OV5640_X_ADDR_ST + 2);
  int xEnd = camReg16(s, OV5640_X_ADDR_END), yEnd = camReg16(s, OV5640_X_ADDR_END + 2);
  int xOff = camReg16(s, OV5640_X_OFFSET), yOff = camReg16(s, OV5640_X_OFFSET + 2);
  int isp01 = camReg(s, OV5640_ISP_CONTROL01);
  LOG_DIA("Window: start %d,%d end %d,%d output %dx%d offset %d,%d",
    xSt, ySt, xEnd, yEnd,
    camReg16(s, OV5640_X_OUTPUT_SIZE), camReg16(s, OV5640_X_OUTPUT_SIZE + 2),
    xOff, yOff);
  // datasheet 4.2: frame rate follows the ISP input size - what is actually read off the pixel
  // array - and not the output size. "Typically, the larger the ISP input size is, the less
  // maximum frame rate can be reached". The driver never crops: set_framesize() programs the
  // window from a fixed ratio_table row and leaves the scaler to shrink whatever that reads,
  // so at FHD these two lines disagree by 2624x1472 against 1920x1080. Pre-scaling size is the
  // input less twice the offset (datasheet figure 4-3); when it equals the output size the
  // scaler is doing nothing and 0x5001[5] should be clear
  LOG_DIA("ISP: input %dx%d, pre-scale %dx%d, scale %s (0x5001=0x%02X)",
    xEnd - xSt + 1, yEnd - ySt + 1, xEnd - xSt + 1 - 2 * xOff, yEnd - ySt + 1 - 2 * yOff,
    (isp01 < 0) ? "?" : ((isp01 & 0x20) ? "on" : "off"), isp01);
  LOG_DIA("Subsample: 0x3814=0x%02X 0x3815=0x%02X (%.1fx by %.1fx) 0x3820=0x%02X 0x3821=0x%02X",
    xInc, yInc, xBin, yBin, camReg(s, OV5640_TIMING_TC_R20), camReg(s, OV5640_TIMING_TC_R21));
  // This is the ESP32-S3's own on-die sensor (utils.cpp readInternalTemp, configured for a
  // 20-100C band), NOT the camera - it sits in this dump only because it is the one thermal
  // number we have. The OV5640 exposes no temperature register at all, and its datasheet
  // table 8-2 puts stable image quality at 0-50C junction against -30 to +70C for merely
  // functioning. So a hot S3 reading is a warning about a part we cannot actually measure
  LOG_DIA("ESP32-S3 die temp: %.1fC (not the camera - OV5640 has no sensor, stable to 50C), light %u%%, free heap %s, free PSRAM %s",
    readInternalTemp(), lightLevel, fmtSize(ESP.getFreeHeap()), fmtSize(ESP.getFreePsram()));
  LOG_DIA("**********************************");
}

void setSubSample(const char* csv) {
  // Debug probe for the readout-row lever. QVGA, VGA and 1280X960 all read the SAME window -
  // 2624x1952 subsampled 2x2 to 1312x976, VTS 984 - and only differ in what the ISP scaler
  // does afterwards, which is why all three cap at 39.47fps. 1280X960 genuinely needs its 960
  // rows, but QVGA and VGA are paying a full 1280x960 readout and then throwing pixels away,
  // so reading fewer rows should raise their ceiling in proportion.
  //
  // csv is <yFactor>[,<vts>[,<xFactor>]]. Datasheet table 4-2: 0x3814 TIMING X INC and 0x3815
  // TIMING Y INC, each [7:4] odd increment and [3:0] even increment, default 0x11. The factor
  // is the mean of the two, and the driver's own tables use even=1, so 1x is 0x11 and the
  // current 2x is 0x31, making 4x 0x71 by the same encoding.
  //
  // DEAD END - MEASURED 3 Sep 2026, KEPT SO NOBODY REPEATS IT. The idea was that reading
  // fewer rows shortens the frame. On this part it does not, because the saving is paid
  // straight back in binning time:
  //   QVGA  4x, VTS 496: registers took (0x3815 reads back 0x71), predicted 78.3fps,
  //                      VSYNC measured 39.1 against a 39.5 baseline - no gain at all.
  //                      xclkStat back-solved the implied clock at 39.99MHz against 80.00
  //                      computed, exactly half, which is the tell.
  //   VGA   4x, VTS 496: sensor stopped producing frames entirely, still returned 0 bytes.
  // The factor of two is datasheet section 3.2: "vertical binning will automatically turn on
  // when in vertical-subsampled formats", and binning AVERAGES row pairs before the ADC
  // rather than skipping them. So 4x halves the row count and doubles the per-row cost, and
  // the two cancel exactly. VGA additionally does not fit: 1952 rows at 4x leaves 488 read,
  // and its 480-row output does not survive the offsets on top of that.
  // Both sizes recover fully on the next framesize change, which reloads the register block.
  //
  // So QVGA, VGA and 1280X960 all remain capped at 39.47fps, and the only lever left is the
  // pixel clock, which helps 1280X960 alone (80 -> 85.33MHz is mul 128, but the measured
  // image cliff is 88-92, so it is thin margin for ~2.6fps).
  //
  // VTS must come down with the rows or nothing changes anyway - the frame period is
  // HTS x VTS whatever the readout does, so leaving VTS at 984 just adds blanking.
  sensor_t* s = esp_camera_sensor_get();
  if (s == NULL) return;
  char buf[32];
  strncpy(buf, csv, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = 0;
  char* tok = strtok(buf, ",");
  int yF = tok ? (int)strtol(tok, NULL, 0) : 0;
  tok = strtok(NULL, ","); int vts = tok ? (int)strtol(tok, NULL, 0) : 0;
  tok = strtok(NULL, ","); int xF = tok ? (int)strtol(tok, NULL, 0) : 0;
  if (yF < 1 || yF > 8) { LOG_WRN("subSample: yFactor %d out of range 1-8", yF); return; }
  int yInc = (((2 * yF - 1) & 0x0F) << 4) | 0x01;
  s->set_reg(s, OV5640_Y_INCREMENT, 0xFF, yInc);
  if (xF >= 1 && xF <= 8) s->set_reg(s, OV5640_X_INCREMENT, 0xFF, (((2 * xF - 1) & 0x0F) << 4) | 0x01);
  if (vts >= 8 && !senWrite16(s, 0x380E, vts)) LOG_WRN("subSample: VTS %d did not read back", vts);
  delay(50);
  applyAecLimits(s); // banding steps and the exposure ceiling both follow VTS
  int gotY = s->get_reg(s, OV5640_Y_INCREMENT, 0xFF), gotX = s->get_reg(s, OV5640_X_INCREMENT, 0xFF);
  int hts = senReg16(s, 0x380C), gotVts = senReg16(s, 0x380E), lf = senLineFactor(s);
  camClocks_t c = camClocks(s);
  float fps = (c.valid && hts > 0 && gotVts > 0) ? (float)c.pixClk / ((float)hts * lf * gotVts) : 0;
  LOG_ALT("subSample: 0x3814=0x%02X 0x3815=0x%02X (%.1fx by %.1fx), HTS %d x%d, VTS %d -> %.2ffps predicted",
    gotX, gotY, (((gotX >> 4) & 0x0F) + (gotX & 0x0F)) / 2.0f, (((gotY >> 4) & 0x0F) + (gotY & 0x0F)) / 2.0f,
    hts, lf, gotVts, fps);
}

void setCamReg(const char* csv) {
  // debug: write one sensor register, for timing experiments such as walking HTS down.
  // csv is addr,value - strtol base 0, so both accept 0x hex or plain decimal.
  // NOTE the write is lost on the next set_framesize(), which reloads the whole register
  // block, so do not change frame size midway through a sweep and expect it to persist
  sensor_t* s = esp_camera_sensor_get();
  if (s == NULL || s->set_reg == NULL) {
    LOG_WRN("camReg: set_reg not available for this sensor");
    return;
  }
  char* end = NULL;
  long reg = strtol(csv, &end, 0);
  if (end == csv || *end != ',') {
    LOG_WRN("camReg: need addr,value eg 0x380C,0x0A - got '%s'", csv);
    return;
  }
  long val = strtol(end + 1, NULL, 0);
  if (reg < 0x3000 || reg > 0x6100 || val < 0 || val > 0xFF) {
    LOG_WRN("camReg: addr must be 0x3000-0x6100 and value 0-255, got 0x%lX,0x%lX", reg, val);
    return;
  }
  s->set_reg(s, (int)reg, 0xFF, (int)val);
  // read back - a rejected SCCB write is otherwise silent
  int got = s->get_reg(s, (int)reg, 0xFF);
  if (got != (int)val) LOG_WRN("camReg: 0x%04lX <- 0x%02lX but reads back 0x%02X", reg, val, got);
  else LOG_INF("camReg: 0x%04lX = 0x%02lX", reg, val);
}

void avgZones(const char* unused) {
  // Diagnostic for the tier-1 motion pre-filter: dump the AEC's own 4x4 zone luminance grid.
  // Section 7.28: the sensor computes sixteen zone averages for its AEC at any resolution and
  // exposes them at 0x5691-0x56A0, with the weighted aggregate YAVG at 0x56A1. Reading them is
  // ~17 SCCB transactions - a scene sample with no frame decode at all.
  //
  // Also reports the two gates the pre-filter must respect:
  //  - AEC stable: section 4.5 defines a deadband - the AEC holds exposure while YAVG sits
  //    inside {0x3A1E .. 0x3A1B}. Zone diffs are only meaningful inside the band; outside it
  //    the AEC is actively re-exposing and every zone moves at once.
  //  - AF idle: a focus hunt refocuses the whole frame, which also moves every zone at once.
  //    FW status codes are from the AF library's app-note derived header.
  sensor_t* s = esp_camera_sensor_get();
  if (s == NULL || s->get_reg == NULL) return;
  uint8_t z[16];
  char zstr[80] = "";
  for (int i = 0; i < 16; i++) {
    int v = s->get_reg(s, 0x5691 + i, 0xFF);
    z[i] = (v < 0) ? 0 : v;
    sprintf(zstr + strlen(zstr), "%3u%s", z[i], (i % 4 == 3) ? " | " : " ");
  }
  int yavg = s->get_reg(s, 0x56A1, 0xFF);
  int bandHi = s->get_reg(s, 0x3A1B, 0xFF);
  int bandLo = s->get_reg(s, 0x3A1E, 0xFF);
  bool aecStable = (yavg >= 0 && yavg >= bandLo && yavg <= bandHi);
  int afStat = -1;
#if INCLUDE_AF
  afStat = ov5640AF.getFWStatus();
#endif
  bool afBusy = (afStat >= 0) && !(afStat == 0x70 || afStat == 0x10); // idle / focused
  LOG_INF("AVG zones: %s YAVG %d band %d..%d %s, AF 0x%02X %s", zstr, yavg, bandLo, bandHi,
    aecStable ? "AEC stable" : "AEC MOVING - zones invalid", afStat,
    afBusy ? "BUSY - zones invalid" : "idle");
}

/************** external DVDD - OV5640 internal regulator bypass **************/

// Datasheet 2.7.2 / table 7-4: a module fed 1.5V DVDD externally should set 0x3031 (SC PWC)
// bit[3] to switch the embedded 1.5V regulator off - otherwise both sources drive the core
// rail in parallel and the regulator burns the difference as heat. The bit is SCCB-only, so
// it can only be set AFTER power-up, and it does not survive a sensor reset: esp_camera_init
// soft-resets the sensor (0x3008[7]) on every boot, so prepCam() re-asserts it each time.
//
// The enable lives in NVS (the WiFi-password store, prefs.cpp) rather than the config file,
// DELIBERATELY: setting this bit on a module WITHOUT the external rail browns the sensor
// core out until a power cycle. NVS is welded to the individual board - it never travels
// with a firmware flash, an SD card, or a config backup - so only a board explicitly
// provisioned with /control?extDVDD=1 (currently COM3 only) ever runs with the bypass.
// The one-time bench measurement is in BOARD_TESTING.md

static bool extDVDDKey() {
  // read-only peek at the provisioning key; default absent = internal regulator
  Preferences p;
  if (!p.begin(APP_NAME, true)) return false;
  bool ext = p.getUChar("extDVDD", 0) != 0;
  p.end();
  return ext;
}

static void applyExtDVDD(sensor_t* s) {
  // switch the internal regulator off, and read back - a silently rejected write would
  // leave the regulator fighting the external rail with nothing in the log to say so
  if (s == NULL || s->set_reg == NULL || s->get_reg == NULL) return;
  s->set_reg(s, 0x3031, 0x08, 0x08);
  int got = s->get_reg(s, 0x3031, 0xFF);
  if (got >= 0 && (got & 0x08)) LOG_INF("External DVDD: internal regulator bypassed");
  else LOG_WRN("External DVDD: bypass bit wrote but reads 0x%02X - regulator still active", got);
}

void setExtDVDD(int val) {
  // provision (1) or deprovision (0) this board's external DVDD key. Enabling also asserts
  // the bit immediately so the first provisioning needs no reboot; disabling leaves the
  // current bit alone (clearing it mid-run is safe either way, but the reboot the user
  // does anyway restores the driver default) - the key controls what happens at boot
  Preferences p;
  if (!p.begin(APP_NAME, false)) {
    LOG_WRN("extDVDD: NVS not available");
    return;
  }
  if (val) {
    p.putUChar("extDVDD", 1);
    p.end();
    LOG_INF("External DVDD key saved for this board");
    sensor_t* s = esp_camera_sensor_get();
    if (s != NULL && s->id.PID == OV5640_PID) applyExtDVDD(s);
  } else {
    p.remove("extDVDD");
    p.end();
    LOG_INF("External DVDD key removed - internal regulator from next boot");
  }
}

void getCamReg(const char* addr) {
  // debug: read one sensor register. The counterpart to setCamReg(), and needed by it - any
  // register holding unrelated bits (0x5001 holds the scale enable next to AWB and the colour
  // matrix) cannot be safely rewritten without seeing what is already there, because set_reg
  // takes a whole byte. strtol base 0, so 0x hex or plain decimal
  sensor_t* s = esp_camera_sensor_get();
  if (s == NULL || s->get_reg == NULL) {
    LOG_WRN("camRegRd: get_reg not available for this sensor");
    return;
  }
  long reg = strtol(addr, NULL, 0);
  if (reg < 0x3000 || reg > 0x6100) {
    LOG_WRN("camRegRd: addr must be 0x3000-0x6100, got 0x%lX", reg);
    return;
  }
  int got = s->get_reg(s, (int)reg, 0xFF);
  if (got < 0) LOG_WRN("camRegRd: 0x%04lX read failed", reg);
  else LOG_INF("camRegRd: 0x%04lX = 0x%02X (%d)", reg, got, got);
}

/************** XCLK and frame rate, measured off the hardware **************/
// Everything else in this file infers the clocks: camClocks() ASSUMES xclkMhz reaches the
// sensor, and PIXCLK is arithmetic on registers. These diagnostics close the loop with two
// independent measurements that share no assumptions with that arithmetic.
#include "soc/ledc_struct.h"
#include "soc/lcd_cam_struct.h"
#include "soc/gpio_struct.h"
#include "soc/gpio_sig_map.h"
#include "hal/gpio_ll.h"
#include "driver/pulse_cnt.h"
#include "esp_timer.h"

static double pcntEdgeHz(int gpio, uint32_t gateMs, int frames) {
  // count rising edges on a pin. frames == 0: free-running gate of gateMs, for MHz-range
  // signals. frames > 0: edge-aligned window of that many events, for slow signals like VSYNC
  // where a fixed gate would quantize badly (gateMs then acts as the timeout).
  //
  // The pin's own configuration is deliberately never touched. pcnt_new_channel() with a real
  // gpio number runs gpio_config(), whose output-enable path reroutes the pad's matrix out
  // signal to plain GPIO - on the XCLK pin that would disconnect the LEDC clock and stop the
  // camera mid-measure. So the channel is created unrouted (edge_gpio_num -1) and the route is
  // made by hand: IO_MUX input-enable (harmless alongside an output) plus a matrix connection
  // into PCNT unit 0 channel 0. Nothing else in this firmware uses PCNT.
  pcnt_unit_handle_t unit = NULL;
  pcnt_channel_handle_t chan = NULL;
  double hz = -1.0;
  pcnt_unit_config_t ucfg = {};
  ucfg.low_limit = -32768;
  ucfg.high_limit = 32767;
  ucfg.flags.accum_count = 1; // 16 bit counter, 20MHz signal: overflow every 1.6ms, accumulate
  if (pcnt_new_unit(&ucfg, &unit) != ESP_OK) {
    LOG_WRN("pcnt: unit alloc failed");
    return -1.0;
  }
  do {
    pcnt_chan_config_t ccfg = {};
    ccfg.edge_gpio_num = -1;  // routed manually below, see comment above
    ccfg.level_gpio_num = -1; // tied high inside the matrix by the driver
    if (pcnt_new_channel(unit, &ccfg, &chan) != ESP_OK) break;
    if (pcnt_channel_set_edge_action(chan, PCNT_CHANNEL_EDGE_ACTION_INCREASE,
      PCNT_CHANNEL_EDGE_ACTION_HOLD) != ESP_OK) break;
    pcnt_channel_set_level_action(chan, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_KEEP);
    if (pcnt_unit_add_watch_point(unit, 32767) != ESP_OK) break; // accumulation trips here
    gpio_ll_input_enable(&GPIO, (gpio_num_t)gpio);
    esp_rom_gpio_connect_in_signal(gpio, PCNT_SIG_CH0_IN0_IDX, false);
    if (pcnt_unit_enable(unit) != ESP_OK) break;
    pcnt_unit_clear_count(unit);
    pcnt_unit_start(unit);
    int count = 0;
    if (frames == 0) {
      int64_t t0 = esp_timer_get_time();
      delay(gateMs);
      pcnt_unit_get_count(unit, &count);
      int64_t gateUs = esp_timer_get_time() - t0;
      if (count > 0 && gateUs > 0) hz = count * 1000000.0 / gateUs;
    } else {
      // wait for an edge so the window opens ON one, then time to the Nth after it
      int64_t deadline = esp_timer_get_time() + (int64_t)gateMs * 1000;
      int c0 = 0;
      int64_t t0 = 0;
      while (esp_timer_get_time() < deadline) {
        pcnt_unit_get_count(unit, &count);
        if (count > 0) { c0 = count; t0 = esp_timer_get_time(); break; }
        delay(1);
      }
      if (t0 > 0) {
        while (esp_timer_get_time() < deadline) {
          pcnt_unit_get_count(unit, &count);
          if (count >= c0 + frames) break;
          delay(1);
        }
        int64_t spanUs = esp_timer_get_time() - t0;
        if (count > c0 && spanUs > 0) hz = (count - c0) * 1000000.0 / spanUs;
      }
    }
    pcnt_unit_stop(unit);
  } while (false);
  // teardown releases the unit for the next call; the matrix route is left pointing at a
  // disabled unit, which reads nothing, and the pad input-enable bit is harmless to keep
  if (unit != NULL) pcnt_unit_disable(unit);
  if (chan != NULL) pcnt_del_channel(chan);
  if (unit != NULL) {
    pcnt_unit_remove_watch_point(unit, 32767);
    pcnt_del_unit(unit);
  }
  return hz;
}

void xclkStat(const char* unused) {
  // ground truth for the clock chain, in three layers that share no assumptions:
  // 1. what the LEDC hardware is programmed to generate (register readback, incl. the
  //    fractional divider bits that decide whether XCLK is cycle-exact or dithered)
  // 2. what frequency actually toggles on the XCLK pad (PCNT hardware count)
  // 3. what frame rate the sensor actually delivers on VSYNC, which x HTS x VTS implies the
  //    real PIXCLK - the one number no register on the ESP32 side can ever report
  if (isCapturing) {
    LOG_WRN("xclkStat: refused while capturing");
    return;
  }
  // 1: despite prepCam passing LEDC_TIMER_1 in the camera config, on the S3 the prebuilt
  // driver never touches LEDC (measured: the whole LEDC block reads zero and uninitialised
  // while XCLK toggles regardless) - XCLK comes from the LCD_CAM peripheral's own camera
  // clock: src / (div_num + div_b/div_a), a genuine fractional divider that dithers between
  // div_num and div_num+1 whenever div_b != 0. The LEDC decode is kept for the one path that
  // does use it, changeXCLK(), which steals the pin for xclkMhz > 20
  uint32_t camSel = LCD_CAM.cam_ctrl.cam_clk_sel; // 0 off, 1 XTAL 40MHz, 2 PLL_D2 240MHz, 3 PLL_F160M 160MHz
  uint32_t divNum = LCD_CAM.cam_ctrl.cam_clkm_div_num;
  uint32_t divA = LCD_CAM.cam_ctrl.cam_clkm_div_a;
  uint32_t divB = LCD_CAM.cam_ctrl.cam_clkm_div_b;
  const char* camSrc = (camSel == 1) ? "XTAL 40MHz" : (camSel == 2) ? "PLL_D2 240MHz"
    : (camSel == 3) ? "PLL_F160M 160MHz" : "DISABLED";
  double camSrcHz = (camSel == 1) ? 40e6 : (camSel == 2) ? 240e6 : (camSel == 3) ? 160e6 : 0;
  double camDiv = divNum + ((divA > 0) ? (double)divB / divA : 0);
  double camHz = (camDiv > 0) ? camSrcHz / camDiv : 0;
  LOG_INF("LCD_CAM clk: src %s, divider %lu + %lu/%lu = %.4f (%s) -> %.3fMHz programmed",
    camSrc, (unsigned long)divNum, (unsigned long)divB, (unsigned long)divA, camDiv,
    divB ? "DITHERED, edges jitter by one src cycle" : "integer, cycle exact", camHz / 1e6);
  uint32_t raw = LEDC.timer_group[0].timer[LEDC_TIMER_1].conf.clock_divider;
  uint32_t dutyRes = LEDC.timer_group[0].timer[LEDC_TIMER_1].conf.duty_resolution;
  uint32_t srcSel = LEDC.conf.apb_clk_sel; // 1 = APB 80MHz, 2 = RC_FAST ~17.5MHz, 3 = XTAL 40MHz
  if (srcSel != 0 && raw != 0) {
    double srcHz = (srcSel == 1) ? 80e6 : (srcSel == 3) ? 40e6 : 17.5e6;
    uint32_t frac = raw & 0xFF;
    double div = raw / 256.0;
    double ledcHz = (dutyRes > 0) ? srcHz / (div * (1 << dutyRes)) : 0;
    LOG_INF("LEDC (changeXCLK path): src sel %lu, divider %.4f (frac %lu/256 - %s), duty res %lu bit -> %.3fMHz",
      (unsigned long)srcSel, div, (unsigned long)frac, frac ? "DITHERED" : "cycle exact",
      (unsigned long)dutyRes, ledcHz / 1e6);
  } else LOG_INF("LEDC: uninitialised (expected at xclkMhz <= 20 - the S3 driver clocks the pin from LCD_CAM)");
  // 2: count the pad itself. PCNT samples on the 80MHz APB clock, reliable to roughly half
  // that, so 20MHz XCLK is in range and 40 is marginal - flagged rather than trusted
  double xclkHz = pcntEdgeHz(XCLK_GPIO_NUM, 100, 0);
  if (xclkHz < 0) LOG_WRN("XCLK count failed");
  else LOG_INF("XCLK counted: %.4fMHz on GPIO%d over 100ms%s", xclkHz / 1e6, XCLK_GPIO_NUM,
    (xclkHz > 35e6) ? " (above ~40MHz PCNT undercounts - treat as a floor)" : "");
  // 3: frame rate off the VSYNC pad, edge aligned over 60 frames (2-3s at these rates)
  double vsyncHz = pcntEdgeHz(VSYNC_GPIO_NUM, 10000, 60);
  if (vsyncHz < 0) {
    LOG_WRN("VSYNC count failed - no frames inside 10s");
    return;
  }
  sensor_t* s = esp_camera_sensor_get();
  int hts = (s != NULL) ? camReg16(s, OV5640_X_TOTAL_SIZE) : -1;
  int vts = (s != NULL) ? camReg16(s, OV5640_X_TOTAL_SIZE + 2) : -1;
  int vtsExtra = (s != NULL) ? camReg16(s, OV5640_AEC_PK_VTS) : 0;
  if (vtsExtra < 0) vtsExtra = 0;
  if (hts > 0 && vts > 0) {
    // full resolution lines cost 2 x HTS clocks (senLineFactor), or the implied figure halves
    int lf = senLineFactor(s);
    double implied = vsyncHz * hts * lf * (vts + vtsExtra);
    LOG_INF("VSYNC counted: %.3ffps over 60 frames -> implied PIXCLK %.2fMHz (HTS %d x%d x VTS %d+%d), vs %.2fMHz computed",
      vsyncHz, implied / 1e6, hts, lf, vts, vtsExtra, camClocks(s).pixClk / 1e6);
  } else LOG_INF("VSYNC counted: %.3ffps over 60 frames (HTS/VTS readback failed, no implied PIXCLK)", vsyncHz);
}

void lencFhd(const char* val) {
  // Experimental A/B control for the LENC-after-crop problem documented in applyCropWindow():
  // the crop shrank the readout window by exactly 4/3 in both axes (2624x1472 -> 1952x1096),
  // so the lens correction grid lands at the wrong radial coordinates and the edges carry a
  // colour/luminance error. Datasheet 5.2 / table 7-29: 0x5842-0x5849 hold the RECIPROCAL of
  // the grid step, 11 bits each, BR channel on a 5x5 grid and G on 6x6. A 4/3 smaller step
  // means a 4/3 LARGER reciprocal. The driver never writes these (power-on defaults 0x012B /
  // 0x018D / 0x018F / 0x0109), but the current values are read and scaled rather than assumed.
  // 1 applies the scaled set, 0 restores what was read before the first apply. Runtime only,
  // lost on reboot - wired into applyTunedTiming()'s FHD path only if the eyeball verdict is favourable.
  // The verdict is necessarily a human one: flat-field corners, before vs after
  static int orig[4] = {-1, -1, -1, -1};
  static const int regs[4] = {0x5842, 0x5844, 0x5846, 0x5848};
  sensor_t* s = esp_camera_sensor_get();
  if (s == NULL || s->set_reg == NULL) {
    LOG_WRN("lencFhd: no sensor");
    return;
  }
  bool apply = (atoi(val) != 0);
  for (int i = 0; i < 4; i++) {
    if (orig[i] < 0) {
      orig[i] = senReg16(s, regs[i]);
      if (orig[i] < 0) {
        LOG_WRN("lencFhd: read of 0x%04X failed, aborting", regs[i]);
        return;
      }
    }
    int want = apply ? ((orig[i] & 0x7FF) * 4 + 1) / 3 : orig[i]; // 4/3, rounded, 11 bit
    if (want > 0x7FF) want = 0x7FF;
    if (!senWrite16(s, regs[i], want)) LOG_WRN("lencFhd: 0x%04X wrote %d but did not read back", regs[i], want);
    else LOG_INF("lencFhd: 0x%04X %d -> %d (%s)", regs[i], orig[i], want, apply ? "scaled 4/3" : "restored");
  }
}

void setCamPll(const char* csv) {
  // apply an arbitrary PLL config, for sweeping at a fixed XCLK
  // csv is the 8 public set_pll() args: bypass,mul,sys_div,root_2x,pre_div,seld5,pclk_manual,pclk_div
  // NOTE the public sensor->set_pll arg order differs from the driver internal set_pll:
  // public is (bypass, mul, sys_div, root_2x, pre_div, seld5, pclk_manual, pclk_div)
  // internal is (bypass, mul, sys_div, pre_div, root_2x, pclk_root_div, pclk_manual, pclk_div)
  sensor_t* s = esp_camera_sensor_get();
  if (s == NULL || s->set_pll == NULL) {
    LOG_WRN("camPll: set_pll not available for this sensor");
    return;
  }
  int bypass, mul, sysDiv, root2x, preDiv, seld5, pclkManual, pclkDiv;
  if (sscanf(csv, "%d,%d,%d,%d,%d,%d,%d,%d", &bypass, &mul, &sysDiv, &root2x,
      &preDiv, &seld5, &pclkManual, &pclkDiv) != 8) {
    LOG_WRN("camPll: need 8 csv values bypass,mul,sys_div,root_2x,pre_div,seld5,pclk_manual,pclk_div - got '%s'", csv);
    return;
  }
  // range check up front - the driver masks mul above 127 silently and only logs at ERROR
  if (mul < 4 || mul > 252 || sysDiv < 0 || sysDiv > 15 || preDiv < 0 || preDiv > 8
      || pclkDiv < 0 || pclkDiv > 31 || seld5 < 0 || seld5 > 3) {
    LOG_WRN("camPll: out of range - mul 4-252, sys_div 0-15, pre_div 0-8, seld5 0-3, pclk_div 0-31");
    return;
  }
  if (mul > 127 && (mul & 1)) LOG_WRN("camPll: mul %d will be masked to %d (only even above 127)", mul, mul & 0xFE);
  LOG_INF("camPll: applying bypass=%d mul=%d sys_div=%d root_2x=%d pre_div=%d seld5=%d pclk_manual=%d pclk_div=%d",
    bypass, mul, sysDiv, root2x, preDiv, seld5, pclkManual, pclkDiv);
  int res = s->set_pll(s, bypass, mul, sysDiv, root2x, preDiv, seld5, pclkManual, pclkDiv);
  if (res) LOG_WRN("camPll: set_pll rejected the config, error %d", res);
  delay(50); // let the PLL relock before reading back
  dumpCamRegs(); // always report what actually landed
}

bool prepCam() {
  // initialise camera depending on model and board
  // frameData mirrors framesize_t and then adds NUM_CUSTOM_FS rows past its end, so the
  // expected total is the enum size plus those. Still catches a real enum/table mismatch
  if (FRAMESIZE_INVALID + NUM_CUSTOM_FS != sizeof(frameData) / sizeof(frameData[0]))
    LOG_ERR("framesize_t entries %d + %d custom != frameData entries %d", FRAMESIZE_INVALID,
      NUM_CUSTOM_FS, sizeof(frameData) / sizeof(frameData[0]));
  if (!camPower()) return false;

  bool res = false;
  // buffer sizing depends on psram size (2M, 4M or 8M)
  // FRAMESIZE_QSXGA = 1MB, FRAMESIZE_UXGA = 375KB (as JPEG)
  // Omnivision camera models
  maxFS = FRAMESIZE_SVGA; // 2M
  if (ESP.getPsramSize() > 5 * ONEMEG) maxFS = FRAMESIZE_QSXGA; // 8M
  else if (ESP.getPsramSize() > 3 * ONEMEG) maxFS = FRAMESIZE_UXGA; // 4M
  // define buffer size depending on maximum frame size available, esp32-camera/driver/cam_hal.c: cam_obj->recv_size
  maxFrameBuffSize = maxAlertBuffSize = frameData[maxFS].frameWidth * frameData[maxFS].frameHeight / 5;
  if (alertBuffer == NULL) alertBuffer = psramFound() ? (byte*)ps_malloc(maxAlertBuffSize) : (byte*)malloc(maxAlertBuffSize);
  LOG_INF("Max frame size for %s PSRAM is %s ", fmtSize(ESP.getPsramSize()), frameData[maxFS].frameSizeStr);

  // configure camera
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_1;
  config.ledc_timer = LEDC_TIMER_1;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = siodGpio;
  config.pin_sccb_scl = siocGpio;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = xclkMhz * OneMHz;
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode = CAMERA_GRAB_LATEST;
  // init with high specs to pre-allocate larger buffers
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.frame_size = maxFS;
  config.jpeg_quality = 10;
  config.fb_count = FB_CNT;
  config.sccb_i2c_port = 0;// using I2C 0. to be sure what port we are using.

  // camera init
  esp_err_t err = ESP_FAIL;
  uint8_t retries = 2;
  while (retries && err != ESP_OK) {
    err = esp_camera_init(&config);
    if (err == ESP_OK) err = changeXCLK(config);
    if (err != ESP_OK) {
      // power cycle the camera, provided pin is connected
#if (defined(PWDN_GPIO_NUM)) && (PWDN_GPIO_NUM > -1) // both checks are needed. if send -1 to digitalWrite, it can cause crash.
      digitalWrite(PWDN_GPIO_NUM, 1);
      delay(100);
      digitalWrite(PWDN_GPIO_NUM, 0);
      delay(100);
#else
      delay(200);
#endif
      retries--;
    }
  }
  uint16_t PID = 0;
  if (err != ESP_OK) snprintf(startupFailure, SF_LEN, STARTUP_FAIL "Camera init error 0x%X:%s on %s", err, espErrMsg(err), CAM_BOARD);
  else {
    sensor_t* s = esp_camera_sensor_get();
    if (s == NULL) snprintf(startupFailure, SF_LEN, STARTUP_FAIL "Failed to access camera data on %s", CAM_BOARD);
    else {
      PID = s->id.PID;
      switch (PID) {
        case (OV2640_PID):
          strcpy(camModel, "OV2640");
          break;
        case (OV3660_PID):
          strcpy(camModel, "OV3660");
          break;
        case (OV5640_PID): {
          strcpy(camModel, "OV5640");
          zoneDetectOK = true; // the 4x4 AEC zone grid (0x5691+) that motion detection reads
          // first, before the AF blob download spends its second: boards provisioned with
          // the NVS key run an external 1.5V DVDD rail, and every boot arrives here with
          // the sensor freshly soft-reset, its internal regulator back on and fighting it
          if (extDVDDKey()) applyExtDVDD(s);
#if INCLUDE_AF
          // enable autofocus for OV5640 if equipped - see https://github.com/0015/ESP32-OV5640-AF
          ov5640AF.start(s);
          uint8_t res = ov5640AF.focusInit();
          if (res == 0) res = ov5640AF.autoFocusMode();
          if (res == 0) LOG_INF("OV5640 Auto Focus available");
          else LOG_WRN("OV5640 Auto Focus fail: %u", res);
#endif
          break;
        }
        case (MEGA_CCM_PID):
          strcpy(camModel, "PY260");
          break;
        default:
          // not recognised
          sprintf(camModel, "PID=0x%X", s->id.PID);
          break;
      }
      // motion detection is the OV5640's zone grid now - say so once when it is absent,
      // rather than letting an armed board silently never trigger
      if (!zoneDetectOK) LOG_WRN("Motion detection unavailable - needs the OV5640 zone grid, sensor is %s", camModel);
      // set frame size to configured value, mapped the same way every other caller maps it.
      // The config can hold an app-only size past the driver's enum (see FS_1280X960), and
      // handing that straight to the driver made it log "Invalid framesize: 25" and clamp to
      // QSXGA, so the frame grabbed below to verify the camera came off a 2560x1920 window
      // rather than the configured one. startSDtasks() put it right afterwards, which is why
      // this only ever showed up as an error line in the boot log
      char fsizePtrStr[4];
      if (retrieveConfigVal("framesize", fsizePtrStr)) s->set_framesize(s, hwFrameSize((framesize_t)(atoi(fsizePtrStr))));
      else s->set_framesize(s, FRAMESIZE_VGA);

      // model specific corrections
      if (PID == OV3660_PID) {
        // initial sensors are flipped vertically and colors are a bit saturated
        s->set_vflip(s, 1);//flip it back
        s->set_brightness(s, 1);//up the brightness just a bit
        s->set_saturation(s, -2);//lower the saturation
      }

      res = true;
    }
  }
  // check that camera data is accessible
  if (res) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb == NULL) {
      // usually a camera hardware / ribbon cable fault
      snprintf(startupFailure, SF_LEN, STARTUP_FAIL "Failed to get camera frame - check camera hardware");
    } else {
      esp_camera_fb_return(fb);
      fb = NULL;
      res = true;
      LOG_INF("Camera model %s ready @ %uMHz", camModel, xclkMhz);
      if (PID == OV5640_PID) dumpCamRegs(); // baseline clock tree in every boot log
      if (dashCamOn) {
        useMotion = false; // continuous recording, so no motion detection
        frameLimit = FPS * dashCamOn * 60; // frameLimit is not changed if FPS later changed without restart
        if (frameLimit > maxFrames) {
          frameLimit = maxFrames;
          LOG_WRN("Max continuous recording time interval is %d mins", frameLimit / FPS / 60);
        } else LOG_INF("Do continuous recording at %d min intervals", dashCamOn);
        forceRecord = true;
      } else frameLimit = maxFrames;
    }
  }
  debugMemory("prepCam");
  return res;
}

#else

// dummies
void appShutdown() {}
void OTAprereq() {}
uint8_t setFPSlookup(uint8_t val) {
  return 0;
}

#endif
