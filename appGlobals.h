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
#define CFG_VER 49

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
#define RAMSIZE (1024 * 8) // set this to multiple of SD card sector size (512 or 1024 bytes)
// SD write block for the AVI capture path only, kept separate from RAMSIZE because
// iSDbuffer is double sized for the playback path - see sdWriteBuf in mjpeg2sd.cpp.
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

// Largest frame motion detection will process, as a pixel count rather than a framesize_t
// index - framesize_t is not ordered by size, so an index test blocks P_HD (0.92MP)
// despite it being smaller than SXGA.
// The value is HD, and it is set by measurement not by preference. Decoding a frame at or
// above roughly 1.3MP hard hangs the board - no HTTP, no ping, no serial, no panic, no
// reboot - intermittently. FHD survived 153 checks once and died in under 10 another time.
// Ruled out: heap and PSRAM exhaustion, duty cycle (VGA runs 1.28x over its frame interval
// and is fine), check duration (P_HD takes 331ms and is fine), and the frame buffer leak
// fixed in processFrame(). Root cause is still unknown. Every size at or below this ran
// hundreds of checks without incident, so the cap stays here until that is understood.
// This is lower than the maxVideoFS video cap on purpose: recording a size and running
// motion detection on it are separate permissions.
#define MOTION_MAX_PIXELS 921600UL
// rgbBuf holds the decoded motion bitmap. Sizing this from the nominal frame dimensions
// is wrong: the decoder pads width and height up to the 16 pixel MCU grid before scaling,
// so SVGA 800x600 becomes 800x608 and decodes at 1/8 to 100x76, not 100x75. The margin
// below covers that padding, and checkMotion() range checks the padded dimensions too.
// MOTION_MAX_PIXELS/64 is the 1/8 case; sizes that use a smaller scaleFactor decode a
// larger fraction of a smaller frame and still land well under it. Worst case across
// everything within the cap is HD and P_HD at 160x90 and 90x160, 43,200 bytes of 49,344
#define MOTION_MCU_PX 16 // jpeg minimum coded unit, 4:2:0
#define MOTION_RGB_BUF_SIZE (((MOTION_MAX_PIXELS / 64) + 2048) * RGB888_BYTES)
// The size the sensor is held at while armed and idle, so motion detection never decodes
// the user's capture resolution. Fixed, not user exposed.
// VGA is chosen by measurement, not by pixel count. Three 45s windows each, recording active:
//   QVGA 320x240 @ scaleFactor 2 (/4)  ->  80x60 bitmap  ->  59.9 ms per check
//   VGA  640x480 @ scaleFactor 3 (/8)  ->  80x60 bitmap  ->  36.5 ms per check
//   QVGA 320x240 @ scaleFactor 3 (/8)  ->  40x30 bitmap  ->  17.5 ms per check
// /8 lets the decoder take its DC only fast path, one coefficient per 8x8 block, while /4
// needs a real partial IDCT - so QVGA at /4 costs 65% more than VGA despite carrying a
// quarter of the pixels. VGA is the smallest row in frameData using scaleFactor 3, and it
// yields the same 80x60 comparator source that QVGA at /4 does, for less time.
#define MOTION_DETECT_FS FRAMESIZE_VGA
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
#define PLAY_PRI 4
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
bool motionSizeAllowed(uint8_t fsize);
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
void setCamPan(int panVal);
void setCamTilt(int tiltVal);
void dumpCamRegs();
void setCamReg(const char* csv);
void getCamReg(const char* addr);
void setCamPll(const char* csv);
uint8_t setFPS(uint8_t val);
uint8_t setFPSlookup(uint8_t val);
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
bool checkMotion(camera_fb_t* fb, bool motionStatus, bool lightLevelOnly = false);
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
extern int captureSecs; // fixed duration of a motion triggered recording

// motion recording parameters
extern int detectMotionFrames; // min sequence of changed frames to confirm motion 
extern int detectNightFrames; // frames of sequential darkness to avoid spurious day / night switching
extern int detectNumBands;
extern int detectStartBand;
extern int detectEndBand; // inclusive
extern int detectChangeThreshold; // min difference in pixel comparison to indicate a change

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
extern uint8_t lightLevel;  
extern uint8_t lampLevel;  
extern int micGain;
extern float motionVal;  // motion sensitivity setting - min percentage of changed pixels that constitute a movement
extern int motionPeakChange; // highest changed pixel count since the last stats dump
extern int motionThreshold; // the threshold that count was measured against
extern uint8_t nightSwitch; // initial white level % for night/day switching
extern bool nightTime; 
extern bool stopPlayback;
extern bool useMotion; // whether to use camera for motion detection (with motionDetect.cpp)  
extern uint8_t colorDepth;
extern int dashCamOn; // enable continuous recording, with given interval
extern int maxFrames;
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
// what the sensor is set to right now, which is not the same thing as fsizePtr. fsizePtr
// stays the user's chosen capture resolution; sensorFS drops to MOTION_DETECT_FS whenever
// the device is armed and idle, so nothing decodes a large frame
extern framesize_t sensorFS;
#endif

// buffers
extern uint8_t iSDbuffer[];
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
// lines lengthen the frame. VGA stays at 20 because it is MOTION_DETECT_FS and the detection
// cost was tuned against that rate.
//
// scaleFactor is passed straight to the jpeg decoder as jpg_scale_t, which only defines
// 0..3 (JPG_SCALE_NONE..JPG_SCALE_8X). A 4 is out of range and the decode fails outright,
// so any size motion detection is allowed to see must be 3 or below. Every row at or below
// the MOTION_MAX_PIXELS cap is 3 or below, so no reachable decode can fail that way.
// UXGA and P_FHD are 3 rather than 4 as a leftover from when the motion cap was FHD. They
// now sit above the motion cap but below the video cap, so they can be recorded but never
// decoded, and their scaleFactor is unused - left at 3 so the rows stay correct if the cap
// is ever raised. Note they would NOT fit rgbBuf at 1/8 (90,000 and 97,200 bytes against
// 49,344), which is what motionSizeAllowed() and the padded range check in checkMotion()
// are there to prevent. The rows above the video cap keep 4 and are unreachable entirely.
//
// scaleFactor dominates motion detection cost, and NOT in the direction pixel count
// suggests. Measured on hardware, three 45 second windows each, recording active:
//   QVGA 320x240 @ scaleFactor 2 (/4)  ->  80x60 bitmap  ->  59.9 ms per check
//   VGA  640x480 @ scaleFactor 3 (/8)  ->  80x60 bitmap  ->  36.5 ms per check
//   QVGA 320x240 @ scaleFactor 3 (/8)  ->  40x30 bitmap  ->  17.5 ms per check
// QVGA at /4 is 65% MORE expensive than VGA despite having a quarter of the pixels,
// because /8 lets the decoder take the DC only fast path - one coefficient per 8x8
// block - while /4 needs a real partial IDCT. Four times the blocks at near zero cost
// each beats a quarter of the blocks at full IDCT cost. VGA is the smallest row in this
// table that uses /8, which makes it the cheapest detection size that still feeds the
// comparator an 80x60 source. QVGA at /8 is faster still but only produces 40x30, and
// that detail loss was judged not worth the 19 ms. Since 19439ee checkMotion() compares the
// decoded bitmap at its native size, so a coarser source really is a coarser compare.
const frameStruct frameData[] = {
  {"96X96", 96, 96, 18, 1, 1},     // 2MP sensors // PY260  | sensor ceiling 19.7 (PLL tier 160)
  {"QQVGA", 160, 120, 18, 1, 1},   // measured 19.7 flat out
  {"128X128", 128, 128, 18, 1, 1}, // PY260
  {"QCIF", 176, 144, 18, 1, 1},
  {"HQVGA", 240, 176, 18, 2, 1},
  {"240X240", 240, 240, 18, 2, 1},
  {"QVGA", 320, 240, 20, 2, 1},    // PY260 | measured 22.2 flat out (PLL tier 180)
  {"320X320", 320, 320, 20, 2, 1}, // PY260 only
  {"CIF", 400, 296, 20, 2, 1},
  {"HVGA", 480, 320, 20, 2, 1},
  {"VGA", 640, 480, 20, 3, 1},     // PY260 | measured 20.0 @ 45% busy, ceiling 22.2
  {"SVGA", 800, 600, 21, 3, 1},    // measured 22.1, already at HTS_FLOOR
  {"XGA", 1024, 768, 24, 3, 1},    // 24.5 at HTS_FLOOR, same timing as 1280X960 which measured it
  {"HD", 1280, 720, 30, 3, 1},     // PY260 | measured 31.9 at HTS_FLOOR, 25.3 before it
  {"SXGA", 1280, 1024, 11, 3, 1},  // measured 11.3 cropped, 4.6 before it
  {"UXGA", 1600, 1200, 9, 3, 1},   // PY260 | measured 9.6 cropped, 2.8 before | scale 3 not 4, see below
  {"FHD", 1920, 1080, 10, 3, 1},   // 3MP Sensors only // PY260 | measured 10.7 cropped, 5.9 before
  {"P_HD", 720, 1280, 9, 3, 1},    // measured 9.9 cropped, 5.0 before
  {"P_3MP", 864, 1536, 4, 3, 1},   // OV3660 only - not selectable on this sensor, set by analogy
  {"QXGA", 2048, 1536, 7, 4, 1},   // stills only, so not measurable by recording - 7.2 predicted cropped
  {"QHD", 2560, 1440, 2, 4, 1},    // 5MP Sensors only | 2.8 @ 77% busy at 3, backed off for SD margin
  {"WQXGA", 2560, 1600, 2, 4, 1},  // measured 2.8 @ 85% busy at req 3, backed off
  {"P_FHD", 1080, 1920, 4, 3, 1},  // measured 4.0 @ 37% busy | scale 3 not 4, see below
  {"QSXGA", 2560, 1920, 2, 4, 1},  // measured 2.0 @ 59% busy
  {"5MP", 2592, 1944, 4, 4, 1},    // PY260 only - unreachable on OV5640, left as inherited
  // Custom sizes past the driver's framesize_t. That enum lives in the precompiled
  // esp32-camera library, so a size it lacks has to be carried here instead. Rows below this
  // point are never handed to the driver - setSensorSize() maps them onto a base size and
  // then overrides only the registers that differ
  {"1280X960", 1280, 960, 24, 3, 1} // measured 24.5 at HTS_FLOOR, 19.1 before it
};

// 1280x960 is the largest 4:3 size the OV5640 can still read 2x2 binned, and binning is what
// decides frame rate on this part: measured fps is PIXCLK/(HTS*VTS) when binned and half that
// at full resolution, confirmed within 2% at five frame sizes. FHD cannot bin at all - a
// binned read of the 2592 wide array yields about 1312 columns, so 1920 is unreachable -
// which is why FHD manages 5.9fps while HD does 25.3. Datasheet table 2-1 lists 1280x960 as a
// native binned mode. The driver has no enum entry for it, so it is configured as XGA, which
// uses the same 2624x1952 binned window and the same HTS/VTS, with only the DVP output size
// overridden. It therefore runs at XGA's exact frame rate while carrying 56% more pixels.
#define NUM_CUSTOM_FS 1
#define FS_1280X960 (FRAMESIZE_INVALID + 0) // index 25, first row past the driver's enum
#define FS_1280X960_BASE FRAMESIZE_XGA      // same binned window, same line timing
