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
// A motion triggered recording now runs for a fixed time rather than for as long as
// movement persists. Detection is suspended while capturing - the sensor is at the user's
// resolution then, and decoding that is what the redesign exists to avoid - so there is
// nothing left to watch for a stop condition. Continuous movement produces a series of
// captureSecs files rather than one long one
int captureSecs = 15; // fixed duration of a motion triggered recording
// audio chunks are written every AUD_CHUNK_MIN bytes rather than every frame, so above
// 4fps the index needs well under 2 entries per frame. Overflowing it now closes the
// recording rather than rebooting, so this is a target rather than a hard ceiling
// Also the self-rescue bound. When the offered byte rate far exceeds the SD bus, HTTP can be
// unreachable for the whole recording, so a stop command may never arrive - the recording
// closing itself is then the only way back. 10000 frames meant up to ~20 minutes of that;
// 3600 caps it at 3 minutes at the 20fps default, ~6-7 under heavy shedding
int maxFrames = 3600; // maximum number of frames in video before auto close

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
// card shows up there as "monitoring time". These isolate the checkMotion() call itself
static uint32_t mTimeTot;  // time actually spent in motion checks, in microseconds
static uint32_t mCheckCnt; // number of motion checks performed
static uint32_t badFrameCnt; // frames rejected as zero length or oversized
static uint32_t fTimeTot; // total frame buffering time
static uint32_t wTimeTot; // total SD write time
static uint32_t oTime; // file opening time
static uint32_t cTime; // file closing time
static uint32_t sTime; // file streaming time
static uint32_t frameInterval; // units of us between frames

// SD card storage
// iSDbuffer is used by the playback path, which treats the upper half as a double
// buffer, so it must stay at (RAMSIZE + CHUNK_HDR) * 2. The AVI capture path has its
// own single buffer so the write block can be enlarged without paying twice for it.
uint8_t iSDbuffer[(RAMSIZE + CHUNK_HDR) * 2];
static uint8_t sdWriteBuf[SD_WRITE_SIZE + CHUNK_HDR];
static size_t highPoint;
static File aviFile;
static char aviFileName[FILE_NAME_LEN];

// SD playback
static File playbackFile;
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
framesize_t sensorFS = MOTION_DETECT_FS;

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

static void openAvi() {
  // derive filename from date & time, store in date folder
  // time to open a new file on SD increases with the number of files already present
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
  haveWav = false;
  highPoint = AVI_HEADER_LEN; // allot space for AVI header
  prepAviIndex();
}

static inline bool doMonitor() {
  // monitor incoming frames for motion. Only ever reached while armed and idle, so there
  // is no separate during-capture rate any more - a recording runs for a fixed captureSecs
  // and detection is suspended for its whole duration
  static uint16_t motionCnt = 0;
  uint16_t checkRate = FPS / moveStartChecks;
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
  // armed and idle is the only state that decodes. With motion detection off the sensor
  // stays at the user's resolution, so a still or a stream is instant rather than paying
  // for a switch it gains nothing from
  if (!useMotion || dashCamOn) return false;
  if (isCapturing) {
    // A capture normally stops detection, because the sensor is at the user's capture
    // resolution then and decoding that is the whole thing this design avoids. That
    // reasoning does not apply when the capture size IS the detection size: no switch
    // happens, and the frames being written to the file are the same frames being analysed.
    // Only taken for Show Motion - an ordinary fixed length capture gains nothing from
    // detection and it would cost ~15% duty cycle for no benefit
    if (!(dbgMotion && (framesize_t)fsizePtr == MOTION_DETECT_FS)) return false;
  }
  // dbgMotion streams the detector's own bitmap to the browser, so detection has to keep
  // running for that view to show anything at all. It is a diagnostic mode, so the live
  // view being at the detection size is the point rather than a cost
  if (dbgMotion) return true;
  return !viewerActive();
}

static inline bool mdAtCaptureActive() {
  // detect at the capture size only when that size is one the decoder is allowed to touch -
  // MOTION_MAX_PIXELS caps at HD because decoding at or above ~1.3MP intermittently hard hangs
  // the board (see appGlobals.h). Above the cap the VGA drop stays, exactly as before
  return mdAtCapture && motionSizeAllowed(fsizePtr);
}

static framesize_t desiredSensorFS() {
  // derived from the same predicate, so the sensor is at the detection size exactly when
  // detection runs - the two cannot drift apart.
  // With mdAtCapture the sensor holds the capture size through idle detection: stills and
  // streams are instant at full resolution, there are no transition glitch frames to flush,
  // and the retimed HD profile is never clobbered by a VGA round trip
  return (detectionActive() && !mdAtCaptureActive()) ? MOTION_DETECT_FS : (framesize_t)fsizePtr;
}

static inline uint8_t desiredFPS(framesize_t forFS) {
  // the detection size runs at its own default; the capture size runs at whatever the user
  // chose, which is what captureFPS preserves across switches
  return (forFS == MOTION_DETECT_FS) ? frameData[MOTION_DETECT_FS].defaultFPS : captureFPS;
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
  // input that makes the scaler a no-op and the output a straight crop
  int needW = frameData[forFS].frameWidth + 2 * xOff;
  int needH = frameData[forFS].frameHeight + 2 * yOff;
  // bigger than the window means there is nothing to crop, which is what leaves QSXGA, 5MP,
  // QHD and WQXGA alone - at those the output already is the whole array
  if (needW > haveW || needH > haveH) return;
  if (needW == haveW && needH == haveH) return;
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
  int isp01 = s->get_reg(s, 0x5001, 0xFF);
  if (isp01 >= 0 && (isp01 & 0x20)) s->set_reg(s, 0x5001, 0xFF, isp01 & ~0x20);
  // A narrower window needs fewer clocks per line. Measured floor is 2060 on a 1984 column
  // crop, 2700 on the uncropped 2624 columns, and 2060 on the 1312 columns of a binned read,
  // which all fit max(2060, columns + 76). Only the FHD point is measured directly; the rule
  // is the conservative reading of the three, and HTS is left alone if it is already lower
  int htsFloor = needW + 76;
  if (htsFloor < HTS_FLOOR) htsFloor = HTS_FLOOR;
  int hts = senReg16(s, 0x380C);
  if (hts > htsFloor && !senWrite16(s, 0x380C, htsFloor)) LOG_WRN("Crop HTS %d did not take", htsFloor);
  LOG_VRB("Cropped %s to %dx%d at %d,%d, VTS %d, HTS %d", frameData[forFS].frameSizeStr,
    needW, needH, newXSt, newYSt, needH + 16, (hts > htsFloor) ? htsFloor : hts);
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
int mdAtCapture = 0; // config: detect motion at the capture size instead of dropping to VGA
volatile bool retimePending = false; // fps changed - capture task retimes on the next frame
// a size or rate chosen MID-RECORDING is deferred here rather than applied, so one AVI keeps
// one geometry and one timing throughout - its header carries a single WxH and fps, and
// settleSensor() used to follow fsizePtr on the next frame regardless of what was consuming
// the stream. Applied by settleSensor() from a quiet state when the recording closes
volatile int pendingFS = -1;
volatile int pendingFPS = -1;

static void applyTunedTiming(sensor_t* s, framesize_t fs) {
  // The generalisation of the retired hdProfile/fhdProfile pair: at every video size, the
  // fps setting IS the sensor's frame timing, hit exactly by computing VTS on the in-spec
  // PLL. Replaces "the timer asks and the sensor delivers whatever the driver's clock tree
  // happens to produce" with timing that agrees by construction.
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
  // investigation ever needs it again - see BOARD_TESTING.md
  uint8_t fps = desiredFPS(fs);
  if (fps < 1) fps = 1;
  s->set_reg(s, 0x3037, 0xFF, 0x03); // pre_div 3, root_2x 0
  s->set_reg(s, 0x3035, 0xFF, 0x11); // sys_div 1
  s->set_reg(s, 0x3036, 0xFF, 120);  // mul -> VCO 800MHz, PIXCLK 80MHz
  delay(50); // let the PLL relock, same settle setCamPll() uses
  int gotMul = s->get_reg(s, 0x3036, 0xFF);
  if (gotMul != 120) {
    LOG_WRN("Tuned timing: PLL mul wrote 120 but reads %d - VTS left alone", gotMul);
    return;
  }
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
  // measured driver value (744 HD, 984 VGA/XGA, 1112 cropped FHD, 1968 QSXGA)
  int ySt = senReg16(s, 0x3802), yEnd = senReg16(s, 0x3806);
  int vtsFloor = vtsNow; // fallback: never lower what is there if the window is unreadable
  if (ySt >= 0 && yEnd > ySt) {
    int rows = yEnd - ySt + 1;
    vtsFloor = (lf == 2) ? rows + 16 : rows / 2 + 8;
  }
  int vts = (int)(80000000UL / ((uint32_t)hts * lf * fps));
  if (vts < vtsFloor) {
    LOG_WRN("Tuned timing: %ufps is beyond the %s ceiling %.1f - delivering the ceiling",
      fps, frameData[fs].frameSizeStr, 80e6 / ((float)hts * lf * vtsFloor));
    vts = vtsFloor;
  }
  if (vts > 0xFFFF) vts = 0xFFFF; // 16 bit register; ~0.02fps floor at HD, never plausible
  if (!senWrite16(s, 0x380E, vts)) LOG_WRN("Tuned timing: VTS %d did not read back", vts);
  else LOG_INF("Tuned timing %s: PIXCLK 80MHz, HTS %d x%d, VTS %d -> %.2ffps",
    frameData[fs].frameSizeStr, hts, lf, vts, 80e6 / ((float)hts * lf * vts));
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
  // ceiling from whatever clock and VTS are in force by the time it reads them back
  if (tunedFps && videoSizeAllowed(fs)) applyTunedTiming(s, fs);
  // last, because it reads back the HTS, VTS and clock the steps above have settled
  applyAecLimits(s);
}

static framesize_t hwFrameSize(framesize_t fs) {
  // custom sizes have no framesize_t of their own, so the driver is given the base size and
  // applySensorTuning() rewrites the registers that differ
  return (fs == FS_1280X960) ? FS_1280X960_BASE : fs;
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
  // it froze sensorFS for the rest of the session: the sensor stopped following fsizePtr and
  // stopped returning to MOTION_DETECT_FS
  if (isPlaying) return;
  if (!isCapturing && (pendingFS >= 0 || pendingFPS >= 0)) {
    // choices made mid-recording, applied now that the clip has closed. Size first with the
    // usual reset to the new size's default rate, then an explicit rate choice on top of it
    if (pendingFS >= 0) {
      fsizePtr = pendingFS;
      captureFPS = frameData[fsizePtr].defaultFPS;
      pendingFS = -1;
    }
    if (pendingFPS >= 0) {
      captureFPS = pendingFPS;
      pendingFPS = -1;
      if (tunedFps) retimePending = true;
    }
  }
  framesize_t want = desiredSensorFS();
  if (want != sensorFS) {
    setSensorSize(want); // runs applySensorTuning, so any pending retime is covered
    retimePending = false;
  } else if (retimePending && !isCapturing) {
    // fps changed at a constant size. Only this task may retime the sensor (the web task
    // cannot - the capture may hold a frame mid-flight), which is why the handler just
    // raises the flag. With the tuned PLL already in force this is a VTS-only change:
    // blanking, no relock. Deferred while capturing so a recording keeps one timing
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

bool motionSizeAllowed(uint8_t fsize) {
  // was fsizePtr > FRAMESIZE_SXGA, which blocked P_HD (0.92MP) even though it is smaller
  // than SXGA (1.31MP) - framesize_t is ordered by neither width nor pixel count. Same
  // pixel limit as video, so anything that can be recorded can also be detected on
  return (uint32_t)frameData[fsize].frameWidth * frameData[fsize].frameHeight <= MOTION_MAX_PIXELS;
}

void dumpMotionStats() {
  // on demand, so the idle (not recording) cost can be measured - that is the steady
  // state load, and it is when doMonitor() runs checks most often
  uint16_t checkRate = FPS / moveStartChecks;
  if (!checkRate) checkRate = 1;
  float perSec = (float)FPS / checkRate;
  LOG_INF("******** Motion detection stats ********");
  // report the size actually being detected on, which is sensorFS, not the capture size
  LOG_INF("Detect size: %s (%ux%u), app FPS %u, enabled: %s", frameData[sensorFS].frameSizeStr,
    frameData[sensorFS].frameWidth, frameData[sensorFS].frameHeight, FPS,
    motionSizeAllowed(sensorFS) ? "yes" : "no - above pixel cap");
  LOG_INF("Capture size: %s, state: %s", frameData[fsizePtr].frameSizeStr, sensorStateStr());
  LOG_INF("moveStartChecks %d -> check every %u frame(s) = %0.1f checks/sec", moveStartChecks, checkRate, perSec);
  if (mCheckCnt) {
    float perCheck = (float)mTimeTot / mCheckCnt / 1000.0f; // us -> ms
    LOG_INF("%lu checks, %lu ms total, %0.2f ms per check", mCheckCnt, mTimeTot / 1000, perCheck);
    LOG_INF("Detection duty cycle: %0.1f%% of wall clock", perCheck * perSec / 10.0f);
  } else LOG_WRN("No checks recorded - motion off, night gated, or size above cap");
  // for tuning motionVal: the peak is the closest any check came to tripping, so a peak
  // well under the threshold means the sensitivity is too low to ever fire on that scene
  LOG_INF("Sensitivity motionVal %.0f -> threshold %d changed pixels, peak seen %d",
    motionVal, motionThreshold, motionPeakChange);
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
  if (!isPlaying && sensorFS != desiredSensorFS()) LOG_WRN("Sensor is %s but state wants %s - settleSensor not reconciling",
    frameData[sensorFS].frameSizeStr, frameData[desiredSensorFS()].frameSizeStr);
  LOG_INF("***************************************");
  mTimeTot = mCheckCnt = badFrameCnt = staleFrameCnt = motionPeakChange = 0; // reset the measurement window
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

static bool closeAvi() {
  // closes the recorded file
  recordingCamMode(false); // unfreeze AWB, restore continuous AF
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
    readLen = writeAviIndex(iSDbuffer, RAMSIZE);
    if (readLen) aviFile.write(iSDbuffer, readLen);
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
  // a motion capture now runs for a fixed captureSecs, so there is no short recording to
  // discard - the only file worth throwing away is one with nothing in it
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
    return true;
  } else {
    // nothing was captured, so there is no usable file to keep
    STORAGE.remove(AVITEMP);
    LOG_WRN("Discarded empty recording after %lu secs", vidDurationSecs);
    return false;
  }
}

static bool zonePreFilter() {
  // Tier 1 of detection at the capture size: decide whether this check needs a JPEG decode at
  // all, from the sensor's own 4x4 zone luminance grid (section 7.28, 0x5691-0x56A0). The
  // sensor computes the grid continuously for its AEC at any resolution, so sampling it is
  // ~20 SCCB reads (~2-3ms) against a 60-90ms HD decode. Returns true when a decode is
  // warranted: zones moved, the periodic floor is due, or the zones cannot be trusted.
  //
  // Two gates make the zones trustworthy, both measured this week:
  //  - AEC deadband (section 4.5): while YAVG sits inside {0x3A1E..0x3A1B} the AEC holds
  //    exposure, and on a static scene the 16 zones are stable to +-1 count. Outside the band
  //    the AEC is re-exposing and every zone moves at once - an optical event, not motion.
  //  - AF state: a focus hunt also moves every zone at once. Codes from the AF library's
  //    app-note derived header: 0x70 idle, 0x10 focused, anything else is a hunt in progress.
  //  When a gate fails the reference is dropped and the decode runs, so gated intervals fall
  //  back to exactly the pre-filter-free behaviour rather than going blind.
  //
  // The reference updates every clean tick, so a very slow mover may never beat ZONE_T against
  // the previous tick - that is what the DECODE_FLOOR is for: an unconditional decode at least
  // every 2s bounds the miss window for anything the grid is too coarse or too slow to see.
  const int ZONE_T = 5;              // per-zone delta to count, 5x the measured noise floor
  const int ZONE_N = 2;              // zones over ZONE_T to trip a decode
  const uint32_t DECODE_FLOOR_MS = 2000;
  static uint8_t refZones[16];
  static bool refValid = false;
  static uint32_t lastDecode = 0;
  sensor_t* s = esp_camera_sensor_get();
  if (s == NULL || s->get_reg == NULL) return true; // cannot read the grid: decode as before
  int yavg = s->get_reg(s, 0x56A1, 0xFF);
  int bandHi = s->get_reg(s, 0x3A1B, 0xFF);
  int bandLo = s->get_reg(s, 0x3A1E, 0xFF);
  bool gatesOk = (yavg >= 0 && yavg >= bandLo && yavg <= bandHi);
#if INCLUDE_AF
  if (gatesOk) {
    uint8_t af = ov5640AF.getFWStatus();
    gatesOk = (af == FW_STATUS_S_IDLE || af == FW_STATUS_S_FOCUSED);
  }
#endif
  if (!gatesOk) {
    refValid = false;
    lastDecode = millis();
    return true;
  }
  uint8_t z[16];
  for (int i = 0; i < 16; i++) {
    int v = s->get_reg(s, 0x5691 + i, 0xFF);
    if (v < 0) { refValid = false; return true; } // SCCB glitch: decode as before
    z[i] = (uint8_t)v;
  }
  bool trip = false;
  if (refValid) {
    int moved = 0;
    for (int i = 0; i < 16; i++) if (abs((int)z[i] - (int)refZones[i]) >= ZONE_T) moved++;
    trip = (moved >= ZONE_N);
  }
  bool hadRef = refValid;
  memcpy(refZones, z, sizeof(refZones));
  refValid = true;
  if (trip || !hadRef || millis() - lastDecode >= DECODE_FLOOR_MS) {
    lastDecode = millis();
    return true;
  }
  return false; // scene quiet by the sensor's own account - skip the decode
}

static boolean processFrame() {
  // get camera frame
  static bool haveMotion = false;
  // whether the open recording was started by motion, and so runs to a fixed frame limit
  // rather than for as long as the record button is held
  static bool motionTriggered = false;
  bool res = true;
  uint32_t dTime = millis();

  camera_fb_t* fb = esp_camera_fb_get();
  if (fb == NULL) return false;
  if (!fb->len || fb->len > maxFrameBuffSize) {
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
    if (!streamBufferSize[i] && streamBuffer[i] != NULL) {
      memcpy(streamBuffer[i], fb->buf, fb->len);
      streamBufferSize[i] = fb->len;
      xSemaphoreGive(frameSemaphore[i]); // signal frame ready for stream
    }
  }
  if (doKeepFrame) {
    keepFrame(fb);
    doKeepFrame = false;
  }

  // Motion detection runs only while armed and idle. Recording, live view, NVR and
  // dashcam all skip the decode entirely, so no frame at the capture resolution ever
  // reaches the decoder - that is what makes the large frame hang unreachable. It is also
  // what makes turning motion detection off genuinely free: the lightLevelOnly path used
  // to return after the decode rather than before it, so it cost the same as a real check
  int reasonId = 0;
  bool prevMotion = haveMotion;
  if (detectionActive()) {
    // with mdAtCapture the decode is skipped whenever the sensor's own zone grid says the
    // scene is quiet - on a skip haveMotion is left untouched, so edges are unaffected
    if (doMonitor() && (!mdAtCaptureActive() || zonePreFilter())) {
      uint32_t mTime = micros(); // micros, as a check can be only a few ms
      // The second argument is the PREVIOUS motion state, which checkMotion() uses to find
      // the start and stop edges - not "are we capturing". Passing a literal false made
      // every check look like a fresh start, so the motion log repeated several times a
      // second and MQTT republished MOTION:ON on every check, while the stop edge could
      // never fire at all
      if (checkMotion(fb, haveMotion)) reasonId = 1; // check 1 in N frames
      mTimeTot += micros() - mTime;
      mCheckCnt++;
#if INCLUDE_PERIPH
      if (pirUse && getPIRval()) reasonId = 2;
#endif
      haveMotion = (reasonId) ? true : false;
    }
  }

  // process motion status
  if (haveMotion && !prevMotion) {
    // Start of movement detection. The frame kept here is the detection frame, so the
    // alert image is at MOTION_DETECT_FS - deliberately, since switching the sensor up
    // just to grab a thumbnail would cost a frame interval for detail an alert image
    // does not need. The still handler reports the real size rather than assuming fsizePtr
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

  // Recording status. A motion triggered capture runs for a fixed captureSecs and is ended
  // by the frame limit, not by movement stopping - detection is suspended for its whole
  // duration, so there is no motion signal left to watch. A forced capture (record button
  // or dashcam) still runs for as long as it is held on
  // doRecording is the Save Capture toggle, and is also cleared when the SD card fills and
  // when there is no card at all. It now gates motion triggered recordings, which it never
  // actually did before: its only read was inside the doMonitor() rate expression, so
  // turning Save Capture off still recorded, and a full card still recorded. The record
  // button bypasses it, being an explicit user action
  bool prevCapture = isCapturing;
  if (!isCapturing) {
    if ((haveMotion && doRecording) || forceRecord) {
      isCapturing = true;
      motionTriggered = haveMotion && !forceRecord;
    }
  } else if (!motionTriggered && !forceRecord) isCapturing = false;

  if (isCapturing && !videoSizeAllowed(fsizePtr)) {
    // refuse the recording, but leave stills running at this size.
    // If a recording was already open this closes it cleanly via the branch below
    if (!prevCapture) LOG_WRN("Video recording unavailable at %s, above the %s cap - stills are unaffected",
      frameData[fsizePtr].frameSizeStr, frameData[maxVideoFS].frameSizeStr);
    isCapturing = forceRecord = motionTriggered = false;
  }
  if (isCapturing && !prevCapture) {
    // New movement has occurred or record button pressed. The frame in hand is a detection
    // frame at MOTION_DETECT_FS, so hand it back and switch the sensor up before opening
    // the file - nothing at the detection size is ever written into a recording. This pass
    // saves no frame; the next notify delivers one at the capture resolution
    esp_camera_fb_return(fb);
    haveMotion = false; // consumed - the frame limit ends this capture now, not movement
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
    setSensorSize((framesize_t)fsizePtr);
    if (!dashCamOn) {
      // FPS now follows the capture size, so derive the limit after the switch
      frameLimit = captureSecs * FPS;
      if (frameLimit > maxFrames) frameLimit = maxFrames;
    }
    openAvi();
    return true;
  }

  if (isCapturing) {
    // capture is ongoing
    showProgress();
    if (frameCnt < frameLimit) {
      dTimeTot += millis() - dTime;
      saveFrame(fb);
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

  // A connected viewer pins the sensor to the capture resolution, which suspends motion
  // recording for as long as it stays connected. That is a big enough behaviour change to
  // say out loud rather than leave the user wondering why nothing recorded
  static bool prevViewer = false;
  bool nowViewer = viewerActive();
  if (nowViewer != prevViewer) {
    prevViewer = nowViewer;
    if (useMotion) LOG_ALT("%s video stream %s - motion recording %s", nvrActive() ? "NVR" : "Browser",
      nowViewer ? "connected" : "disconnected", nowViewer ? "suspended" : "resumed");
  }
  // settle the sensor for whatever state we are now in. No-op when already correct, and
  // safe here because the frame buffer has been handed back above
  settleSensor();
  return res;
}

static void captureTask(void* parameter) {
  // woken by frame timer when time to capture frame
  uint32_t ulNotifiedValue;
  while (true) {
    ulNotifiedValue = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
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
  // set FPS from framesize lookup
  fsizePtr = val;
  captureFPS = frameData[fsizePtr].defaultFPS; // new capture size, so a new default rate
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
  uint32_t rTime = millis();
  // read to interim dram before copying to psram
  readLen = 0;
  if (!stopPlayback && playbackFile) {
    // File::read() returns (size_t)-1 on an invalid file, which would reach
    // memcpy() as a length of SIZE_MAX, so clamp it rather than trusting it
    size_t bytesRead = playbackFile.read(iSDbuffer + RAMSIZE + CHUNK_HDR, RAMSIZE);
    readLen = (bytesRead > RAMSIZE) ? 0 : bytesRead;
    LOG_VRB("SD read time %lu ms", millis() - rTime);
  }
  wTimeTot += millis() - rTime;
  xSemaphoreGive(readSemaphore); // signal that ready
  delay(10);
}


void openSDfile(const char* streamFile) {
  // open selected file on SD for streaming
  if (stopPlayback) LOG_WRN("Playback refused - capture in progress");
  else {
    stopPlaying(); // in case already running
    strcpy(aviFileName, streamFile);
    LOG_INF("Playing %s", aviFileName);
    playbackFile = STORAGE.open(aviFileName, FILE_READ);
    if (!playbackFile) {
      // eg a folder shortcut or deleted file was selected. Must bail out here -
      // File::read() on an invalid file returns (size_t)-1, which readSD() would
      // hand to memcpy() as a length
      LOG_WRN("Playback refused - cannot open %s", aviFileName);
      return;
    }
    playbackFile.seek(AVI_HEADER_LEN, SeekSet); // skip over header
    playbackFPS(aviFileName);
    isPlaying = true; //playback status
    doPlayback = true; // control playback
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
      memcpy(iSDbuffer + CHUNK_HDR, iSDbuffer + RAMSIZE + CHUNK_HDR, buffLen); // load new cluster from double buffer
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
    playbackFile.close();
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
      LOG_INF("Average SD read speed: %lu kB/s", ((vidSize / wTimeTot) * 1000) / 1024);
      LOG_INF("Average frame SD read time: %lu ms", wTimeTot / frameCnt);
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
  // actually set to - processFrame() drops it to MOTION_DETECT_FS on the first frame if
  // the device comes up armed and idle
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
  reloadConfigs(); // apply camera config
  if (!startSDtasks()) return false;

  if ((fs::LittleFSFS*)&STORAGE == &LittleFS) {
    // prevent recording
    sdFreeSpaceMode = 0;
    sdMinCardFreeSpace = 0;
    doRecording = false;
    sdLog = false;
    useMotion = false;
    LOG_WRN("Recording disabled as no SD card");
  } else {
    LOG_INF("To record new AVI, do one of:");
    LOG_INF("- press Start Recording on web page");
#if INCLUDE_PERIPH
    if (pirUse) {
      LOG_INF("- attach PIR to pin %u", pirPin);
      LOG_INF("- raise pin %u to 3.3V", pirPin);
    }
#endif
    if (useMotion) LOG_INF("- move in front of camera");
  }
  logLine();
  debugMemory("prepRecording");
  return true;
}

void appShutdown() {
  // nothing to flush on shutdown - motion recordings are closed by processFrame().
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
  if (thisTaskHandle != NULL) vTaskDelete(thisTaskHandle);
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
  doPlayback = forceRecord = false;
  controlFrameTimer(false);
#if INCLUDE_PERIPH
  setStickTimer(false);
#endif
  stopPing();
  endTasks();
  esp_camera_deinit();
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
#define OV5640_AEC_MAX_B50   0x3a0d // [5:0] how many B50 steps fit in the frame
#define OV5640_AEC_MAX_B60   0x3a0e // [5:0] how many B60 steps fit in the frame
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
  int maxExp = vts - 4;                     // datasheet 4.6.2: exposure must leave 4 lines
  // 10 bit fields, and a zero step would make the AEC divide by it
  if (b50 < 1 || b60 < 1 || b50 > 0x3FF || b60 > 0x3FF || maxExp < 1 || maxExp > 0xFFFF) {
    LOG_WRN("AEC limits not applied: B50 %d, B60 %d, max exposure %d out of range", b50, b60, maxExp);
    return;
  }
  bool ok = senWrite16(s, OV5640_AEC_B50_STEP, b50);
  ok &= senWrite16(s, OV5640_AEC_B60_STEP, b60);
  ok &= senWrite16(s, OV5640_AEC_MAX_EXPO, maxExp);
  ok &= senWrite16(s, OV5640_AEC_MAX_EXPO_50, maxExp);
  // read modify write the two count registers, which are [5:0] with the rest reserved
  int c50 = s->get_reg(s, OV5640_AEC_MAX_B50, 0xFF);
  int c60 = s->get_reg(s, OV5640_AEC_MAX_B60, 0xFF);
  if (c50 >= 0) s->set_reg(s, OV5640_AEC_MAX_B50, 0x3F, (maxExp / b50) & 0x3F);
  if (c60 >= 0) s->set_reg(s, OV5640_AEC_MAX_B60, 0x3F, (maxExp / b60) & 0x3F);
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

  LOG_INF("******** OV5640 clock tree ********");
  LOG_INF("Frame size: %s, XCLK %uMHz (fixed)", frameData[sensorFS].frameSizeStr, xclkMhz);
  LOG_INF("PLL regs: 0x3034=0x%02X 0x3035=0x%02X 0x3036=%d 0x3037=0x%02X 0x3039=0x%02X",
    r34, r35, mul, r37, r39);
  LOG_INF("PCLK regs: 0x3108=0x%02X 0x3824=%d 0x460C=0x%02X 0x3103=0x%02X",
    r08, pclkDiv, pclkMan, camReg(s, OV5640_SYSREM_RESET));
  LOG_INF("Decoded: mul=%d sys_div=%d pre_div=%d(/%.1f) root_2x=%d pclk_root=%d(/%d) pclk_manual=%d pclk_div=%d bypass=%d",
    mul, sysDiv, preDiv, preDivVal, root2x, pclkRoot, c.pclkRootDiv, pclkManual, pclkDiv, bypass);
  LOG_INF("Clocks: REFIN %.2fMHz, VCO %.1fMHz, PLL_CLK %.2fMHz, PIXCLK %.2fMHz (table 8-5: typ 48, max 96), driver PCLK %.2fMHz (4x low, do not use)",
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
  LOG_INF("Timing: HTS %d x%d clocks/line, VTS %d + %d AEC extra = %d effective, %.0f pixel clocks/frame",
    hts, lineFactor, vts, vtsExtra, vtsEff, frameClks);
  LOG_INF("Ceiling: %.1f fps on PIXCLK basis, %.1f on the driver's PCLK figure (app FPS %u)",
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
  LOG_INF("Exposure: %.1f lines = %.2fms of the %.2fms frame, gain %.2fx (ceiling %.2fx), night mode %s",
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
  LOG_INF("JPEG: mode %d (0x4713=0x%02X), 0x4600=0x%02X fixed height %s, VFIFO output %dx%d, input %s, JFIFO overflow %s",
    jpgMode < 0 ? -1 : jpgMode & 0x07, jpgMode, vfifo00, (vfifo00 > 0 && (vfifo00 & 0x20)) ? "on" : "off",
    camReg16(s, OV5640_VFIFO_HSIZE), camReg16(s, OV5640_VFIFO_VSIZE),
    (jpegCtrl < 0) ? "?" : ((jpegCtrl & 0x80) ? "YUV422" : "YUV420"),
    (jfifoOvf < 0) ? "?" : ((jfifoOvf & 0x01) ? "YES - encoder could not keep up" : "no"));
  int xSt = camReg16(s, OV5640_X_ADDR_ST), ySt = camReg16(s, OV5640_X_ADDR_ST + 2);
  int xEnd = camReg16(s, OV5640_X_ADDR_END), yEnd = camReg16(s, OV5640_X_ADDR_END + 2);
  int xOff = camReg16(s, OV5640_X_OFFSET), yOff = camReg16(s, OV5640_X_OFFSET + 2);
  int isp01 = camReg(s, OV5640_ISP_CONTROL01);
  LOG_INF("Window: start %d,%d end %d,%d output %dx%d offset %d,%d",
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
  LOG_INF("ISP: input %dx%d, pre-scale %dx%d, scale %s (0x5001=0x%02X)",
    xEnd - xSt + 1, yEnd - ySt + 1, xEnd - xSt + 1 - 2 * xOff, yEnd - ySt + 1 - 2 * yOff,
    (isp01 < 0) ? "?" : ((isp01 & 0x20) ? "on" : "off"), isp01);
  LOG_INF("Subsample: 0x3814=0x%02X 0x3815=0x%02X (%.1fx by %.1fx) 0x3820=0x%02X 0x3821=0x%02X",
    xInc, yInc, xBin, yBin, camReg(s, OV5640_TIMING_TC_R20), camReg(s, OV5640_TIMING_TC_R21));
  // This is the ESP32-S3's own on-die sensor (utils.cpp readInternalTemp, configured for a
  // 20-100C band), NOT the camera - it sits in this dump only because it is the one thermal
  // number we have. The OV5640 exposes no temperature register at all, and its datasheet
  // table 8-2 puts stable image quality at 0-50C junction against -30 to +70C for merely
  // functioning. So a hot S3 reading is a warning about a part we cannot actually measure
  LOG_INF("ESP32-S3 die temp: %.1fC (not the camera - OV5640 has no sensor, stable to 50C), light %u%%, free heap %s, free PSRAM %s",
    readInternalTemp(), lightLevel, fmtSize(ESP.getFreeHeap()), fmtSize(ESP.getFreePsram()));
  LOG_INF("**********************************");
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
