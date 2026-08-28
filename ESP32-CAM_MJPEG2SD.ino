/*
* Capture ESP32 Cam JPEG images into a AVI file and store on SD
* AVI files stored on the SD card can also be selected and streamed to a browser as MJPEG.
*
* s60sc 2020 - 2026
*/

#include "appGlobals.h"

void setup() {
  if (utilsStartup()) {
#ifndef AUXILIARY
    LOG_INF("Selected board %s", CAM_BOARD);
    prepCam();
    // Repair a recording interrupted by supply loss. After prepCam(), which is what sets
    // maxFrameBuffSize - the sanity bound the chunk walk validates lengths against, so
    // running earlier would reject every frame and discard the very file being rescued.
    // Still well before anything opens AVITEMP for write. Deliberately here rather than
    // in prepRecording(): that runs only if the web server started, and the boot after a
    // power failure is exactly when it might not
    recoverAvi();
#endif
  }

  // connect network (WiFi or Ethernet per config) and start web server
  if (startNetwork()) {
    // start rest of services
#ifndef AUXILIARY
    startSustainTasks(); 
#endif
#if INCLUDE_SMTP
    prepSMTP(); 
#endif
#if INCLUDE_FTP_HFS
    prepUpload();
#endif
#if INCLUDE_PERIPH
    prepPeripherals();
#endif
#if INCLUDE_AUDIO
    prepAudio(); 
#endif
#if INCLUDE_TGRAM
    prepTelegram();
#endif
#if INCLUDE_PERIPH
    startHeartbeat();
#endif
#ifndef AUXILIARY
    if (!prepRecording()) {
      snprintf(startupFailure, SF_LEN, STARTUP_FAIL "Insufficient memory, remove optional features");
      LOG_WRN("%s", startupFailure);
    }
#endif
    checkMemory();
  }
}

void loop() {
  // confirm not blocked in setup
  LOG_INF("=============== Total tasks: %u ===============\n", uxTaskGetNumberOfTasks() - 1);
  delay(1000);
  vTaskDelete(NULL); // free 8k ram
}
