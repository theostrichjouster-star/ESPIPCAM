// Global MJPEG2SD declarations
//
// s60sc 2021, 2022, 2024

#pragma once
#include "globals.h"

/**************************************************************************
 This build supports only the Seeed XIAO ESP32S3 Sense camera board.
 Select an ESP32S3-based board in the Arduino IDE board manager.
***************************************************************************/

#if defined(CONFIG_IDF_TARGET_ESP32S3)
#define CAMERA_MODEL_XIAO_ESP32S3
#else
#error "This build only supports the Seeed XIAO ESP32S3 Sense - select an ESP32S3 board in the IDE"
#endif

/***************************************************************
  Optional features NOT included by default to reduce heap use 
  To include a particular feature, change false to true
***************************************************************/
#define INCLUDE_FTP_HFS false // ftp.cpp (file upload)
#define INCLUDE_TGRAM false   // telegram.cpp (Telegram app interface)
#define INCLUDE_AUDIO true    // audio.cpp (microphones & speakers) - XIAO Sense has an onboard PDM mic
#define INCLUDE_PERIPH false  // peripherals.cpp (servos, PIR, led etc)
#define INCLUDE_SMTP false    // smtp.cpp (email)
#define INCLUDE_MQTT false    // mqtt.cpp (MQTT)
#define INCLUDE_HASIO false   // mqtt.cpp (Send home assistant discovery messages). Needs INCLUDE_MQTT true

#define INCLUDE_CERTS false   // certificates.cpp (https and server certificate checking)
#define INCLUDE_WEBDAV false  // webDav.cpp (WebDAV protocol)
#define INCLUDE_DS18B20 false // if true, requires INCLUDE_PERIPH and additional libraries: OneWire and DallasTemperature
#define INCLUDE_AF true       // for auto focused equipped OV5640. Requires additional library: OV5640_Auto_Focus_for_ESP32_Camera - XIAO Sense ships with an OV5640
// Stays false, and this is now a measured decision rather than a convenience one.
// It was originally wanted for scaleFactor 4, which the built in decoder rejects - but
// every size motion detection can reach uses 3 or below, so there is nothing to gain
// there. The remaining argument was decode speed, and that argument is weak: picking the
// right scaleFactor took a motion check at VGA to 36.5 ms and 18% duty cycle with the
// built in decoder (see the frameData notes below). esp_new_jpeg would have to beat that
// by a wide margin to be worth it, because it is not in the Arduino library index - it is
// an ESP-IDF component, so enabling this puts a manual install step on every user.
#define INCLUDE_NEW_JPG false // true to use esp_new_jpg library, which must be installed first. Faster but uses more memory

/**************************************************************************/

#define ALLOW_SPACES false  // set true to allow whitespace in configs.txt key values

// web server ports 
#define HTTP_PORT 80 // insecure app access
#define HTTPS_PORT 443 // secure app access

#define USE_IP6 false // if true use IPv6 when available, else use IPv4


/*********************** Fixed defines leave as is ************************/ 
/** Do not change anything below here unless you know what you are doing **/

#ifndef AUXILIARY
#include "esp_camera.h"
#endif
#include "camera_pins.h"

#define STATIC_IP_OCTAL "133" // dev only
#define DEBUG_MEM false // leave as false
#define FLUSH_DELAY 0 // for debugging crashes
#define DBG_ON false // esp debug output
#define DBG_LVL ESP_LOG_ERROR // level to use if DBG_ON true: ESP_LOG_ERROR, ESP_LOG_WARN, ESP_LOG_INFO, ESP_LOG_DEBUG, ESP_LOG_VERBOSE
#define DOT_MAX 50
#define HOSTNAME_GRP 99
 
#define APP_VER "1.0.0"
// to determine if newer data files need to be loaded. Bump whenever a config row is
// added, removed or moved group, so an existing configs.txt is regenerated rather than
// leaving stale keys behind.
// Bumping this is not free: prefs.cpp treats a configs.txt carrying a different value as
// outdated and deletes the whole of DATA_DIR, so the board loses its settings and refetches
// its web files from GITHUB_PATH on the next boot. Recordings and the WiFi credentials are
// untouched - the credentials live in NVS, not on the card - but the download means the repo
// must already hold the files you expect the board to come back with
#define CFG_VER 52

#define APP_NAME "ESP-CAM_MJPEG" // max 15 chars
#define INDEX_PAGE_PATH DATA_DIR "/MJPEG2SD" HTML_EXT
#define NEED_PSRAM true
#define MIN_PSRAM 2

#define HTTP_CLIENTS 2 // http(s), ws(s)
#define MAX_STREAMS 3 // (web stream, playback, download), NVR video, NVR audio
#define FILE_NAME_LEN 64
#define IN_FILE_NAME_LEN (FILE_NAME_LEN * 2)
#define JSON_BUFF_LEN (32 * 1024) // set big enough to hold all file names in a folder
#define MAX_CONFIGS 220 // must be > number of entries in configs.txt
#define MIN_RAM 8 // min object size stored in ram instead of PSRAM default is 4096
#define MAX_RAM 4096 // max object size stored in ram instead of PSRAM default is 4096
#define TLS_HEAP (64 * 1024) // min free heap for TLS session
#define WARN_HEAP (32 * 1024) // low free heap warning
#define WARN_ALLOC (16 * 1024) // low free max allocatable free heap block
#define MAX_FRAME_WAIT 1200
#define RGB888_BYTES 3 // number of bytes per pixel
#define GRAYSCALE_BYTES 1 // number of bytes per pixel 

#define STORAGE SD_MMC
// OTA updates are pulled from GitHub Releases on the repo below, which is
// also where setupAssist looks for missing web UI files (GITHUB_PATH).
// A release must be tagged with a version above APP_VER (a leading 'v' is
// optional, eg v1.0.1) and carry an asset named exactly OTA_ASSET_NAME.
#define OTA_REPO_OWNER "theostrichjouster-star"
#define OTA_REPO_NAME "ESPIPCAM"
#define OTA_ASSET_NAME "ESPIPCAM.bin"
#define GITHUB_PATH "/" OTA_REPO_OWNER "/" OTA_REPO_NAME "/main"
#define OTA_TAG_LEN 16 // longest release tag handled, eg "v10.20.30"
#define OTA_STATUS_LEN 64 // update status message shown in the web UI
// smallest plausible firmware image - guards against a garbage or truncated
// image being handed to Update.begin(), which would brick the running partition
#define MIN_OTA_IMAGE_SIZE (64 * 1024)
// playback read cluster. Must be a multiple of the SD card sector size (512 or 1024 bytes).
// Each cluster costs one SDMMC command round trip on top of the transfer, and at 8KB that
// overhead was most of the read: measured 7.7ms per 8KB cluster (~1.0MB/s) against
// 4.4MB/s on the 32KB write path. The DRAM landing buffer (sdReadBuf) is this size; the
// consumer's copy (iSDbuffer) lives in PSRAM so the cluster can grow without paying for
// it twice in internal RAM
#define RAMSIZE (1024 * 32)
// SD write block for the AVI capture path only - see sdWriteBuf in mjpeg2sd.cpp.
// Must be a multiple of the SD card sector size (512 or 1024 bytes)
#define SD_WRITE_SIZE (1024 * 32)
#define CHUNKSIZE (1024 * 4)
#define ISCAM // cam specific code in generic cpp files

#define AVI_EXT "avi"
#define CSV_EXT "csv"
#define SRT_EXT "srt"
#define AVI_HEADER_LEN 310 // AVI header length
#define CHUNK_HDR 8 // bytes per jpeg hdr in AVI 
#define AVITEMP "/current.avi"

// 800 samples is one whole read per video frame at 20fps (16000/20), and a whole number
// of reads at 10, 5, 4, 2 and 1 fps - the rates frameData uses. 512 divided none of them,
// which is what produced the ragged 1024/2048 byte audio chunks
#define DMA_BUFF_LEN 800 // used for I2S buffer size
#define DMA_BUFF_CNT 4
// PSRAM accumulator holding audio awaiting interleave into the AVI, one per ping pong half.
// Drained at most once per video frame, so it has to hold one frame period of audio plus
// slack for an SD write stall. 48kB is 1.5 secs at 16kHz 16 bit mono, which covers even
// FPS 1 (32000 bytes per frame) with room to spare
#define AUD_CHUNK_SIZE (1024 * 48)
// audio accumulated before a chunk is written, 250ms at 16kHz. Above 4fps this means
// fewer audio chunks than video frames, so the index needs well under 2 entries per frame
#define AUD_CHUNK_MIN 8000

// Motion detection is the OV5640's own 4x4 AEC zone grid (0x5691-0x56A1) compared between
// checks - no JPEG decode, so it works at every frame size and keeps running during
// recording and live view. The decode-based detector this replaced hard hung the board
// intermittently at or above ~1.3MP (root cause never found), which forced a pixel cap at
// HD and a VGA detection round-trip; both are gone with the decoder. Measurements and the
// hang history are in BOARD_TESTING.md.
#define MIC_GAIN_CENTER 3 // mid point

#ifdef CONFIG_IDF_TARGET_ESP32S3 
#define SERVER_STACK_SIZE (1024 * 8)
#define DS18B20_STACK_SIZE (1024 * 2)
#else
#define SERVER_STACK_SIZE (1024 * 4)
#define DS18B20_STACK_SIZE (1024)
#endif
#define STICK_STACK_SIZE (1024 * 4)
#define BATT_STACK_SIZE (1024 * 2)
#define CAPTURE_STACK_SIZE (1024 * 4)
#define EMAIL_STACK_SIZE (1024 * 6)
#define FS_STACK_SIZE (1024 * 4)
#define OTA_STACK_SIZE (1024 * 6)
#define LOG_STACK_SIZE (1024 * 3)
#define AUDIO_STACK_SIZE (1024 * 4)
#define MICREM_STACK_SIZE (1024 * 2)
#define MQTT_STACK_SIZE (1024 * 4)
#define PING_STACK_SIZE (1024 * 6)
#define PLAYBACK_STACK_SIZE (1024 * 2)
#define SERVO_STACK_SIZE (1024 * 1)
#define SUSTAIN_STACK_SIZE (1024 * 4)
#define TGRAM_STACK_SIZE (1024 * 6)
#define TELEM_STACK_SIZE (1024 * 4)
#define HB_STACK_SIZE (1024 * 2)
#define UART_STACK_SIZE (1024 * 2)
#define INTERCOM_STACK_SIZE (1024 * 2)
#define SENSOR_STACK_SIZE (1024 * 2)

// task priorities
#define CAPTURE_PRI 6
#define SUSTAIN_PRI 5
#define HTTP_PRI 5
#define STICK_PRI 5
#define AUDIO_PRI 5
#define INTERCOM_PRI 5
#define LOG_PRI 5
// playback reader above every other app task: it is I/O bound (one cluster read per
// notify, blocked on DMA for most of it) so it cannot starve anything, and its wake latency
// after each DMA completion feeds straight into the playback frame rate. Measured 4 -> 7:
// per-cluster read 8.3 -> 7.7ms, consumer wait 44 -> 29ms/frame. A modest gain - most of
// the per-read overhead is below app priorities (sdmmc completion path vs wifi/lwip), so
// cluster size, not priority, is the lever that matters
#define PLAY_PRI 7
#define TELEM_PRI 3
#define TGRAM_PRI 1
#define EMAIL_PRI 1
#define FTP_PRI 1
#define MQTT_PRI 1
#define LED_PRI 1
#define SERVO_PRI 1
#define HB_PRI 1
#define UART_PRI 1
#define DS18B20_PRI 1
#define BATT_PRI 1
#define SENSOR_PRI 1

/******************** Function declarations *******************/

struct mjpegStruct {
  size_t buffLen;
  size_t buffOffset;
  size_t jpegSize;
};

struct fnameStruct {
  uint8_t recFPS;
  uint32_t recDuration;
  uint16_t frameCnt;
};

enum audioAction {NO_ACTION, UPDATE_CONFIG, RECORD_ACTION, PLAY_ACTION, PASS_ACTION, WAV_ACTION, STOP_ACTION};
enum stepperModel {BYJ_48, BIPOLAR_8mm};

// global app specific functions

void appShutdown();
void buildAviHdr(uint8_t FPS, uint8_t frameType, uint16_t frameCnt, uint32_t durationMs = 0);
void buildAviIdx(size_t dataSize, bool isVid = true);
void buzzerAlert(bool buzzerOn);
void currentStackUsage();
void displayAudioLed(int16_t audioSample);
void finalizeAviIndex(uint16_t frameCnt);
void finishAudioRecord(bool isValid);
mjpegStruct getNextFrame(bool firstCall = false);
bool getPIRval();
size_t getAudioChunk(uint8_t** buf, size_t minLen);
bool aviIndexNearFull();
bool videoSizeAllowed(uint8_t fsize);
void dumpMotionStats();
void intercom();
bool isNight(uint8_t nightSwitch);
void openSDfile(const char* streamFile);
void prepAudio();
void prepAviIndex();
bool prepCam();
bool checkForUpdate();
bool startOtaUpdate();
extern char otaLatestTag[];
extern char otaStatus[];
extern bool otaUpdateAvailable;
bool prepRecording();
bool recoverAvi(); // repair a recording interrupted by supply loss - call before capture starts
void setCamPan(int panVal);
void setCamTilt(int tiltVal);
void dumpCamRegs();
void setSubSample(const char* csv); // debug probe: subsample increments + VTS, see mjpeg2sd.cpp
void setCamReg(const char* csv);
void setCamRegGrp(const char* csv);
void setBanding(int hz); // config: mains banding filter 0 (off) / 50 / 60, see applyBanding() in mjpeg2sd.cpp
void getCamReg(const char* addr);
void setExtDVDD(int val); // per-board NVS key: OV5640 internal regulator bypass for external DVDD
uint16_t sdBudgetKBs(); // measured SD write ceiling for the live bus clock - UI badge and governor
uint16_t frameWindowKB(int fs); // measured JPEG frame-window cliff per size, 0 = no prediction
void govRebaseQuality(int q); // user quality change mid-recording re-bases the SD governor
void avgZones(const char* unused); // diagnostic: dump the AEC 4x4 zone grid, see mjpeg2sd.cpp
void zoneStatsJson(char* buff, size_t buffLen); // detector snapshot as JSON for threshold tuning
void xclkStat(const char* unused); // diagnostic: measure XCLK / VSYNC off the pins, see mjpeg2sd.cpp
void lencFhd(const char* val); // experimental: A/B the LENC rescale for the FHD crop, see mjpeg2sd.cpp
void setCamPll(const char* csv);
uint8_t setFPS(uint8_t val);
uint8_t setFPSlookup(uint8_t val);
uint8_t fpsCeiling(framesize_t fs); // top of the fps slider for a size, see mjpeg2sd.cpp
void setInputPeripheral(uint8_t cmd, uint32_t controlVal);
void setLamp(uint8_t lampVal);
void setLightsRC(bool lightsOn);
void setSteering(int steerVal);
void setStepperPin(uint8_t pinNum, uint8_t pinPos);
void setStickTimer(bool restartTimer, uint32_t interval = 0);
void startAudioRecord();
void startHeartbeat();
void startSustainTasks();
void stepperRun(float RPM, float revFraction, bool _clockwise, stepperModel thisStepper);
void stopPlaying();
void stopSustainTask(int taskId);
size_t updateWavHeader();
size_t writeAviIndex(byte* clientBuf, size_t buffSize);

#ifndef AUXILIARY
void keepFrame(camera_fb_t* fb);
void setSensorSize(framesize_t newFS);
uint16_t jpegWidth(const uint8_t* buf, size_t len); // width from the JPEG SOF, not fb->width
bool viewerActive(); // a browser or NVR video stream is connected
bool nvrActive(); // an NVR video stream is connected
const char* sensorStateStr();
#endif

/******************** Global app declarations *******************/

// motion detection parameters
extern int moveStartChecks; // checks per second for start motion
extern int moveStopChecks; // checks per second while recording
extern int moveStopSecs; // secs without motion before a triggered recording closes
extern int zoneCount; // zones changed at once to signal motion
extern int zoneMask; // view-order bitmask of the 4x4 zones participating in detection

// motion recording parameters
extern int detectMotionFrames; // min sequence of tripped checks to confirm motion
extern int detectNightFrames; // frames of sequential darkness to avoid spurious day / night switching

// status & control fields
extern const char* appConfig;
extern bool autoUpload;
extern bool dbgMotion;
extern bool doPlayback;
extern bool doRecording; // whether to capture to SD or not
extern bool forceRecord; // Recording enabled by rec button or dashcam slider
extern uint8_t FPS;
extern uint8_t captureFPS; // user's chosen rate for the capture resolution
extern uint8_t fsizePtr; // index to frameData[] for record
extern bool isCapturing;
extern bool supplyParked; // supply sagged: recordings blocked, sensor shed, awaiting power
// battery monitor - a divider on the BATTERY side, which sees the decline while the buck
// still holds 3V3 regulated. Named batt* not volt* so nothing collides with the
// peripherals.cpp voltage code if INCLUDE_PERIPH is ever enabled on this board
extern int battUse;
// sensor idle throttle (Tier 2 power saving) - see idleThrottle() in mjpeg2sd.cpp
extern int idleFps;   // sensor fps while nothing needs frames, 0 = feature off
extern int idleSecs;  // seconds of no activity before throttling
bool streamsBusy();   // any sustained client needing frames - streamServer.cpp
bool sustainCancelled(); // the sustain download has been asked to stop - streamServer.cpp
bool streamSlotActive(uint8_t taskNum); // a viewer is attached to this slot - streamServer.cpp
// hand a captured frame to the stream ring; false means every slot is busy - streamServer.cpp
bool streamOfferFrame(uint8_t taskNum, const uint8_t* data, size_t len);
extern uint32_t streamSkipped[]; // frames the capture task could not hand to a busy sender
extern uint64_t streamSendUs[];    // microseconds spent inside httpd send calls
extern uint64_t streamSentBytes[]; // bytes handed to those send calls
extern int battPin;
extern int battScale;
extern int battWarnMv;
extern uint16_t battMv;
extern uint8_t sdGovBoost; // SD governor's active quality boost steps, 0 outside recordings
extern uint16_t sdGovFrameKB; // last-second average frame KB while recording, else 0
extern uint8_t lightLevel;  
extern uint8_t lampLevel;  
extern int micGain;
extern float motionVal;  // motion sensitivity setting - maps to the per-zone delta threshold
extern uint8_t nightSwitch; // initial white level % for night/day switching
extern bool nightTime; 
extern bool stopPlayback;
extern bool useMotion; // whether to use camera for motion detection (with motionDetect.cpp)
extern int dashCamOn; // enable continuous recording, with given interval
extern int maxFrames;
extern int tunedFps; // fps choices drive the sensor's own timing, see applyTunedTiming()
extern int bandingHz; // mains banding filter: 0 off (default), 50 or 60 manual
extern volatile bool retimePending; // fps changed - capture task retimes on the next frame
extern volatile int pendingFS; // size chosen mid-recording, applied when the clip closes
extern volatile int pendingFPS; // rate chosen mid-recording, applied when the clip closes
extern uint8_t xclkMhz;
extern char camModel[];
extern bool doKeepFrame;
extern int alertMax; // too many could cause account suspension (daily emails)
extern bool streamVid;
extern bool streamAud;
extern uint8_t numStreams;
extern uint8_t vidStreams;

#ifndef AUXILIARY
extern framesize_t maxFS;
extern framesize_t maxVideoFS; // AVI recording cap, stills are not capped
// what the sensor is set to right now. It follows fsizePtr in every state, but only the
// capture task moves it - so it is the hardware truth while fsizePtr is the user's intent
extern framesize_t sensorFS;
#endif

// buffers
extern uint8_t* iSDbuffer;
extern uint8_t aviHeader[];
extern const uint8_t dcBuf[]; // 00dc
extern const uint8_t wbBuf[]; // 01wb
extern byte* streamBuffer[]; // buffer for stream frame
extern size_t streamBufferSize[];
extern uint8_t* motionJpeg;
extern size_t motionJpegLen;
extern uint8_t* audioBuffer;
extern size_t audioBytes;
extern size_t maxFrameBuffSize;

// Auxiliary use
extern bool useUart;

// peripherals used
extern bool pirUse; // true to use PIR or radar sensor (RCWL-0516) for motion detection
extern bool lampAuto; // if true in conjunction with usePir, switch on lamp when PIR activated
extern bool lampNight;
extern int lampType;
extern bool voltUse; // true to report on ADC pin eg for for battery
extern bool wakeUse;
extern bool buzzerUse; // true to use active buzzer
extern int buzzerPin; 
extern int buzzerDuration; 
extern int relayPin;
extern bool relayMode;

// sensors 
extern int pirPin; // if usePir is true
extern int lampPin; // if useLamp is true
extern int lightsPin;

// Pan / Tilt Servos 
extern int servoPanPin; 
extern int servoTiltPin;
// ambient / module temperature reading 
extern int ds18b20Pin; // if INCLUDE_DS18B20 true
// batt monitoring 
extern int voltPin; 

// audio
extern int micSckPin; // I2S SCK 
extern int micSWsPin;  // I2S WS / PDM CLK
extern int micSdPin;  // I2S SD / PDM DAT
extern bool spkrRem; // true to use browser speaker
extern int mampBckIo; 
extern int mampSwsIo;
extern int mampSdIo;
extern volatile bool stopAudio;
extern volatile audioAction THIS_ACTION;
extern uint32_t SAMPLE_RATE; // audio sample rate

// configure for specific servo model, eg for SG90
extern int servoDelay;
extern int servoMinAngle; // degrees
extern int servoMaxAngle;
extern int servoMinPulseWidth; // usecs
extern int servoMaxPulseWidth;
extern int servoCenter;
extern bool SVactive;

// battery monitor
extern int voltDivider;
extern float voltLow;
extern int voltInterval;

// stepper motor
extern bool stepperUse;
extern uint8_t stepINpins[];

// Motors and RC
extern int servoSteerPin;
extern int lightsRCpin;
extern int maxSteerAngle;
extern int maxTurnSpeed;
extern int maxDutyCycle;
extern int minDutyCycle;
extern bool allowReverse;
extern bool autoControl;
extern int waitTime;
extern int heartbeatRC;
extern bool stickUse;
extern int stickzPushPin;
extern int stickXpin;
extern int stickYpin;

// task handling
extern TaskHandle_t battHandle;
extern TaskHandle_t captureHandle;
extern TaskHandle_t DS18B20handle;
extern TaskHandle_t emailHandle;
extern TaskHandle_t fsHandle;
extern TaskHandle_t logHandle;
extern TaskHandle_t mqttTaskHandle;
extern TaskHandle_t playbackHandle;
extern esp_ping_handle_t pingHandle;
extern TaskHandle_t servoHandle;
extern TaskHandle_t stickHandle;
extern TaskHandle_t sustainHandle[];
extern TaskHandle_t telegramHandle;
extern TaskHandle_t audioHandle;
extern SemaphoreHandle_t frameSemaphore[];
extern SemaphoreHandle_t motionSemaphore;


/************************** structures ********************************/

struct frameStruct {
  const char* frameSizeStr;
  const uint16_t frameWidth;
  const uint16_t frameHeight;
  const uint16_t defaultFPS;
  const uint8_t scaleFactor; // (0..3) see esp_jpeg_image_scale_t
  const uint8_t sampleRate; // (1..N)
  // ceiling with tunedFps: PIXCLK 80 at the size's VTS/HTS floors, fps = 80e6/(HTS x lf x VTS).
  // Values are the sweep's to own: rows the sweep has verified are measured, the rest are
  // model predictions from the measured HTS/VTS floors and are marked in the row comments.
  // 0 = no tuned menu (stills-only or unreachable rows)
  const uint16_t maxTunedFPS;
};

// indexed by frame size - needs to be consistent with sensor.h framesize_t enum
// and update corresponding frameSizeData[] entries in avi.cpp 
// https://github.com/espressif/esp32-camera/blob/master/driver/include/sensor.h
// defaultFPS retuned for the OV5640 on XIAO Sense, measured on hardware (Phase 5.8).
// Upstream's values were tuned for an OV2640 on an AI Thinker board and are wrong here in
// both directions: the sub-VGA rows claimed 30fps the sensor cannot produce, while XGA and
// HD were pinned at 5fps when the pipeline sustains 2-2.4x that.
// Frame rate is the sensor's, and it follows one formula, measured within 2% at five frame
// sizes, seven PLL multipliers and five HTS values:
//
//     fps = PIXCLK / (HTS x VTS_effective)   binned 2x2
//     fps = PIXCLK / (HTS x VTS_effective) / 2   full resolution readout
//
// PIXCLK is the parallel port pixel clock, and it is the pixel clock rather than any other
// because HTS and VTS are a count of pixel clocks: section 6.6 specifies the DVP sync widths
// in 0x470A/0x470B in "PCLK unit". This formula used to be written here with SYSCLK in place
// of PIXCLK, copied from the naming in the driver's calc_sysclk(). That was wrong - section
// 2.5 defines SysClk as "the internal clock of the Image Signal Processing (ISP) block" and
// never connects it to the frame timing. The arithmetic and every measurement were unaffected;
// only the name was, and the name sent the investigation looking for an ISP clock ceiling
// instead of checking table 8-5, which caps the pixel clock at 96MHz.
//
// VTS_effective is 0x380E/0x380F plus the AEC extra lines in 0x350C/0x350D, which auto extend
// the frame in poor light. PIXCLK is 45MHz below XGA and 50MHz above, from the driver's PLL
// tiers, and its value is dumpCamRegs()' pllClk/4 - an identification supported by table 8-5
// putting fPCLK typical at 48MHz and by agreement with every rate measured, not by a datasheet
// derivation. Raising the multiplier past 200 stops moving it while the driver's own PLL tree
// is used, so XCLK stays at 20MHz.
//
// Binning is therefore what decides everything. Full resolution costs exactly double, which is
// why HD does 31.9fps - and FHD cannot bin, since a binned read of the 2592 wide array yields
// only about 1312 columns. setSensorSize() drops HTS to HTS_FLOOR for the binned sizes, which
// the driver leaves needlessly high at 2644 for XGA and HD.
//
// The other half of HTS x VTS is how much of the array is read at all. Datasheet 4.2: frame
// rate follows the ISP input size, and the driver never crops - it programs the window from a
// fixed ratio_table row and downscales. FHD was reading 2624x1472 to emit 1920x1080, so
// setSensorSize() now calls applyCropWindow() to read only what is wanted, which took FHD from
// 5.9 to 10.7fps, SXGA from 4.6 to 11.3, UXGA from 2.8 to 9.6 and P_HD from 5.0 to 9.9. It
// costs field of view - FHD reads 1984 of 2624 columns, so framing is about 1.29x tighter -
// and it costs light, since a shorter frame is a shorter maximum exposure.
//
// Storage is not the constraint at any of these rates: the card sustains 3.4MB/s and identical
// frame rates come back at quality 10 and 18. Note "Busy" in the recording stats counts blocked
// time in frame acquisition, so it reads ~100% whenever the app asks for more than the sensor
// can give. It indicates frame starvation, not load, and is not a headroom measure.
//
// Values below are the measured rate with a little margin for dim light, where the AEC extra
// lines lengthen the frame.
//
// scaleFactor and sampleRate are vestigial: they parameterised the JPEG-decode motion
// detector, which has been replaced by the sensor's own zone grid (no decode at any size).
// The columns are kept so the rows keep their shape; nothing reads them any more.
const frameStruct frameData[] = {
  // Real ISP-scaler sizes halve delivery when VTS rises above the driver's value (at ANY
  // clock - the threshold is state-dependent, measured 27/28 Aug 2026), so VGA and QVGA
  // are CLOCK-tuned at driver VTS instead (applyScalerClock: fps via PLL mul/sys_div,
  // verified exact at five rates over 20-61MHz, QVGA 39.47 at the 80MHz ceiling). The
  // remaining scaler sizes (96X96..XGA) stay untuned at 0 - unmeasured and outside the
  // OV5640 UI set. 1280X960 is exempt by measurement - its scaler pass is 1:1
  {"96X96", 96, 96, 18, 1, 1, 0}, // 2MP sensors // PY260  | sensor ceiling 19.7 (PLL tier 160) | scaler size, untuned
  {"QQVGA", 160, 120, 18, 1, 1, 0},   // measured 19.7 flat out | scaler size, untuned
  {"128X128", 128, 128, 18, 1, 1, 0}, // PY260
  {"QCIF", 176, 144, 18, 1, 1, 0},
  {"HQVGA", 240, 176, 18, 2, 1, 0},
  {"240X240", 240, 240, 18, 2, 1, 0},
  {"QVGA", 320, 240, 20, 2, 1, 39},   // PY260 | driver clock 22.2 (PLL tier 180) | clock-tuned: 39.47 MEASURED 28 Aug at mul 120 / VTS 984 | swept 1-39: exposure = full period at every fps (VTS 984 < AEC cap) | re-swept 2 Sep 2026 lit q10: register tier identical, 39.0 delivered at 39 (11 KB frames, boost 0)
  {"320X320", 320, 320, 20, 2, 1, 0}, // PY260 only
  {"CIF", 400, 296, 20, 2, 1, 0},
  {"HVGA", 480, 320, 20, 2, 1, 0},
  {"VGA", 640, 480, 20, 3, 1, 39},    // PY260 | driver clock 22.2 ceiling | clock-tuned: 5 rates 10-30 MEASURED 28 Aug (same 2060x984 timing as QVGA), 39 ceiling | swept 1-39: exposure = full period at every fps | re-swept 2 Sep 2026 lit q10: register tier identical, 39.0 delivered at 39 (31 KB frames, boost 0)
  {"SVGA", 800, 600, 21, 3, 1, 0},    // measured 22.1, already at HTS_FLOOR | scaler size, untuned
  {"XGA", 1024, 768, 24, 3, 1, 0},    // 24.5 at HTS_FLOOR, same timing as 1280X960 which measured it | scaler size, untuned
  {"HD", 1280, 720, 30, 3, 1, 52},    // PY260 | measured 31.9 at HTS_FLOOR, 25.3 before it | tuned 52.06 MEASURED 27 Aug, exact 12-52 | exposure-first sweep 28 Aug: 95ms @10fps, 399ms @1fps (PIXCLK walk, VTS held 1968 below 19fps) | re-swept 2 Sep 2026 lit q10: register tier identical; 52 delivers 51.4 at governor boost 2, 51 delivers 48.5 at boost 1 - storage time per 75 KB frame (17 of 19 ms), scene-dependent, not the sensor (BOARD_TESTING 26)
  {"SXGA", 1280, 1024, 11, 3, 1, 17}, // measured 11.3 cropped, 4.6 before it | tuned 17 predicted at the 2200 full-res HTS floor (AEC stats floor - see applyCropWindow)
  {"UXGA", 1600, 1200, 9, 3, 1, 14},  // PY260 | measured 9.6 cropped, 2.8 before | scale 3 not 4, see below | tuned 14 predicted at HTS 2200
  {"FHDNARROW", 1920, 1080, 10, 3, 1, 16},  // 3MP Sensors only // PY260 | was "FHD" until 3 Sep 2026, renamed when FHDMID and FHDFULL joined it - this is the 1:1 crop, the narrowest framing of the three (see the FHD ladder note below frameData) |  measured 10.7 cropped, 5.9 before | tuned: 15.0 verified at the 2200 floor, 16 ceiling (HTS 2200 x2 x VTS 1128 - 1112 is the cropped row count, the VTS floor adds 16) | swept 1-16 28 Aug incl 16.0 delivered; 191ms @5fps. 15-16fps delivery is frame-window-gated: dim/noisy q6 frames outgrow the JPEG window and vanish (see BOARD_TESTING 19) | re-swept 2 Sep 2026 lit q10: register tier identical, 16.0 exact at 232 KB frames, boost 1 (unlit the same point was SD-bound at 15.5, 252 KB, boost 4)
  {"P_HD", 720, 1280, 9, 3, 1, 13},   // measured 9.9 cropped, 5.0 before | tuned 13 predicted at HTS 2200
  {"P_3MP", 864, 1536, 4, 3, 1, 0},   // OV3660 only - not selectable on this sensor, set by analogy
  {"QXGA", 2048, 1536, 7, 4, 1, 11},  // was stills only - 7.2 predicted cropped at driver clock | tuned 11 predicted
  {"QHD", 2560, 1440, 2, 4, 1, 9},    // 5MP Sensors only | 2.8 @ 77% busy at 3, backed off for SD margin | THE SAME READOUT AS FHDFULL - 2624x1472, the driver's own 16:9 window - but emitted 1:1 at 2560x1440 instead of downscaled, so its scaler pass is a no-op like 1280X960's and applyCropWindow writes nothing | MEASURED 3 Sep 2026 (was "tuned 9 predicted", never swept until then): ceiling 9.092 VSYNC-COUNTED against 9.092 computed, 5fps counted 5.047 | register tier 9/9 all gates, VTS 1547 at the ceiling then the 1968 cap with a clock walk below 7fps, max exposure 110ms at 9fps to 986ms at 1fps | recordings 9 and 5 delivered exact, 372/298KB frames, no rescue | NOT stills-only: 3.69MP is inside the QSXGA video cap and both clips recorded fine (the UI label saying otherwise was stale) | TIGHT at its ceiling though - 93% busy and 75% of the SD budget at 9fps, against FHDFULL's 39% and 38% for the same framing | frame-window cliff MEASURED 3 Sep 2026 by random-pattern descend at 3fps and 2fps: clean to 808-815KB, intermittent from ~800, fully dead 2 steps below - a real cliff, not the 960KB maxFrameBuffSize gate that masks QSXGA's. frameWindowKB returns the conservative 800. Lit q10 frames are 372KB at 9fps, so a real scene has ~2.1x headroom and would need to roughly double before the window bit
  {"WQXGA", 2560, 1600, 2, 4, 1, 8},  // measured 2.8 @ 85% busy at req 3, backed off | tuned 8 predicted
  {"P_FHD", 1080, 1920, 4, 3, 1, 9},  // measured 4.0 @ 37% busy | scale 3 not 4, see below | tuned 9 predicted
  {"QSXGA", 2560, 1920, 2, 4, 1, 7},  // measured 2.0 @ 59% busy | tuned 7.00 MEASURED 27 Aug (full array, HTS 2844 x2); q6 frames 764KB - see the budget badge | swept 1-7 28 Aug all exact; 140ms @7fps, 952ms @1fps (PIXCLK 11.73) | re-swept 2 Sep 2026 lit q10: register tier identical, 7.0 exact at 354 KB frames, boost 0, busy 54%
  {"5MP", 2592, 1944, 4, 4, 1, 0},    // PY260 only - unreachable on OV5640, left as inherited
  // Custom sizes past the driver's framesize_t. That enum lives in the precompiled
  // esp32-camera library, so a size it lacks has to be carried here instead. Rows below this
  // point are never handed to the driver - setSensorSize() maps them onto a base size and
  // then overrides only the registers that differ
  {"1280X960", 1280, 960, 24, 3, 1, 41}, // measured 24.5 at HTS_FLOOR, 19.1 before it | tuned 39.03 MEASURED 27 Aug, exact 12-39 (scaler pass is 1:1, exempt from the scaler halving) | exposure-first sweep 28 Aug: same timing as HD, 95ms @10fps | re-swept 2 Sep 2026 lit q10: register tier identical (39.47 counted); 39 delivers 37.7 and 38 delivers 37.5 - 98 KB frames cost 22 ms of storage in a 25.6 ms period while KB/s demand sits at 88%, under the governor's push line. Ceiling kept: the sensor is exact and the shortfall is scene-dependent storage time (BOARD_TESTING 26) | 42 as of 4 Sep 2026 (BOARD_TESTING 37): SCLK 88 on the in-spec 0x3108=0x11 route (VCO 440, mul 66) at HTS 2112 / VTS 984, 42.343 VSYNC-counted three times with min = max, still indistinguishable from the 2060/80 baseline (channel ratio 1.023, hdiff 2.3). HTS 2112 not 2060: 24.0us line inside the ~24us row-time floor, and the 1280-wide output stretch (2-5% of frames run long at 2060) is gone at 2112. Requests 38-42 run route B, 37 and below the 80 MHz tree as before. The datasheet's 45 needs a 22.58us line and SCLK 96, both corrupt on this board | 41 later on 4 Sep 2026: HTS 2156 not 2112 - a 24.5us line, the length that has never shown the bistable magenta readout latch (2112 = 24.0us had a dozen clean samples and no soak; 23.75us latches sometimes). Ceiling 41.48, requests 37-41 route B, 36 and below route A. The banding filter is off by config default from the same commit, so exposure at the ceiling is the whole 24.0 ms frame
  // The two wider 1080p variants. Same 1920x1080 output as FHDNARROW, read off a LARGER slice
  // of the array and downsized by the ISP scaler instead of cropped 1:1 - see the FHD ladder
  // note below. Both MEASURED on COM4, 3 Sep 2026, the register tier generating their sweep.csv
  // reference. The scaler-halving hazard did NOT bite either of them
  {"FHDMID", 1920, 1080, 10, 3, 1, 12},  // pre-scale 2304x1296 (scaler 1.2x), window 2368x1328, HTS 2444 x2, VTS 1344 | 90% of array width, 68% of its height | clock-tuned: ceiling 12.178 VSYNC-COUNTED against 12.18 computed, 8fps exact at 53.33MHz | register tier 12/12 all gates, max exposure 82ms at the ceiling to 647ms at 1fps | recordings 12 and 6 delivered exact at 152KB frames (34% of the frame window), boost 0, busy 42/20%
  {"FHDFULL", 1920, 1080, 9, 3, 1, 9}    // pre-scale 2560x1440 (scaler 1.333x) - the DRIVER'S OWN window, so no window register is written at all | 100% of array width, 75% of its height | clock-tuned: ceiling 9.059 VSYNC-COUNTED against 9.06 computed, 6fps exact at 51.33MHz | register tier 9/9 all gates, max exposure 110ms at the ceiling to 833ms at 1fps | recordings 9 and 5 delivered exact at 188/171KB frames, boost 0, busy 39/19% - far more headroom at its ceiling than FHDNARROW has at 16 (busy 99%)
  ,
  // The narrow pair. Same trade as the FHD ladder run the other way: these buy RATE with field
  // of view and with light, by cropping the array until the ISP scaler lands at 1:1. Their
  // parents read the whole 2624x1952 array and throw most of it away in the scaler, which is
  // exactly what pins QVGA, VGA and 1280X960 to a shared 39.47 ceiling.
  // New rows APPEND. Never insert above an existing custom size - that shifts its index out
  // from under its FS_ define, its UI option value and its sweep.csv rows, silently
  {"VGANARROW", 640, 480, 20, 3, 1, 77},   // window 1344x992 -> 2x2 -> ISP in 672x496 -> pre-scale 640x480, scaler 1:1 | 51% of array width and height | MEASURED 3 Sep 2026: 77.022 VSYNC-counted at VTS 504, against 39.448 for full-frame VGA | costs light too: max exposure 12.9ms against 25.2
  {"QVGANARROW", 320, 240, 20, 2, 1, 147}  // window 704x512 -> 2x2 -> ISP in 352x256 -> pre-scale 320x240, scaler 1:1 | 27% of array width, 26% of height | MEASURED 3 Sep 2026: 147.059 VSYNC-counted at VTS 264, against 39.474 for full-frame QVGA | max exposure 6.7ms, so this size needs real light
};

// 1280x960 is the largest 4:3 size the OV5640 can still read 2x2 binned, and binning is what
// decides frame rate on this part: measured fps is PIXCLK/(HTS*VTS) when binned and half that
// at full resolution, confirmed within 2% at five frame sizes. FHD cannot bin at all - a
// binned read of the 2592 wide array yields about 1312 columns, so 1920 is unreachable -
// which is why FHD manages 5.9fps while HD does 25.3. Datasheet table 2-1 lists 1280x960 as a
// native binned mode. The driver has no enum entry for it, so it is configured as XGA, which
// uses the same 2624x1952 binned window and the same HTS/VTS, with only the DVP output size
// overridden. It therefore runs at XGA's exact frame rate while carrying 56% more pixels.
// The FHD ladder. All three emit 1920x1080 and differ only in how much of the array they read
// to get there, which is the frame rate / field of view / light trade in its rawest form.
// applyCropWindow() aims the readout at a target PRE-SCALE size (cropPreScaleW), and the
// driver's 16:9 window is 2624x1472 at offsets 32/16, ie a pre-scale of exactly 2560x1440.
//
//   size       pre-scale   window     HTS   VTS   ceiling  maxExp  width  height  scaler
//   FHDNARROW  1920x1080   1984x1112  2200  1128   16.120   62ms    76%    57%    off (1:1)
//   FHDMID     2304x1296   2368x1328  2444  1344   12.178   82ms    90%    68%    1.2x
//   FHDFULL    2560x1440   2624x1472  2844  1488    9.059  110ms   100%    75%    1.333x
//
// Ceilings are VSYNC counts off the pin, 3 Sep 2026, not the computed figures - and each
// agreed with its computation to better than 0.01fps. The halving did not happen: the implied
// pixel clock came back at 80.01, 80.00 and 76.68MHz against 80.00, 80.00 and 76.67 computed.
//
// The wide variants also produce SMALLER frames than the 1:1 crop despite covering more scene
// (152KB and 188KB against 228KB at q10), because downscaling averages away sensor noise the
// crop keeps. So they cost less to store as well as seeing more, and both sit near 40% busy at
// their ceilings where FHDNARROW sits at 99%.
//
// FHDFULL asks for the driver's own pre-scale, so applyCropWindow()'s "nothing to crop" early
// return fires and not one window, HTS or VTS register is written - the safest form the change
// could take. Full width at 80MHz is not new ground: QSXGA already runs HTS 2844 x2 at
// 79.33MHz. The genuinely new operating point is the scaler actively DOWNSIZING at that clock.
//
// All three are clock-tuned (scalerClockSize), not VTS-tuned. Turning the scaler on puts a
// size into the class that halves delivery when VTS rises above the driver's value at any
// clock (27/28 Aug), and the safe side of that rule is a pinned VTS with fps on the PLL. It
// costs nothing: applyTunedTiming clamps VTS upward only, so the ceiling is the same either
// way, and a slower clock lengthens every row where VTS above 1968 buys no exposure at all.
// The narrow pair are the first BINNED sizes to be cropped. Everything above them crops at full
// resolution, where the window is the ISP input; here the window is TWICE the ISP input in each
// axis, because 2x2 subsampling sits between them. applyCropWindow() multiplies by the live
// subsample factor for exactly this reason, and their frame length follows the binned rule
// (rows after subsampling, plus 8) rather than the full-resolution one.
#define NUM_CUSTOM_FS 5
#define FS_1280X960 (FRAMESIZE_INVALID + 0) // index 25, first row past the driver's enum
#define FS_1280X960_BASE FRAMESIZE_XGA      // same binned window, same line timing
#define FS_FHDMID (FRAMESIZE_INVALID + 1)   // index 26
#define FS_FHDFULL (FRAMESIZE_INVALID + 2)  // index 27
#define FS_FHD_BASE FRAMESIZE_FHD           // both wide variants ride the driver's FHD window
#define FS_VGANARROW (FRAMESIZE_INVALID + 3)  // index 28
#define FS_QVGANARROW (FRAMESIZE_INVALID + 4) // index 29
#define FS_VGANARROW_BASE FRAMESIZE_VGA       // same binned window, cropped by applyCropWindow
#define FS_QVGANARROW_BASE FRAMESIZE_QVGA
