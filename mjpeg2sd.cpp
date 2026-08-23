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
int moveStopSecs = 2; // secs between each check for stop, also determines post motion time
// each frame now also writes an audio chunk, so the index needs 2 entries per frame.
// 5000 keeps the index buffer at 160kB, half what 20000 video only entries used
int maxFrames = 5000; // maximum number of frames in video before auto close

// record timelapse avi independently of motion capture, file name has same format as avi except ends with T
int tlSecsBetweenFrames; // too short interval will interfere with other activities
int tlDurationMins; // a new file starts when previous ends
int tlPlaybackFPS;  // rate to playback the timelapse, min 1

// status & control fields
uint8_t FPS = 0;
bool nightTime = false;
uint8_t fsizePtr; // index to frameData[]
uint8_t minSeconds = 5; // default min video length (includes POST_MOTION_TIME)
bool doRecording = true; // whether to capture to SD or not
uint8_t xclkMhz = 20; // camera clock rate MHz
bool doKeepFrame = false;
static bool haveSrt = false;
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
bool timeLapseOn = false;
int dashCamOn = 0; // whether to use / duration of dashcam style continuous recording

#ifndef AUXILIARY
framesize_t maxFS = FRAMESIZE_SVGA; // default

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
#if INCLUDE_AUDIO
  startAudioRecord();
#endif
#if INCLUDE_TELEM
  haveSrt = startTelemetry();
#endif
  // initialisation of counters
  startTime = millis();
  frameCnt = fTimeTot = wTimeTot = dTimeTot = vidSize = 0;
  haveWav = false;
  highPoint = AVI_HEADER_LEN; // allot space for AVI header
  prepAviIndex();
}

static inline bool doMonitor(bool capturing) {
  // monitor incoming frames for motion
  static uint16_t motionCnt = 0;
  // ratio for monitoring stop during capture / movement prior to capture
  uint16_t checkRate = (capturing) ? FPS * moveStopSecs : FPS / moveStartChecks;
  if (!checkRate) checkRate = 1;
  if (++motionCnt / checkRate) motionCnt = 0; // time to check for motion
  return !(bool)motionCnt;
}

static void timeLapse(camera_fb_t* fb, bool tlStop = false) {
  // record a time lapse avi
  // Note that if FPS changed during time lapse recording,
  //  the time lapse counters wont be modified
  static int frameCntTL, requiredFrames, intervalCnt = 0;
  static int intervalMark = tlSecsBetweenFrames * saveFPS;
  static File tlFile;
  static char TLname[FILE_NAME_LEN];
  if (tlStop) {
    // force save of file on controlled shutdown
    intervalCnt = 0;
    requiredFrames = frameCntTL - 1;
  }
  if (timeLapseOn) {
    if (timeSynchronized) {
      if (!frameCntTL) {
        // initialise time lapse avi
        requiredFrames = tlDurationMins * 60 / tlSecsBetweenFrames;
        if (requiredFrames > maxFrames) {
          LOG_WRN("Frames required for timelapse %u reduced to max frame limit %u", requiredFrames, maxFrames);
          requiredFrames = maxFrames;
        }
        dateFormat(partName, sizeof(partName), true);
        STORAGE.mkdir(partName); // make date folder if not present
        dateFormat(partName, sizeof(partName), false);
        int tlen = snprintf(TLname, FILE_NAME_LEN - 1, "%s_%s_%u_%u_T.%s",
                            partName, frameData[fsizePtr].frameSizeStr, tlPlaybackFPS, tlDurationMins, AVI_EXT);
        if (tlen > FILE_NAME_LEN - 1) LOG_WRN("file name truncated");
        if (STORAGE.exists(TLTEMP)) STORAGE.remove(TLTEMP);
        tlFile = STORAGE.open(TLTEMP, FILE_WRITE);
        tlFile.write(aviHeader, AVI_HEADER_LEN); // space for header
        prepAviIndex(true);
        LOG_INF("Started time lapse file %s, duration %u mins, for %u frames",  TLname, tlDurationMins, requiredFrames);
        frameCntTL++; // to stop re-entering
      }
      // switch on light before capture frame if nightTime
#if INCLUDE_PERIPH
      if (nightTime && intervalCnt == intervalMark - (saveFPS / 2)) setLamp(lampLevel);
#endif
      if (intervalCnt > intervalMark) {
        // save this frame to time lapse avi
#if INCLUDE_PERIPH
        if (!lampNight) setLamp(0);
#endif
        uint8_t hdrBuff[CHUNK_HDR];
        memcpy(hdrBuff, dcBuf, 4);
        // align end of jpeg on 4 byte boundary for AVI
        uint16_t filler = (4 - (fb->len & 0x00000003)) & 0x00000003;
        uint32_t jpegSize = fb->len + filler;
        memcpy(hdrBuff + 4, &jpegSize, 4);
        tlFile.write(hdrBuff, CHUNK_HDR); // jpeg frame details
        tlFile.write(fb->buf, jpegSize);
        buildAviIdx(jpegSize, true, true); // save avi index for frame
        frameCntTL++;
        intervalCnt = 0;
        intervalMark = tlSecsBetweenFrames * saveFPS;  // recalc in case FPS changed
      }
      intervalCnt++;
      if (frameCntTL > requiredFrames) {
        // finish timelapse recording
        xSemaphoreTake(aviMutex, portMAX_DELAY);
        buildAviHdr(tlPlaybackFPS, fsizePtr, --frameCntTL, true);
        xSemaphoreGive(aviMutex);
        // add index
        finalizeAviIndex(frameCntTL, true);
        size_t idxLen = 0;
        do {
          idxLen = writeAviIndex(iSDbuffer, RAMSIZE, true);
          tlFile.write(iSDbuffer, idxLen);
        } while (idxLen > 0);
        // add header
        tlFile.seek(0, SeekSet); // start of file
        tlFile.write(aviHeader, AVI_HEADER_LEN);
        tlFile.close();
        STORAGE.rename(TLTEMP, TLname);
        frameCntTL = intervalCnt = 0;
        LOG_INF("Finished time lapse: %s", TLname);
#if INCLUDE_FTP_HFS
        if (autoUpload) fsStartTransfer(TLname); // Transfer it to remote ftp server if requested
#endif
      }
    }
  } else frameCntTL = intervalCnt = 0;
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
static uint32_t writeAudioChunk() {
  // append the audio captured since the last video frame as an 01wb chunk, and return
  // the ms spent writing it so the caller can include it in the SD storage total.
  // Interleaving here replaces the temporary WAV file that used to be read back off
  // SD and rewritten into the AVI when the recording closed
  uint8_t* audBuf = NULL;
  size_t audLen = getAudioChunk(&audBuf);
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
  wTime += writeAudioChunk(); // audio is an SD write too, so count it as storage time
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
  uint32_t vidDuration = millis() - startTime;
  uint32_t vidDurationSecs = lround(vidDuration / 1000.0);
  logLine();
  LOG_VRB("Capture time %lu, min seconds: %u ", vidDurationSecs, minSeconds);

  cTime = millis();
#if INCLUDE_AUDIO
  // stop the mic, then append whatever audio is still pending as a final 01wb chunk.
  // Must happen before the write buffer is flushed below
  finishAudioRecord(true);
  writeAudioChunk();
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
  buildAviHdr(actualFPSint, fsizePtr, frameCnt);
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
  if (vidDurationSecs >= minSeconds) {
    // name file to include actual dateTime, FPS, duration, and frame count
    int alen = snprintf(aviFileName, FILE_NAME_LEN - 1, "%s_%s_%u_%lu%s%s%s.%s",
                        partName, frameData[fsizePtr].frameSizeStr, actualFPSint, vidDurationSecs,
                        haveWav ? "_S" : "", haveSrt ? "_M" : "", dashCamOn ? "_C" : "", AVI_EXT);
    if (alen > FILE_NAME_LEN - 1) LOG_WRN("file name truncated");
    STORAGE.rename(AVITEMP, aviFileName);
    LOG_VRB("AVI close time %lu ms", millis() - hTime);
    cTime = millis() - cTime;
#if INCLUDE_TELEM
    stopTelemetry(aviFileName);
#endif
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
    // delete too small files if exist
    STORAGE.remove(AVITEMP);
    LOG_INF("Insufficient capture duration: %lu secs", vidDurationSecs);
    return false;
  }
}

static boolean processFrame() {
  // get camera frame
  static bool haveMotion = false;
  bool res = true;
  uint32_t dTime = millis();

  camera_fb_t* fb = esp_camera_fb_get();
  if (fb == NULL || !fb->len || fb->len > maxFrameBuffSize) return false;
  timeLapse(fb);

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

  // determine if time to check for motion change
  int reasonId = 0;
  bool prevMotion = haveMotion;
  if (doMonitor(doRecording ? isCapturing : dbgMotion ? false : true)) {
    if (useMotion && checkMotion(fb, isCapturing)) reasonId = 1; // check 1 in N frames
    if (!useMotion) checkMotion(fb, false, true); // calc light level only
#if INCLUDE_PERIPH
    if (pirUse && getPIRval()) reasonId = 2;
#endif
#if INCLUDE_I2C && USE_MPU
    if (accelUse && checkAccelMove()) reasonId = 3;
#endif
    haveMotion = (reasonId) ? true : false;
  }

  // process motion status
  if (haveMotion && !prevMotion) {
    // start of movement detection
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

  // recording status
  bool prevCapture = isCapturing;
  isCapturing = haveMotion | forceRecord;
  if (isCapturing && !prevCapture) {
    // new movement has occurred or record button pressed, start recording
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
    openAvi();
  }

  if (isCapturing) {
    // capture is ongoing
    showProgress();
    if (frameCnt < frameLimit) {
      dTimeTot += millis() - dTime;
      saveFrame(fb);
      if (frameCnt >= frameLimit) {
        // stop saving frames for this avi as limit reached
        isCapturing = forceRecord = false;
        if (!dashCamOn) {
          logLine();
          LOG_WRN("Auto closed recording after %u frames", frameLimit);
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
  return res;
}

static void captureTask(void* parameter) {
  // woken by frame timer when time to capture frame
  uint32_t ulNotifiedValue;
  while (true) {
    ulNotifiedValue = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
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
  return setFPS(frameData[fsizePtr].defaultFPS);
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
  // set initial camera framesize and FPS from configs
  sensor_t * s = esp_camera_sensor_get();
  s->set_framesize(s, (framesize_t)fsizePtr);
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
#if INCLUDE_TINYML
  LOG_INF("%sUsing TinyML", mlUse ? "" : "Not ");
#endif

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
#if INCLUDE_I2C
    if (accelUse) LOG_INF("- accelerometer movement");
#endif
    if (useMotion) LOG_INF("- move in front of camera");
  }
  logLine();
  debugMemory("prepRecording");
  return true;
}

void appShutdown() {
  timeLapse(NULL, true);
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
#if INCLUDE_TELEM
  deleteTask(telemetryHandle);
#endif
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

void dumpCamRegs() {
  // report the OV5640 clock tree and frame timing as actually programmed
  sensor_t* s = esp_camera_sensor_get();
  if (s == NULL) {
    LOG_WRN("dumpCam: no camera sensor");
    return;
  }
  int r34 = camReg(s, OV5640_SC_PLL_CTRL0);
  int r35 = camReg(s, OV5640_SC_PLL_CTRL1);
  int mul = camReg(s, OV5640_SC_PLL_CTRL2);
  int r37 = camReg(s, OV5640_SC_PLL_CTRL3);
  int r39 = camReg(s, OV5640_SC_PLL_CTRL5);
  int r08 = camReg(s, OV5640_SC_PLLS_CTRL0);
  int pclkDiv = camReg(s, OV5640_PCLK_DIV);
  int pclkMan = camReg(s, OV5640_PCLK_MANUAL);
  if (mul < 0 || r35 < 0 || r37 < 0) {
    LOG_WRN("dumpCam: SCCB read failed - camera may be unresponsive");
    return;
  }

  // decode, then recompute the clocks using the driver's own arithmetic (calc_sysclk)
  int sysDiv = (r35 >> 4) & 0x0F;
  if (!sysDiv) sysDiv = 1;
  int preDiv = r37 & 0x0F;
  bool root2x = (r37 & 0x10) ? true : false;
  int pclkRoot = (r08 >> 4) & 0x03;
  bool bypass = (r39 & 0x80) ? true : false;
  bool pclkManual = (pclkMan == 0x22);
  static const float preDivMap[] = {1, 1, 2, 3, 4, 1.5, 6, 2.5, 8};
  static const int pclkRootMap[] = {1, 2, 4, 8};
  float preDivVal = preDivMap[preDiv > 8 ? 0 : preDiv];

  uint32_t xclk = xclkMhz * OneMHz;
  uint32_t refin = (uint32_t)(xclk / preDivVal);
  uint32_t vco = refin * mul / (root2x ? 2 : 1);
  uint32_t pllClk = bypass ? xclk : (vco / sysDiv * 2 / 5); // 2/5 is 10 bit mode
  uint32_t sysClk = pllClk / 4;
  uint32_t pclk = pllClk / pclkRootMap[pclkRoot] / ((pclkManual && pclkDiv) ? pclkDiv : 2);

  int hts = camReg16(s, OV5640_X_TOTAL_SIZE);
  int vts = camReg16(s, OV5640_X_TOTAL_SIZE + 2);
  float sensorFps = (hts > 0 && vts > 0) ? (float)sysClk / ((float)hts * vts) : 0.0;

  LOG_INF("******** OV5640 clock tree ********");
  LOG_INF("Frame size: %s, XCLK %uMHz (fixed)", frameData[fsizePtr].frameSizeStr, xclkMhz);
  LOG_INF("PLL regs: 0x3034=0x%02X 0x3035=0x%02X 0x3036=%d 0x3037=0x%02X 0x3039=0x%02X",
    r34, r35, mul, r37, r39);
  LOG_INF("PCLK regs: 0x3108=0x%02X 0x3824=%d 0x460C=0x%02X 0x3103=0x%02X",
    r08, pclkDiv, pclkMan, camReg(s, OV5640_SYSREM_RESET));
  LOG_INF("Decoded: mul=%d sys_div=%d pre_div=%d(/%.1f) root_2x=%d pclk_root=%d(/%d) pclk_manual=%d pclk_div=%d bypass=%d",
    mul, sysDiv, preDiv, preDivVal, root2x, pclkRoot, pclkRootMap[pclkRoot], pclkManual, pclkDiv, bypass);
  LOG_INF("Clocks: REFIN %.2fMHz, VCO %.1fMHz, PLL_CLK %.2fMHz, SYSCLK %.2fMHz, PCLK %.2fMHz",
    refin / 1000000.0, vco / 1000000.0, pllClk / 1000000.0, sysClk / 1000000.0, pclk / 1000000.0);
  LOG_INF("Timing: HTS %d, VTS %d -> sensor ceiling %.1f fps (app FPS %u)", hts, vts, sensorFps, FPS);
  LOG_INF("Window: start %d,%d end %d,%d output %dx%d offset %d,%d",
    camReg16(s, OV5640_X_ADDR_ST), camReg16(s, OV5640_X_ADDR_ST + 2),
    camReg16(s, OV5640_X_ADDR_END), camReg16(s, OV5640_X_ADDR_END + 2),
    camReg16(s, OV5640_X_OUTPUT_SIZE), camReg16(s, OV5640_X_OUTPUT_SIZE + 2),
    camReg16(s, OV5640_X_OFFSET), camReg16(s, OV5640_X_OFFSET + 2));
  LOG_INF("Subsample: 0x3814=0x%02X 0x3815=0x%02X 0x3820=0x%02X 0x3821=0x%02X",
    camReg(s, OV5640_X_INCREMENT), camReg(s, OV5640_Y_INCREMENT),
    camReg(s, OV5640_TIMING_TC_R20), camReg(s, OV5640_TIMING_TC_R21));
  LOG_INF("Die temp: %.1fC, free heap %s, free PSRAM %s",
    readInternalTemp(), fmtSize(ESP.getFreeHeap()), fmtSize(ESP.getFreePsram()));
  LOG_INF("**********************************");
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
  if (FRAMESIZE_INVALID != sizeof(frameData) / sizeof(frameData[0]))
    LOG_ERR("framesize_t entries %d != frameData entries %d", FRAMESIZE_INVALID, sizeof(frameData) / sizeof(frameData[0]));
  if (!camPower()) return false;
#if INCLUDE_I2C
  if (shareI2C(SIOD_GPIO_NUM, SIOC_GPIO_NUM)) {
    // if shared, set camera to use shared
    siodGpio = -1;
    siocGpio = -1;
  }
#endif

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
      // set frame size to configured value
      char fsizePtrStr[4];
      if (retrieveConfigVal("framesize", fsizePtrStr)) s->set_framesize(s, (framesize_t)(atoi(fsizePtrStr)));
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
      if (timeLapseOn) dashCamOn = 0;
      if (dashCamOn) {
        timeLapseOn = useMotion = false; // disable timeLapse and motion recording
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
