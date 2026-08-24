
/* 
 Detect movement in sequential images using background subtraction.
 
 Very small (96x96) bitmaps are used both to provide image smoothing to reduce spurious motion changes 
 and to enable rapid processing
 Bitmaps can either be color or grayscale. Color requires triple memory
 of grayscale and more processing.

 The amount of change between images will depend on the frame rate.
 A faster frame rate will need a higher sensitivity

 When frame size is changed the OV2640 outputs a few glitched frames whilst it 
 makes the transition. These could be interpreted as spurious motion.

 s60sc 2020, 2023, 2025
*/

#include "appGlobals.h"

#define INACTIVE_COLOR 96 // color for inactive motion pixel
#define JPEG_QUAL 80 // % quality for generated motion detect jpeg
  
// motion recording parameters
bool dbgMotion = false;
int detectMotionFrames = 5; // min sequence of changed frames to confirm motion 
int detectNightFrames = 10; // frames of sequential darkness to avoid spurious day / night switching
// define region of interest, ie exclude top and bottom of image from movement detection if required
// divide image into detectNumBands horizontal bands, define start and end bands of interest, 1 = top
int detectNumBands = 10;
int detectStartBand = 3;
int detectEndBand = 8; // inclusive
int detectChangeThreshold = 15; // min difference in pixel comparison to indicate a change
uint8_t colorDepth; // set by depthColor config
static size_t stride;

uint8_t lightLevel; // Current ambient light level
uint8_t nightSwitch = 20; // initial white level % for night/day switching
// tuning aids, reported and reset by dumpMotionStats()
int motionPeakChange = 0; // highest changed pixel count since the last stats dump
int motionThreshold = 0; // the threshold that count was measured against
float motionVal = 8.0; // initial motion sensitivity setting
uint8_t* motionJpeg = NULL;
size_t motionJpegLen = 0;

#ifndef AUXILIARY

#if INCLUDE_NEW_JPG
// use esp_new_jpeg library instead of built in
#include <esp_jpeg_dec.h>
#include <esp_jpeg_enc.h>

struct esp_jpeg_stream {
    jpeg_dec_handle_t       jpeg_dec;
    jpeg_dec_io_t*          jpeg_io;
    jpeg_dec_header_info_t* out_info;
    jpeg_pixel_format_t     output_type;
};
typedef struct esp_jpeg_stream* esp_jpeg_stream_handle_t;

static void jpgReduce(int inWidth, int inHeight, uint8_t downsize, int* outWidth, int* outHeight);
static bool jpg2rgbOpen(esp_jpeg_stream_handle_t jpegHandle, uint16_t width, uint16_t height);
static bool jpg2rgb(esp_jpeg_stream_handle_t jpegHandle, uint8_t* inputBuf, int inputLen, uint8_t* outputBuf);
static bool jpg2rgbClose(esp_jpeg_stream_handle_t jpegHandle);
static size_t rgb2jpg(uint8_t* rgb888, int width, int height, int qual, uint8_t* outputBuf);
#else
// built in
static bool jpg2rgb(const uint8_t* src, size_t src_len, uint8_t* out, uint8_t scale);
#endif

/**********************************************************************************/


bool isNight(uint8_t nightSwitch) {
  // check if night time for suspending recording
  // or for switching relay if enabled
  static bool nightTime = false;
  static uint16_t nightCnt = 0;
  if (nightTime) {
    if (lightLevel > nightSwitch) {
      // light image
      if (nightCnt > 0) nightCnt--;
      // signal day time after given sequence of light frames
      if (nightCnt == 0) {
        nightTime = false;
        LOG_INF("Day time");
      }
    }
  } else {
    if (lightLevel < nightSwitch) {
      // dark image
      nightCnt++;
      // signal night time after given sequence of dark frames
      if (nightCnt > detectNightFrames) {
        nightTime = true;     
        nightCnt = detectNightFrames;           
        LOG_INF("Night time"); 
      }
    } else {
      // back to light while not yet in nightTime: reset counter
      if (nightCnt > 0) nightCnt--;
    }
  } 
  return nightTime;
}

static void rgbToGray(uint8_t* buffer, int width, int height) {
  // convert rgb buffer to grayscale in place
  for (int i = 0; i < width * height; ++i) {
    int index = i * 3;
    // Calculate grayscale value using luminance formula
    buffer[i] = (uint8_t)(((77 * buffer[index]) + (150 * buffer[index + 1]) + (29 * buffer[index + 2])) >> 8);
  }
}

bool checkMotion(camera_fb_t* fb, bool motionStatus, bool lightLevelOnly) {
  // check difference between current and previous image (subtract background)
  // convert image from JPEG to downscaled RGB888 or 8 bit grayscale bitmap
  // The decoded bitmap is compared at its native size. It used to be bilinear resampled up
  // to a fixed 96x96 first, for two reasons that no longer hold:
  //   - normalisation: detection once ran at the capture resolution, so the bitmap varied
  //     (QVGA/4 80x60, SVGA/8 100x76, HD/8 160x90) and had to be squared off to compare
  //     against prevBuff. Detection is now pinned to MOTION_DETECT_FS, so it never varies.
  //   - smoothing: true when the source was larger than the target, where averaging
  //     neighbours suppresses noise. From VGA/8 it was an UPSCALE, 80x60 to 96x96, which
  //     cannot recover smoothing that way - it invented 4,416 interpolated pixels carrying
  //     no new information, in floating point, on every check.
  // Measured 8ms of a ~40ms check, plus a whole buffer and a copy.
  // The square shape was probably inherited from TinyML, since removed.
  // keyed off sensorFS, the size the sensor is actually at, not fsizePtr - the capture
  // resolution the user picked is never what gets decoded here
  if (!motionSizeAllowed(sensorFS)) return false; // pixel count, not enum index
  // The sensor has to actually be at the size sensorFS claims. Anything that changes the
  // frame size without going through setSensorSize() leaves this stale, and the scaling
  // below would then be derived from the wrong size - decoding a large frame into a buffer
  // sized for the detection frame. Cheap to check, and it fails a check instead of
  // overrunning rgbBuf
  uint16_t encodedW = jpegWidth((const uint8_t*)fb->buf, fb->len);
  if (encodedW && encodedW != frameData[sensorFS].frameWidth) {
    LOG_WRN("Frame encodes %u wide but sensorFS says %s - check skipped", encodedW,
      frameData[sensorFS].frameSizeStr);
    return motionStatus;
  }
  uint32_t dTime = millis();
  uint32_t lux = 0;
  static uint32_t motionCnt = 0;
  static uint8_t sensorFSprev = 255; // initially invalid to force setup on first call
  static uint8_t scaling, downsize;
  static uint16_t reducer;
  static int sampleWidth = 0, sampleHeight = 0;
  // sized from the pixel cap, not from a framesize index. The old expression was SXGA
  // pixels * 3 / 8, which over allocated by 8x - that accident was the only thing making
  // frames above SXGA safe to decode. Deriving it from the cap is correct by construction
  // and frees ~432kB of PSRAM (491,520 down to 49,344). 16 byte aligned, no need to free
  static uint8_t* rgbBuf = (uint8_t*)heap_caps_aligned_calloc(16, 1, MOTION_RGB_BUF_SIZE, MALLOC_CAP_SPIRAM);
 #if INCLUDE_NEW_JPG
  static struct esp_jpeg_stream jpegHandle = {0};
  static uint8_t* jpgBuf = (uint8_t*)ps_malloc(MOTION_RGB_BUF_SIZE);
#endif

  // calculate parameters for sample size when resolution changes
  static bool prevBuffValid = false; // prevBuff holds a comparable image at this geometry
  if (sensorFS != sensorFSprev) {
    sensorFSprev = sensorFS;
    prevBuffValid = false; // whatever prevBuff holds was captured at the old geometry
    scaling = frameData[sensorFS].scaleFactor;
    reducer = frameData[sensorFS].sampleRate;
    downsize = pow(2, scaling) * reducer;
    stride = (colorDepth == RGB888_BYTES) ? GRAYSCALE_BYTES : RGB888_BYTES; // stride is inverse of colorDepth
    sampleWidth = frameData[sensorFS].frameWidth / downsize;
    sampleHeight = frameData[sensorFS].frameHeight / downsize;
    // the decoder writes straight into rgbBuf, so refuse rather than overflow it. Must
    // test the MCU padded dimensions, not the nominal ones - FHD writes 240x136 while
    // frameData says 1920x1080, and checking the nominal 240x135 misses the overrun
    size_t padW = ((frameData[sensorFS].frameWidth + MOTION_MCU_PX - 1) / MOTION_MCU_PX * MOTION_MCU_PX) / downsize;
    size_t padH = ((frameData[sensorFS].frameHeight + MOTION_MCU_PX - 1) / MOTION_MCU_PX * MOTION_MCU_PX) / downsize;
    if (padW * padH * RGB888_BYTES > MOTION_RGB_BUF_SIZE) {
      LOG_WRN("Motion bitmap %ux%u for %s needs %u bytes, buffer is %u - detection disabled, lower its scaleFactor",
        (unsigned)padW, (unsigned)padH, frameData[sensorFS].frameSizeStr,
        (unsigned)(padW * padH * RGB888_BYTES), (unsigned)MOTION_RGB_BUF_SIZE);
      sampleWidth = sampleHeight = 0;
    }
#if INCLUDE_NEW_JPG
    jpg2rgbClose(&jpegHandle);
    jpgReduce(fb->width, fb->height, downsize, &sampleWidth, &sampleHeight);
    if (!jpg2rgbOpen(&jpegHandle, sampleWidth, sampleHeight)) return motionStatus;
#endif
  }
  if (!sampleWidth || !sampleHeight) return motionStatus; // refused by the size check above
#if INCLUDE_NEW_JPG
  if (!jpg2rgb(&jpegHandle, fb->buf, fb->len, rgbBuf)) return motionStatus;
#else
  if (!jpg2rgb((uint8_t*)fb->buf, fb->len, rgbBuf, scaling)) return motionStatus;
#endif

#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 3, 0)
  if (colorDepth == GRAYSCALE_BYTES) rgbToGray(rgbBuf, sampleWidth, sampleHeight);
#endif
  LOG_VRB("JPEG to %s bitmap conversion %u bytes in %lums", colorDepth == RGB888_BYTES ? "color" : "grayscale", sampleWidth * sampleHeight * colorDepth, millis() - dTime);

  // allocate buffer space on heap, sized from the bitmap actually being compared. Done on
  // first use because the geometry is not known until the setup block above has run
  size_t bmpPixels = (size_t)sampleWidth * sampleHeight;
  size_t resizeDimLen = bmpPixels * colorDepth; // byte size of bitmap
  static size_t allocPixels = 0;
  if (motionJpeg == NULL) motionJpeg = (uint8_t*)ps_malloc(32 * 1024);
  static uint8_t* prevBuff = NULL;
  static uint8_t* changeMap = NULL;
  if (allocPixels != bmpPixels) {
    // geometry changed, which after commit 3534ba9 can only happen for at most one frame at
    // boot before settleSensor() pins the sensor. Reallocate rather than compare mismatched
    // buffers, and treat prevBuff as stale
    free(prevBuff); free(changeMap);
    prevBuff = (uint8_t*)ps_malloc(bmpPixels * RGB888_BYTES);
    changeMap = (uint8_t*)ps_malloc(bmpPixels * RGB888_BYTES);
    allocPixels = (prevBuff && changeMap) ? bmpPixels : 0;
    prevBuffValid = false;
    if (!allocPixels) {
      LOG_WRN("Failed to allocate %u byte motion buffers - check skipped", bmpPixels * RGB888_BYTES);
      return motionStatus;
    }
  }

  // compare each pixel in current frame with previous frame, at the bitmap's native size
  dTime = millis();
  int changeCount = 0;
  // set horizontal region of interest in image. The first term is a row count and the
  // second the row stride, so they take height and width respectively - bands stay
  // proportional and cover the same part of the scene as before
  uint16_t startPixel = (sampleHeight*(detectStartBand-1)/detectNumBands) * sampleWidth * colorDepth;
  uint16_t endPixel = (sampleHeight*(detectEndBand)/detectNumBands) * sampleWidth * colorDepth;
  int moveThreshold = max(1, (int)(((endPixel - startPixel) / colorDepth) * (11 - motionVal) / 100)); // number of changed pixels that constitute a movement
  for (size_t i = 0; i < resizeDimLen; i += colorDepth) {
    uint16_t currPix = 0, prevPix = 0;
    for (int j = 0; j < colorDepth; j++) {
      currPix += rgbBuf[i + j]; // compared straight from the decoder output, no copy
      prevPix += prevBuff[i + j];
    }
    currPix /= colorDepth;
    prevPix /= colorDepth;
    lux += currPix; // for calculating light level
    uint8_t pixVal = 255; // show active changed pixel as bright red color in changeMap image
    // set up display image for motion tracking debug
    if (dbgMotion) for (int j = 0; j < RGB888_BYTES; j++) changeMap[(i * stride) + j] = currPix; // grayscale
    // determine pixel change status
    if (abs((int)currPix - (int)prevPix) > detectChangeThreshold) {
      if (i > startPixel && i < endPixel) changeCount++; // number of changed pixels
      else pixVal = 80; // show inactive changed pixel as dark red color in changeMap image
      if (dbgMotion) {
        changeMap[(i * stride) + 2] = pixVal;
        for (int j = 0; j < RGB888_BYTES - 1; j++) changeMap[(i * stride) + j] = 0;
      }
    }
  }

  lightLevel = (lux*100)/(bmpPixels*255); // light value as a %
  nightTime = isNight(nightSwitch);
  memcpy(prevBuff, rgbBuf, resizeDimLen); // save image for next comparison
  prevBuffValid = true;
  LOG_VRB("Detected %u changes, threshold %u, light level %u, in %lums", changeCount, moveThreshold, lightLevel, millis() - dTime);
  // kept for tuning: dumpMotionStats reports the highest change count seen against the
  // threshold it was measured against, so sensitivity can be judged from near misses too
  // rather than only from the checks that happened to trip
  motionThreshold = moveThreshold;
  // Skip the first comparison after startup or a geometry change: prevBuff is either
  // uninitialised or holds an image at the old size, so changeCount is meaningless and
  // lands as a huge peak that makes the scene look far more active than it is. Measured
  // 5343 against a threshold of 164 on a completely static scene
  if (prevBuffValid && changeCount > motionPeakChange) motionPeakChange = changeCount;
  if (lightLevelOnly) return false; // no motion checking, only calc of light level
  if (dbgMotion) {
    // show motion detection during streaming for tuning
    if (!motionJpegLen) {
      // ready to setup next movement map for streaming
      dTime = millis();
      // build jpeg of changeMap for debug streaming
#if INCLUDE_NEW_JPG
      motionJpegLen = rgb2jpg(changeMap, sampleWidth, sampleHeight, JPEG_QUAL, jpgBuf);
      if (motionJpegLen == 0) LOG_WRN("motionDetect: encode() failed"); 
      else memcpy(motionJpeg, jpgBuf, motionJpegLen); 
#else
      uint8_t* jpg_buf = NULL;
      // changeMap is always RGB888 regardless of colorDepth, so its length is the RGB888
      // one - the old call passed the colorDepth sized length here, which understated it
      // threefold in grayscale mode
      if (!fmt2jpg(changeMap, bmpPixels * RGB888_BYTES, sampleWidth, sampleHeight, PIXFORMAT_RGB888, JPEG_QUAL, &jpg_buf, &motionJpegLen))
        LOG_WRN("motionDetect: fmt2jpg() failed"); 
      else memcpy(motionJpeg, jpg_buf, motionJpegLen); 
      free(jpg_buf);
      jpg_buf = NULL;
#endif
      if (motionJpegLen) xSemaphoreGive(motionSemaphore);
      LOG_VRB("Created changeMap JPEG %d bytes in %lums", motionJpegLen, millis() - dTime);
    }
  } else {
    // normal motion detection
    dTime = millis();
    // The night gate blocks detection outright, and lightLevel measures auto exposure
    // output rather than room brightness, so it can latch in ordinary indoor light. Say so
    // once per transition - otherwise detection silently does nothing and looks like a
    // sensitivity problem
    static bool warnedNight = false;
    if (nightTime && !warnedNight) {
      LOG_WRN("Motion detection suspended - night mode (light %u%% below lswitch %u)", lightLevel, nightSwitch);
      warnedNight = true;
    } else if (!nightTime && warnedNight) {
      LOG_INF("Motion detection resumed - daylight (light %u%%)", lightLevel);
      warnedNight = false;
    }
    if (!nightTime && changeCount > moveThreshold) {
      LOG_VRB("### Change detected");
      motionCnt++; // number of consecutive changes
      // need minimum sequence of changes to signal valid movement
      if (!motionStatus && motionCnt >= detectMotionFrames) {
        // Logged at INF rather than VRB: verbose also emits a decode and a rescale line for
        // every check several times a second, which buries exactly this. Not LOG_ALT, which
        // pops a browser alert - "Capture started by Camera" already alerts when this leads
        // to a recording, and this fires even when Save Capture is off, which is the state
        // sensitivity is usually tuned in
        LOG_INF("Motion detected: %d changed pixels vs threshold %d (motionVal %.0f, %d consecutive), light %u%%",
          changeCount, moveThreshold, motionVal, motionCnt, lightLevel);
        motionStatus = true; // motion started
        dTime = millis();
#if INCLUDE_MQTT
        if (mqtt_active && motionCnt) {
          sprintf(jsonBuff, "{\"MOTION\":\"ON\",\"TIME\":\"%s\"}", esp_log_system_timestamp());
          mqttPublish(jsonBuff);
          mqttPublishPath("motion", "on");
#if INCLUDE_HASIO
          mqttPublishPath("cmd", "still");
#endif
        }
#endif
      } 
    } else motionCnt = 0;
  
    if (motionStatus && !motionCnt) {
      // insufficient change or motion not classified
      LOG_INF("Motion ended: %d changed pixels, below threshold %d", changeCount, moveThreshold);
      motionStatus = false; // motion stopped
#if INCLUDE_MQTT
      if (mqtt_active) {
        sprintf(jsonBuff, "{\"MOTION\":\"OFF\",\"TIME\":\"%s\"}", esp_log_system_timestamp());
        mqttPublish(jsonBuff);
        mqttPublishPath("motion", "off");
      }
#endif
    } 
    if (motionStatus) LOG_VRB("*** Motion - ongoing %lu frames", motionCnt);
  }
  
  if (dbgVerbose) checkMemory();
  LOG_VRB("============================");
  // motionStatus indicates whether motion previously ongoing or not
  return nightTime ? false : motionStatus;
}

/*****************************************************************************************************/

#if INCLUDE_NEW_JPG

// Need to have installed espressif__esp_new_jpeg library

static void jpgReduce(int inWidth, int inHeight, uint8_t downsize, int* outWidth, int* outHeight) {
  // downsize then round width and height up to the nearest multiple of 8 while preserving the aspect ratio
  uint8_t roundTo8 = 8; // new width and height must be multiples of 8
  // Calculate the original aspect ratio 
  inWidth /= downsize;
  inHeight /= downsize;
  float aspectRatio = (float)(inWidth) / inHeight;

  auto roundUpToMultiple = [](int n, int m) {
    // round n up to the nearest multiple of m
    return ((n + m - 1) / m) * m;
  };

  // determine larger dimension
  int newLarger = inWidth;
  int newSmaller = inHeight;   
  if (inWidth < inHeight) {
    newLarger = inHeight;
    newSmaller = inWidth;
  }

  // Round the larger dimension up to the nearest multiple of 8.
  newLarger = roundUpToMultiple(inWidth, roundTo8);
  
  // Calculate the new smaller based on the new larger and original aspect ratio, then round up.
  newSmaller = (int)(ceil((float)newLarger / aspectRatio));
  newSmaller = roundUpToMultiple(newSmaller, roundTo8);

  // update the values to return
  *outWidth = newLarger;
  *outHeight = newSmaller;
  if (inWidth < inHeight) {
    *outWidth = newSmaller;
    *outHeight = newLarger;
  }
}

static bool jpg2rgbOpen(esp_jpeg_stream_handle_t jpegHandle, uint16_t width, uint16_t height) {
  // configure jpeg handler
  jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
  config.output_type = JPEG_PIXEL_FORMAT_RGB888;
  config.rotate = JPEG_ROTATE_0D;
  config.scale.width = width;
  config.scale.height = height;
  jpegHandle->output_type = JPEG_PIXEL_FORMAT_RGB888;

  // Create jpeg_dec handle
  jpeg_error_t ret = jpeg_dec_open(&config, &jpegHandle->jpeg_dec);
  if (ret != JPEG_ERR_OK) {
    LOG_ERR("Unable to create jpeg decoder handle: %d", ret);
    return false;
  }

  // Create io_callback handle
  jpegHandle->jpeg_io = (jpeg_dec_io_t*)calloc(1, sizeof(jpeg_dec_io_t));
  if (jpegHandle->jpeg_io == NULL) {
    LOG_ERR("Insufficient memory to create input handle");
    jpg2rgbClose(jpegHandle);
    return false;
  }

  // Create out_info handle
  jpegHandle->out_info = (jpeg_dec_header_info_t*)calloc(1, sizeof(jpeg_dec_header_info_t));
  if (jpegHandle->out_info == NULL) {
    LOG_ERR("Insufficient memory to create output handle");
    jpg2rgbClose(jpegHandle);
    return false;
  }
  return true;
}

static bool jpg2rgb(esp_jpeg_stream_handle_t jpegHandle, uint8_t* inputBuf, int inputLen, uint8_t* outputBuf) {
  // decode jpeg to rgb888
  // Set input buffer and buffer len to io_callback
  jpegHandle->jpeg_io->inbuf = inputBuf;
  jpegHandle->jpeg_io->inbuf_len = inputLen;

  // Parse jpeg header and get image for decoder
  jpeg_error_t ret = jpeg_dec_parse_header(jpegHandle->jpeg_dec, jpegHandle->jpeg_io, jpegHandle->out_info);
  if (ret != JPEG_ERR_OK) {
    LOG_ERR("Failed to parse jpeg header: %d", ret);
    return false;
  }

  // decode jpeg into outputBuf
  jpegHandle->jpeg_io->outbuf = outputBuf;
  ret = jpeg_dec_process(jpegHandle->jpeg_dec, jpegHandle->jpeg_io);
  if (ret != JPEG_ERR_OK) {
    LOG_ERR("Failed to decode jpeg: %d", ret);
    return false;
  }
  return true;
}

static bool jpg2rgbClose(esp_jpeg_stream_handle_t jpegHandle) {
   // remove old stream handles when resolution changes
  jpeg_error_t ret = jpeg_dec_close(jpegHandle->jpeg_dec);
  if (jpegHandle->jpeg_io) free(jpegHandle->jpeg_io);
  if (jpegHandle->out_info) free(jpegHandle->out_info);
  return ret == JPEG_ERR_OK;
}

static size_t rgb2jpg(uint8_t* rgb888, int width, int height, int qual, uint8_t* outputBuf) {
  // encode rgb888 to jpeg
  static bool firstCall = true;
  static jpeg_enc_handle_t jpeg_enc = NULL;
  static int bufLen = width * height * RGB888_BYTES;
  jpeg_error_t ret = JPEG_ERR_OK;

  if (firstCall) {
    firstCall = false;
    // configure encoder
    jpeg_enc_config_t jpeg_enc_cfg = DEFAULT_JPEG_ENC_CONFIG();
    jpeg_enc_cfg.width = width;
    jpeg_enc_cfg.height = height;
    jpeg_enc_cfg.src_type = JPEG_PIXEL_FORMAT_RGB888;
    jpeg_enc_cfg.subsampling = JPEG_SUBSAMPLE_420;
    jpeg_enc_cfg.quality = qual;
    jpeg_enc_cfg.rotate = JPEG_ROTATE_0D;
    jpeg_enc_cfg.task_enable = false;
    jpeg_enc_cfg.hfm_task_priority = 13;
    jpeg_enc_cfg.hfm_task_core = 1;

    // open encoder
    ret = jpeg_enc_open(&jpeg_enc_cfg, &jpeg_enc);
    if (ret != JPEG_ERR_OK) {
      LOG_ERR("Failed to open decoder: %d", ret);
      return 0;
    }
  }

  // encoding
  int jpgLen = 0;
  ret = jpeg_enc_process(jpeg_enc, rgb888, bufLen, outputBuf, bufLen, &jpgLen);
  if (ret != JPEG_ERR_OK) LOG_ERR("Failed to encode: %d", ret);

  //jpeg_enc_close(jpeg_enc); // keep open
  return (size_t)jpgLen;
}

#else

#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 3, 0)

// based on jpg2rgb888() from esp32-camera/to_bmp.c for access to rescaling

static uint8_t work[3100]; // Default size is 3.1kB for JPEG decoder

static bool jpg2rgb(const uint8_t* src, size_t src_len, uint8_t* out, uint8_t scale) {
  esp_jpeg_image_cfg_t jpeg_cfg = {
      .indata = (uint8_t *)src,
      .indata_size = src_len,
      .outbuf = out,
      .outbuf_size = UINT32_MAX, // sic @todo: this is very bold assumption, keeping this like this for now, not to break existing code
      .out_format = JPEG_IMAGE_FORMAT_RGB888,
      .out_scale = (esp_jpeg_image_scale_t)scale,
      .flags = {.swap_color_bytes = 0},
      .advanced = {
        .working_buffer = work,
        .working_buffer_size = sizeof(work)
      }
  };
  esp_jpeg_image_output_t output_img = {};
  esp_err_t res = esp_jpeg_decode(&jpeg_cfg, &output_img);
  if (res != ESP_OK) LOG_WRN("jpg2rgb failure: %s", espErrMsg(res)); 
  return (res == ESP_OK);
}

#else

// for arduino-esp32 versions 3.2.1 or earlier

/************* copied and modified from esp32-camera/to_bmp.c to access jpg_scale_t *****************/

typedef struct {
  uint16_t width;
  uint16_t height;
  uint16_t data_offset;
  const uint8_t *input;
  uint8_t *output;
} rgb_jpg_decoder;

static bool _rgb_write(void * arg, uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t *data) {
  // mpjpeg2sd: modified to generate 24 bit RGB or 8 bit grayscale
  rgb_jpg_decoder * jpeg = (rgb_jpg_decoder *)arg;
  if (!data){
    if (x == 0 && y == 0) {
      // write start
      jpeg->width = w;
      jpeg->height = h;
    } 
    return true;
  }

  size_t jw = jpeg->width*RGB888_BYTES;
  size_t t = y * jw;
  size_t b = t + (h * jw);
  size_t l = x * RGB888_BYTES;
  uint8_t *out = jpeg->output+jpeg->data_offset;
  uint8_t *o = out;
  size_t iy, ix;
  w *= RGB888_BYTES;

  for (iy=t; iy<b; iy+=jw) {
    o = out+(iy+l)/stride;
    for (ix=0; ix<w; ix+=RGB888_BYTES) {
      if (colorDepth == RGB888_BYTES) {
        o[ix] = data[ix+2];
        o[ix+1] = data[ix+1];
        o[ix+2] = data[ix];
      } else {
        // simple average for grayscale (matching original behaviour)
        o[ix / RGB888_BYTES] = (uint8_t)((data[ix + 2] + data[ix + 1] + data[ix]) / RGB888_BYTES);
      }
    }
    data+=w;
  }
  return true;
}

static unsigned int _jpg_read(void * arg, size_t index, uint8_t *buf, size_t len) {
  rgb_jpg_decoder * jpeg = (rgb_jpg_decoder *)arg;
  if (buf) memcpy(buf, jpeg->input + index, len);
  return len;
}

static bool jpg2rgb(const uint8_t* src, size_t src_len, uint8_t* out, uint8_t scale) {
  rgb_jpg_decoder jpeg;
  jpeg.width = 0;
  jpeg.height = 0;
  jpeg.input = src;
  jpeg.output = out;
  jpeg.data_offset = 0;
  esp_err_t res = esp_jpg_decode(src_len, (jpg_scale_t)scale, _jpg_read, _rgb_write, (void*)&jpeg);
  if (res != ESP_OK) LOG_WRN("jpg2rgb failure: %s", espErrMsg(res)); 
  return (res == ESP_OK);
}

#endif // ESP_ARDUINO_VERSION

#endif // INCLUDE_NEW_JPG

#else 
// dummies
bool isNight(uint8_t nightSwitch) {return false;}

#endif // AUXILIARY

