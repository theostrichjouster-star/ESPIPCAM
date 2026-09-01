// streamServer handles streaming, playback, file downloads
// each sustained activity uses a separate task if available
// - web streaming, playback, file downloads use task 0
// - video streaming uses task 1
// - audio streaming uses task 2
// - subtitle streaming uses task 3
//
// s60sc 2022 - 2025

#include "appGlobals.h"

// stream separator
#define STREAM_CONTENT_TYPE "multipart/x-mixed-replace;boundary=" BOUNDARY_VAL
#define JPEG_BOUNDARY "\r\n--" BOUNDARY_VAL "\r\n"
#define JPEG_TYPE "Content-Type: image/jpeg\r\nContent-Length: %10u\r\n\r\n"
#define HDR_BUF_LEN 128 // must hold JPEG_BOUNDARY and JPEG_TYPE as one string
#define END_WAIT 100

static bool forcePlayback = false; // browser playback status
bool streamVid = false;
bool streamAud = false;
static bool isStreaming[MAX_STREAMS] = {false};
size_t streamBufferSize[MAX_STREAMS] = {0};
byte* streamBuffer[MAX_STREAMS] = {NULL}; // buffer for stream frame
static char variable[FILE_NAME_LEN]; 
static char value[FILE_NAME_LEN];
uint16_t sustainId = 0;
uint8_t numStreams = 1;
uint8_t vidStreams = 1;

#ifndef AUXILIARY

TaskHandle_t sustainHandle[MAX_STREAMS]; 
struct httpd_sustain_req_t {
  httpd_req_t* req = NULL;
  uint8_t taskNum; 
  char activity[16];
  bool inUse = false; 
};
httpd_sustain_req_t sustainReq[MAX_STREAMS];

static void showPlayback(httpd_req_t* req) {
  // output playback file to browser
  esp_err_t res = ESP_OK; 
  stopPlaying();
  forcePlayback = true;
  // doPlayback is a global already set by the sfile handler, so both failure paths
  // below must clear it - otherwise a stale true falls through to openSDfile() with
  // a file that cannot be opened, and File::read() then returns (size_t)-1
  if (STORAGE.exists(inFileName)) {
    if (stopPlayback) {
      LOG_WRN("Playback refused - capture in progress");
      doPlayback = false;
    } else {
      LOG_INF("Playback enabled (SD file selected)");
      doPlayback = true;
    }
  } else {
    LOG_WRN("File %s doesn't exist when Playback requested", inFileName);
    doPlayback = false;
  }

  if (doPlayback) {
    // playback mjpeg from SD
    mjpegStruct mjpegData;
    // output header for playback request
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
    char hdrBuf[HDR_BUF_LEN];
    openSDfile(inFileName);
    mjpegData = getNextFrame(true);
    while (doPlayback) {
      size_t jpgLen = mjpegData.buffLen;
      size_t buffOffset = mjpegData.buffOffset;
      if (!jpgLen && !buffOffset) {
        // complete mjpeg playback streaming
        res = httpd_resp_sendstr_chunk(req, JPEG_BOUNDARY);
        doPlayback = false; 
      } else {
        if (jpgLen) {
          if (mjpegData.jpegSize) { // start of frame
            // send mjpeg header 
            if (res == ESP_OK) res = httpd_resp_sendstr_chunk(req, JPEG_BOUNDARY);
            snprintf(hdrBuf, HDR_BUF_LEN-1, JPEG_TYPE, mjpegData.jpegSize);
            if (res == ESP_OK) res = httpd_resp_sendstr_chunk(req, hdrBuf);   
          } 
          // send buffer 
          if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char*)iSDbuffer+buffOffset, jpgLen);
        }
        if (res == ESP_OK) mjpegData = getNextFrame(); 
        else {
          // when browser closes playback get send error
          LOG_VRB("Playback aborted due to error: %s", espErrMsg(res));
          stopPlaying();
        }
      }
    }
    if (res == ESP_OK) httpd_resp_sendstr_chunk(req, NULL);
    sustainId = currEpoch;
  } 
  if (!doPlayback && forcePlayback) {
    // switch off playback on browser
    forcePlayback = false;
    wsAsyncSendJson("ustatus", "\"forcePlayback\":0");
  }
}

static void showStream(httpd_req_t* req, uint8_t taskNum) {
  // start live streaming to browser
  esp_err_t res = ESP_OK; 
  size_t jpgLen = 0;
  uint8_t* jpgBuf = NULL;
  uint32_t startTime = millis();
  uint32_t frameCnt = 0;
  uint32_t mjpegLen = 0;
  isStreaming[taskNum] = true;
  streamBufferSize[taskNum] = 0;
  if (!taskNum) motionJpegLen = 0;
  // TCP_NODELAY was tried here and MEASURED AS A LOSS - 177 -> 165 frames per 20s at
  // identical frame sizes, ~6% of goodput, repeatable across runs (COM4, 1 Sep). The
  // theory was that Nagle stalls the small header chunk for an RTT; the reality is that
  // httpd_resp_send_chunk issues several small writes per chunk (size line, body,
  // trailing CRLF) and Nagle coalescing those is worth more than the stall costs.
  // Leave Nagle enabled. The stream is window-limited (TCP_SND_BUF / RTT), and that
  // ceiling is not reachable by socket options - see BOARD_TESTING.md
  // output header for streaming request
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
  char hdrBuf[HDR_BUF_LEN];
  while (isStreaming[taskNum]) {
    // stream from camera at current frame rate
    if (xSemaphoreTake(frameSemaphore[taskNum], pdMS_TO_TICKS(MAX_FRAME_WAIT)) == pdFAIL) {
      // failed to take semaphore, allow retry
      streamBufferSize[taskNum] = 0;
      continue;
    }
    if (dbgMotion && !taskNum) {
      // motion tracking stream on task 0 only, wait for new move mapping image
      if (xSemaphoreTake(motionSemaphore, pdMS_TO_TICKS(MAX_FRAME_WAIT)) == pdFAIL) continue;
      // use zone overlay image created by zoneMotion()
      jpgLen = motionJpegLen;
      if (!jpgLen) continue;
      jpgBuf = motionJpeg;
    } else {
      // live stream 
      if (!streamBufferSize[taskNum]) continue;
      jpgLen = streamBufferSize[taskNum];
      // use frame stored by processFrame()
      jpgBuf = streamBuffer[taskNum];
    }
    if (res == ESP_OK) {
      // send next frame in stream, boundary and jpeg header as a single chunk
      snprintf(hdrBuf, HDR_BUF_LEN-1, JPEG_BOUNDARY JPEG_TYPE, jpgLen);
      res = httpd_resp_sendstr_chunk(req, hdrBuf);
      if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char*)jpgBuf, jpgLen);
      frameCnt++;
    }
    mjpegLen += jpgLen;
    jpgLen = streamBufferSize[taskNum] = 0;
    if (dbgMotion && !taskNum) motionJpegLen = 0;
    if (res != ESP_OK) {
      // get send error when browser closes stream 
      LOG_VRB("Streaming aborted due to error: %s", espErrMsg(res));
      isStreaming[taskNum] = false;
    }     
  }
  if (res == ESP_OK) httpd_resp_sendstr_chunk(req, NULL);
  uint32_t mjpegTime = millis() - startTime;
  float mjpegTimeF = float(mjpegTime) / 1000; // secs
  LOG_INF("MJPEG: %lu frames, total %s in %0.1fs @ %0.1ffps", frameCnt, fmtSize(mjpegLen), mjpegTimeF, (float)(frameCnt) / mjpegTimeF);
}

static void audioStream(httpd_req_t* req, uint8_t taskNum) {
  // output WAV audio stream to remote NVR
#if INCLUDE_AUDIO
  if (micGain) {
    esp_err_t res = ESP_OK;
    httpd_resp_set_type(req, "audio/wav");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    isStreaming[taskNum] = true;
    uint32_t totalSamples = 0;
    audioBytes = WAV_HDR_LEN;
    updateWavHeader();
    while (isStreaming[taskNum]) {
      if (audioBytes) {
        res = httpd_resp_send_chunk(req, (const char*)audioBuffer, audioBytes); 
        audioBytes = 0;
      } else delay(20); // allow time for buffer to load
      if (res != ESP_OK) isStreaming[taskNum] = false; // client connection closed
      else totalSamples += audioBytes / 2; // 16 bit samples
    }
    audioBytes = 1; // stop loading of buffer
    if (res == ESP_OK) httpd_resp_sendstr_chunk(req, NULL);
    LOG_INF("WAV: sent %lu samples", totalSamples);
  } else LOG_WRN("No ESP mic defined or mic is off");
#else 
  httpd_resp_sendstr(req, NULL);
#endif
}

// Downloads had no cancellation surface at all: showStream() polls isStreaming[] and
// showPlayback() polls doPlayback, but fileHandler -> downloadFile -> sendChunks blocks
// on SD reads and httpd sends until EOF or the peer disconnects. A stop request could
// not reach it, so slot 0 stayed inUse through the OTA quiesce window and endTasks()
// then vTaskDeleted the task inside FATFS or lwip - the exact wedge class OTAprereq
// exists to prevent. sendChunks polls sustainCancelled() between chunks
static volatile bool cancelDownload = false;

bool sustainCancelled() {
  // Task-scoped on purpose. sendChunks also serves ordinary page requests from the
  // httpd worker (/web?, webdav), and those must never be aborted by a flag raised for
  // the sustain task - only the download running on slot 0 is cancellable here
  return cancelDownload && xTaskGetCurrentTaskHandle() == sustainHandle[0];
}

void stopSustainTask(int taskId) {
  isStreaming[taskId] = false;
  if (taskId == 0) cancelDownload = true; // only task 0 serves downloads
}

bool viewerActive() {
  // a live video consumer is attached, so the sensor must stay at the capture resolution
  // for the whole session rather than switching under it. Slot 0 is the browser live
  // stream, slot 1 the NVR video stream - slot 2 is audio, which has no bearing on the
  // sensor. showPlayback() uses doPlayback and does not set isStreaming[0]
  return isStreaming[0] || isStreaming[1];
}

bool nvrActive() {
  return isStreaming[1];
}

static void sustainTask(void* p) {
  // process sustained http(s) requests as a separate task 
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    uint8_t i = *(uint8_t*)p; // identify task number
    if (i == 0) {
      if (!strcmp(sustainReq[i].activity, "download")) fileHandler(sustainReq[i].req, true); 
      else if (!strcmp(sustainReq[i].activity, "playback")) showPlayback(sustainReq[i].req);
      else if (!strcmp(sustainReq[i].activity, "stream")) showStream(sustainReq[i].req, i);
    } 
    else if (i == 1) showStream(sustainReq[i].req, i);
    else if (i == 2) audioStream(sustainReq[i].req, i);
    // cleanup as request now complete on return
    if (httpd_req_async_handler_complete(sustainReq[i].req) != ESP_OK) LOG_ERR("Failed to free req for sustain task: %i", i);
    sustainReq[i].inUse = false; 
  }
  vTaskDelete(NULL);
}

void startSustainTasks() {
  // start httpd sustain tasks
  if (streamVid) numStreams = vidStreams = 2;
  if (streamAud) numStreams = 3;
  if (numStreams > MAX_STREAMS) {
    LOG_WRN("numStreams %d exceeds MAX_STREAMS %d", numStreams, MAX_STREAMS);
    numStreams = MAX_STREAMS;
  }
  if (maxFrameBuffSize * (vidStreams + 1) > ESP.getFreePsram()) {
    LOG_WRN("Insufficient PSRAM for NVR streams");
    vidStreams = 1;
    streamVid = streamAud = false;
  }
  for (int i = 0; i < vidStreams; i++)
    if (streamBuffer[i] == NULL) streamBuffer[i] = (byte*)ps_malloc(maxFrameBuffSize); 

  for (int i = 0; i < numStreams; i++) {
    sustainReq[i].taskNum = i; // so task knows its number
    xTaskCreateWithCaps(sustainTask, "sustainTask", SUSTAIN_STACK_SIZE, &sustainReq[i].taskNum, SUSTAIN_PRI, &sustainHandle[i], STACK_MEM);
  }
  
  LOG_INF("Started %d sustain tasks", numStreams);
  debugMemory("startSustainTasks");
}

bool streamsBusy() {
  // any sustained client - browser stream, playback, download, NVR feeds - needs frames
  // at the user's rate; the sensor idle throttle asks before slowing the sensor down
  for (int i = 0; i < numStreams; i++) if (sustainReq[i].inUse) return true;
  return false;
}

esp_err_t appSpecificSustainHandler(httpd_req_t* req) {
  // first check if authentication is required & passed
  esp_err_t res = ESP_FAIL;
  if (checkAuth(req)) { 
    // handle long running request as separate task
    // obtain details from query string
    if (extractQueryKeyVal(req, variable, value) == ESP_OK) {
      // playback, download, web streaming uses task 0
      // remote streaming eg video uses task 1, audio task 2
      uint8_t taskNum = 99;
      if (!strcmp(variable, "download")) taskNum = 0;
      else if (!strcmp(variable, "playback")) taskNum = 0;
      else if (!strcmp(variable, "stream")) taskNum = 0;
      else if (!strcmp(variable, "video")) taskNum = 1;
      else if (!strcmp(variable, "audio")) taskNum = 2;
      if (taskNum < numStreams) {
        if (taskNum == 0) {
          if (req->method == HTTP_HEAD) { 
            // task check request from app web page
            if (sustainReq[taskNum].inUse) {
              // task not free, try stopping it for new stream
              if (!strcmp(variable, "stream")) {
                isStreaming[taskNum] = false;
                if (!taskNum) doPlayback = false; // only for task 0
                delay(END_WAIT + 100);
              }
            } 
            if (sustainReq[taskNum].inUse) {
              LOG_WRN("Task %d not free", taskNum);
              httpd_resp_set_status(req, "500 No free task");
            }
            else {
              sustainId = currEpoch; // task available
              res = ESP_OK;
            }
            httpd_resp_sendstr(req, NULL);
            return res;
          }
        } else {
          // stop remote streaming if currently active
          if (taskNum < MAX_STREAMS) {
            if (sustainReq[taskNum].inUse) {
              isStreaming[taskNum] = false;
              delay(END_WAIT + 100);
            }
          }
        }
            
        // action request if task available
        if (!sustainReq[taskNum].inUse) {
          // make copy of request data and pass request to task indexed by request
          uint8_t i = taskNum;
          sustainReq[i].inUse = true;
          httpd_req_t* copy = NULL;
          if ((res = httpd_req_async_handler_begin(req, &copy)) != ESP_OK) {
            LOG_ERR("Failed to copy req for sustain task: %i", i);
            return res;
          }
          sustainReq[i].req = copy;
          strncpy(sustainReq[i].activity, variable, sizeof(sustainReq[i].activity) - 1);
          // clear any cancel left over from stopping the previous slot 0 activity, or
          // this new request would abort itself on its first chunk
          if (i == 0) cancelDownload = false;
          // activate relevant task
          xTaskNotifyGive(sustainHandle[i]);
          return ESP_OK;
        } else httpd_resp_set_status(req, "500 No free task");
      } else {
        if (taskNum < MAX_STREAMS) LOG_WRN("Task not created for stream: %s, numStreams %d", variable, numStreams);
        else LOG_WRN("Invalid task id: %s", variable);
        httpd_resp_set_status(req, "400 Invalid url");
      }
    } else httpd_resp_set_status(req, "400 Bad URL");
    httpd_resp_sendstr(req, NULL);
  } 
  return res;
}

#else

// dummies
esp_err_t appSpecificSustainHandler(httpd_req_t* req) {return ESP_OK;}
bool streamsBusy() {return false;}
bool sustainCancelled() {return false;}

#endif
