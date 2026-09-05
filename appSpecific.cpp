// mjpeg2sd app specific functions
//
// Direct access (HTTP) URLs for NVR:
// - Video streaming: app_ip/sustain?video=1 
// - Audio streaming: app_ip/sustain?audio=1
// - Stills: app_ip/control?still=1
//
// s60sc 2022 - 2024

#include "appGlobals.h"
#include <esp_wifi.h> // negotiated PHY mode and AP info for the transport telemetry

static char variable[FILE_NAME_LEN]; 
static char value[FILE_NAME_LEN];
static char alertCaption[100];
static bool alertReady = false;
bool useUart = false; // auxiliary board support removed; kept for the INCLUDE_PERIPH heartbeat test
volatile audioAction THIS_ACTION = PASS_ACTION;
static void stopRC();

/************************ webServer callbacks *************************/

bool updateAppStatus(const char* variable, const char* value, bool fromUser) {
  // update vars from browser input
  esp_err_t res = ESP_OK; 
#ifndef AUXILIARY
  sensor_t* s = esp_camera_sensor_get();
#endif
  int intVal = atoi(value);
#if INCLUDE_PERIPH
  float fltVal = atof(value); // only consumed by voltLow, INCLUDE_PERIPH gated
#endif
  if (!strcmp(variable, "custom")) return true;
#ifndef AUXILIARY
  else if (!strcmp(variable, "stopStream")) stopSustainTask(intVal);
  else if (!strcmp(variable, "stopPlaying")) stopPlaying();
  else if (!strcmp(variable, "motionVal")) motionVal = intVal;
  else if (!strcmp(variable, "moveStartChecks")) moveStartChecks = intVal;
  else if (!strcmp(variable, "moveStopChecks")) moveStopChecks = intVal > 0 ? intVal : moveStopChecks;
  else if (!strcmp(variable, "moveStopSecs")) moveStopSecs = intVal > 0 ? intVal : moveStopSecs;
  else if (!strcmp(variable, "zoneCount")) zoneCount = (intVal >= 1 && intVal <= 16) ? intVal : zoneCount;
  else if (!strcmp(variable, "zoneMask")) zoneMask = intVal & 0xFFFF;
  else if (!strcmp(variable, "maxFrames")) maxFrames = intVal > 0 ? intVal : maxFrames;
  else if (!strcmp(variable, "detectMotionFrames")) detectMotionFrames = intVal;
  else if (!strcmp(variable, "detectNightFrames")) detectNightFrames = intVal;
  else if (!strcmp(variable, "enableMotion")) {
    // Turn on/off motion detection
    useMotion = (intVal) ? true : false;
    LOG_INF("%s motion detection by camera", useMotion ? "Enabling" : "Disabling");
    if (!useMotion && dbgMotion) {
      // Show Motion has nothing to show without a detector running
      dbgMotion = false;
      LOG_WRN("Show Motion turned off - it needs motion detection");
      wsAsyncSendJson("ustatus", "\"dbgMotion\":0");
    }
  }
  else if (!strcmp(variable, "dashCamOn")) {
    dashCamOn = intVal;
    if (dashCamOn == 0) forceRecord = false;
  }
  else if (!strcmp(variable, "streamVid")) streamVid = (bool)intVal;
  else if (!strcmp(variable, "streamAud")) streamAud = (bool)intVal;
  else if (!strcmp(variable, "lswitch")) nightSwitch = intVal;
#endif // AUXILIARY
#if INCLUDE_FTP_HFS
  else if (!strcmp(variable, "upload")) fsStartTransfer(value); 
#endif
  else if (!strcmp(variable, "delete")) {
    stopPlayback = true;
    deleteFolderOrFile(value);
  }
  else if (!strcmp(variable, "record")) doRecording = (intVal) ? true : false;   
  else if (!strcmp(variable, "forceRecord")) forceRecord = (intVal) ? true : false; 
  else if (!strcmp(variable, "dbgMotion")) {
    // Show Motion streams the detector's zone overlay, which works at any frame size now -
    // it only needs detection to be running. A refusal is said out loud and the real state
    // pushed back, so the toggle cannot lie
    if (intVal && !useMotion) {
      LOG_WRN("Show Motion needs motion detection enabled");
      dbgMotion = false;
      wsAsyncSendJson("ustatus", "\"dbgMotion\":0");
    } else {
      dbgMotion = (bool)intVal;
      LOG_INF("%s Show Motion", dbgMotion ? "Enabling" : "Disabling");
    }
  }
  // peripherals
#if INCLUDE_PERIPH
  else if (!strcmp(variable, "pirUse")) pirUse = (bool)intVal;
  else if (!strcmp(variable, "lampLevel")) {
    lampLevel = intVal;
    if (!lampType) setLamp(lampLevel); // manual
  }
  else if (!strcmp(variable, "lampType")) {
    lampType = intVal;
    lampAuto = lampNight = false;
    if (lampType == 1) lampAuto = true; // lamp activated by motion detector device
    if (!lampType) setLamp(lampLevel); 
    else setLamp(0); 
  }
  else if (!strcmp(variable, "relayPin")) relayPin = intVal;
  else if (!strcmp(variable, "relayMode")) relayMode = (bool)intVal;
  else if (!strcmp(variable, "relaySwitch")) digitalWrite(relayPin, intVal);
  else if (!strcmp(variable, "SVactive")) SVactive = (bool)intVal;
  else if (!strcmp(variable, "voltUse")) voltUse = (bool)intVal;
  else if (!strcmp(variable, "pirPin")) pirPin = intVal;
  else if (!strcmp(variable, "lampPin")) lampPin = intVal;
  else if (!strcmp(variable, "servoPanPin")) servoPanPin = intVal;
  else if (!strcmp(variable, "servoTiltPin")) servoTiltPin = intVal;
  else if (!strcmp(variable, "voltPin")) voltPin = intVal;
  else if (!strcmp(variable, "servoSteerPin")) servoSteerPin = intVal;
  else if (!strcmp(variable, "servoDelay")) servoDelay = intVal;
  else if (!strcmp(variable, "servoMinAngle")) servoMinAngle = intVal;
  else if (!strcmp(variable, "servoMaxAngle")) servoMaxAngle = intVal;
  else if (!strcmp(variable, "servoMinPulseWidth")) servoMinPulseWidth = intVal;
  else if (!strcmp(variable, "servoMaxPulseWidth")) servoMaxPulseWidth = intVal;
  else if (!strcmp(variable, "servoCenter")) servoCenter = intVal;
  else if (!strcmp(variable, "voltDivider")) voltDivider = intVal;
  else if (!strcmp(variable, "voltLow")) voltLow = fltVal;
  else if (!strcmp(variable, "voltInterval")) voltInterval = intVal;
  else if (!strcmp(variable, "buzzerUse")) buzzerUse = (bool)intVal;  
  else if (!strcmp(variable, "buzzerPin")) buzzerPin = intVal; 
  else if (!strcmp(variable, "buzzerDuration")) buzzerDuration = intVal;
  else if (!strcmp(variable, "ds18b20Pin")) ds18b20Pin = intVal;
#endif
#if INCLUDE_AUDIO
  else if (!strcmp(variable, "spkrRem")) {
    spkrRem = (bool)intVal;
    LOG_INF("Remote speaker is %s", spkrRem ? "On" : "Off");
    if (spkrRem && !micGain) LOG_WRN("Mic gain is off");
  }
  else if (!strcmp(variable, "micGain")) micGain = intVal;
  else if (!strcmp(variable, "micSckPin")) micSckPin = intVal;
  else if (!strcmp(variable, "micSWsPin")) micSWsPin = intVal;
  else if (!strcmp(variable, "micSdPin")) micSdPin = intVal;
  else if (!strcmp(variable, "mampBckIo")) mampBckIo = intVal;
  else if (!strcmp(variable, "mampSwsIo")) mampSwsIo = intVal;
  else if (!strcmp(variable, "mampSdIo")) mampSdIo = intVal;
#endif
  else if (!strcmp(variable, "wakeUse")) wakeUse = (bool)intVal;
#if INCLUDE_PERIPH
  else if (!strcmp(variable, "RCactive")) {
    RCactive = (bool)intVal;
  }
  else if (!strcmp(variable, "heartbeatRC")) heartbeatRC = intVal;
  else if (!strcmp(variable, "maxSteerAngle")) maxSteerAngle = intVal;  
  else if (!strcmp(variable, "maxDutyCycle")) maxDutyCycle = intVal;  
  else if (!strcmp(variable, "minDutyCycle")) minDutyCycle = intVal;  
  else if (!strcmp(variable, "maxTurnSpeed")) maxTurnSpeed = intVal;  
  else if (!strcmp(variable, "allowReverse")) allowReverse = (bool)intVal;   
  else if (!strcmp(variable, "autoControl")) autoControl = (bool)intVal; 
  else if (!strcmp(variable, "waitTime")) waitTime = intVal;    
  else if (!strcmp(variable, "lightsRCpin")) lightsRCpin = intVal;
  else if (!strcmp(variable, "stickUse")) stickUse = (bool)intVal; 
  else if (!strcmp(variable, "stickXpin")) stickXpin = intVal; 
  else if (!strcmp(variable, "stickYpin")) stickYpin = intVal; 
  else if (!strcmp(variable, "stickzPushPin")) stickzPushPin = intVal; 
#endif // INCLUDE_PERIPH

#ifndef AUXILIARY
  // camera settings
  // diagnostics - no config row, so nothing is persisted and CFG_VER is unaffected
  else if (!strcmp(variable, "dumpCam")) dumpCamRegs();
  else if (!strcmp(variable, "motionStats")) dumpMotionStats();
  else if (!strcmp(variable, "camPll")) setCamPll(value);
  // debug: the readout-row probe - <yFactor>[,<vts>[,<xFactor>]]. QVGA, VGA and 1280X960
  // share one 2624x1952 readout at VTS 984, which is why they share a 39.47 ceiling.
  // Reading fewer rows does NOT lift it: measured 3 Sep, 4x vertical gained nothing at
  // QVGA and stopped VGA producing frames at all. Kept as the record of that dead end -
  // see setSubSample() in mjpeg2sd.cpp and BOARD_TESTING.md 31
  else if (!strcmp(variable, "subSample")) setSubSample(value);
  else if (!strcmp(variable, "camReg")) setCamReg(value);
  // bench: several registers landed together at one frame boundary (0x3212 group write) -
  // the atomic HTS write the floor campaign should have used, see setCamRegGrp()
  else if (!strcmp(variable, "camRegGrp")) setCamRegGrp(value);
  else if (!strcmp(variable, "camRegRd")) getCamReg(value);
  // per-board provisioning, stored in NVS not the config file - see setExtDVDD()
  else if (!strcmp(variable, "extDVDD")) setExtDVDD(intVal);
  // bench: pulse the OTHER board's reset line (PEER_RESET_PIN), see peerReset() - no config row
  else if (!strcmp(variable, "peerReset")) peerReset(intVal);
  // the night panel's focus slider: hold the lens at a VCM code, or give it back to the AF
  // program. The AF cannot focus in low light - it parks wherever it gives up - so a long
  // exposure needs the lens placed by hand and held (BOARD_TESTING §37)
  else if (!strcmp(variable, "afManual")) camFocusManual(intVal);
  else if (!strcmp(variable, "afAuto")) camFocusAuto();
  // manual white balance gains "<r>,<g>,<b>", 1024 = 1.0x. The night panel's answer to the green
  // cast a long exposure leaves: raise R and B against G. NOT the same as awb, which stops the ISP
  // applying any gains at all and goes further green
  else if (!strcmp(variable, "awbGains")) {
    int r = atoi(value), g = 1024, b = 1024;
    const char* p1 = strchr(value, ',');
    if (p1) {
      g = atoi(p1 + 1);
      const char* p2 = strchr(p1 + 1, ',');
      if (p2) b = atoi(p2 + 1);
    }
    camAwbGains(r, g, b);
  }
  else if (!strcmp(variable, "awbGainsAuto")) camAwbAuto();
  // debug: exercise the supply sag path without a failing supply. Sets the same flag the
  // brownout ISR sets, so the whole Stage 1 sequence runs for real - close, park, re-arm
  else if (!strcmp(variable, "sagTest")) supplySagging = (bool)intVal;
  else if (!strcmp(variable, "battUse")) battUse = intVal;
  else if (!strcmp(variable, "battPin")) battPin = intVal;
  else if (!strcmp(variable, "battScale")) battScale = intVal;
  else if (!strcmp(variable, "battWarnMv")) battWarnMv = intVal;
  else if (!strcmp(variable, "idleFps")) idleFps = constrain(intVal, 0, 30);
  else if (!strcmp(variable, "idleSecs")) idleSecs = max(1, intVal);
  else if (!strcmp(variable, "wifiSleep")) {
    wifiSleep = (bool)intVal;
    applyPowerConfig();
  }
  else if (!strcmp(variable, "cpuFreqMhz")) {
    if (intVal == 80 || intVal == 160 || intVal == 240) {
      cpuFreqMhz = (uint8_t)intVal;
      applyPowerConfig();
    } else LOG_WRN("cpuFreqMhz must be 80, 160 or 240, ignoring %d", intVal);
  }
  else if (!strcmp(variable, "wifiTxDbm")) {
    wifiTxDbm = (uint8_t)constrain(intVal, 2, 20);
    applyPowerConfig();
  }
  // debug: dirty reboot with no cleanup - the same esp_restart_noos() the terminal
  // brownout stage uses. Leaves whatever the filesystem had actually flushed, which is
  // how a recording looks after a power cut, so boot recovery can be tested repeatably
  // without pulling the plug. Not a full substitute: a real cut can also tear the SD
  // block that is in flight, which this cannot reproduce
  else if (!strcmp(variable, "crashTest")) {
    LOG_ALT("Debug: dirty reboot requested, no cleanup - emulating supply loss");
    delay(50);
    debugDirtyReboot();
  }
  // debug: starve the task watchdog. INTENDED to make it fire so the RTC ring gets the
  // starved task name from esp_task_wdt_isr_user_handler. DANGER: on this board it does not
  // do that. Three runs on 3 Sep 2026 all ended in a hard wedge needing a physical reset -
  // no watchdog reboot, and not even the 300s bail-out below ever reached the card. Treat
  // this as a reliable way to hang the board, not as a self-test. See BOARD_TESTING.md 29
  else if (!strcmp(variable, "wdtTest")) {
    LOG_ALT("Debug: starving the task watchdog - WARNING, all 3 runs so far wedged the board instead of rebooting it");
    delay(50);
    starveWatchDog();
  }
  // debug: arm the comparator at a given level for the false trip soak. Never terminal
  // from here - a web request must not be able to arm the killswitch.
  // Level 1 (3.30V) is at the nominal rail, so arming it asserts a PERMANENT brownout:
  // the sag response then escalates to the terminal config, whose flash power-down stops
  // the chip executing entirely - a hard hang needing a physical power cycle, not a reset.
  // Learned the hard way on 28 Aug. Use bodDump to inspect the hardware instead
  else if (!strcmp(variable, "bodLevel")) {
    // Never arm a threshold at or above the live rail. Crossing it does not deliver a
    // usable interrupt - it wedges the chip outright, before any of this code runs, and
    // only a physical power cycle recovers it. Proven twice on 28 Aug, the second time
    // with the sag response in log-only mode, which rules out the escalation as the
    // cause. Level 1 is 3.30V against a 3.3V rail, so it is permanently unsafe; use
    // bodDump to inspect the comparator instead of provoking it
    if (intVal < 2 || intVal > 7) LOG_WRN("bodLevel %d refused - only 2-7 may be armed (1 is the nominal rail and hangs the board)", intVal);
    else armBrownout((uint8_t)intVal, false);
  }
  else if (!strcmp(variable, "bodDump")) brownoutDump();
  else if (!strcmp(variable, "sdBusClk")) sdBusClk(value); // 0 reports, 2-16 sets the host divider (transient)
  // the persisted counterpart: the saved row reapplies the divider on every boot via the
  // config load, which runs after the SD is mounted. Default 4 = the stock 40MHz - safe for
  // fresh installs; 3 = 53.33MHz, out of SD HS spec and only for individually qualified cards
  else if (!strcmp(variable, "sdBusDiv")) sdBusClk(value);
  else if (!strcmp(variable, "avgZones")) avgZones(value); // dump the AEC 4x4 zone grid + gates
  else if (!strcmp(variable, "xclkStat")) xclkStat(value); // measure XCLK and VSYNC off the pins
  else if (!strcmp(variable, "lencFhd")) lencFhd(value); // A/B the LENC 4/3 rescale for the FHD crop
  else if (!strcmp(variable, "xclkMhz")) xclkMhz = intVal;
  // mains banding filter: 0 off (the default), 50 or 60 manual - applyBanding() in mjpeg2sd.cpp
  else if (!strcmp(variable, "banding")) setBanding(intVal);
  // takes effect on the next size change (or boot), when applySensorTuning() next runs - the
  // sensor cannot be retimed from the web task while the capture task may hold a frame
  else if (!strcmp(variable, "tunedFps")) {
    tunedFps = intVal;
    if (playbackHandle != NULL) retimePending = true; // capture task re-times on next frame
  }
  else if (!strcmp(variable, "framesize")) {
    // Compare pixels, not enum index. The custom sizes sit past framesize_t so they are
    // numerically above maxFS while being far smaller than it - an index test would reject
    // 1280x960 (1.23MP) against a QSXGA (4.9MP) buffer it fits inside several times over
    if (intVal >= (int)(sizeof(frameData) / sizeof(frameData[0]))) LOG_WRN("Frame size index %d out of range", intVal);
    else if ((uint32_t)frameData[intVal].frameWidth * frameData[intVal].frameHeight
           > (uint32_t)frameData[maxFS].frameWidth * frameData[maxFS].frameHeight && fromUser)
      LOG_WRN("Frame size %s too large for %s PSRAM ", frameData[intVal].frameSizeStr, fmtSize(ESP.getPsramSize()));
    else if (isCapturing && fromUser) {
      // mid-recording: defer so the AVI keeps one geometry - settleSensor() applies this
      // when the clip closes. The config vector still takes the new value above, so the UI
      // shows the user's choice while the sensor honours the recording in progress
      pendingFS = intVal;
      LOG_INF("Frame size %s takes effect when the recording stops", frameData[intVal].frameSizeStr);
    }
    else {
      // A long exposure session owns the sensor's timing. Picking a size here ends it, or the
      // sensor stays on a multi-second frame while this panel reports the size just chosen -
      // which starves the stream and walks the no-frame rescue (5 Sep 2026, BOARD_TESTING §38.12).
      // restoreSize false: the user's choice below wins, not the size the session remembered
      if (nightFrameMs > 0 && fromUser && intVal != fsizePtr) nightExit(false, true); // their size, the session's rate back
      fsizePtr = intVal;
      if (!videoSizeAllowed(fsizePtr) && fromUser) LOG_WRN("%s is above the %s video cap - stills only, no AVI recording",
        frameData[fsizePtr].frameSizeStr, frameData[maxVideoFS].frameSizeStr);
      // Deliberately does NOT touch the sensor. settleSensor() in the capture task owns
      // sensorFS and applies this on the next frame - setting the hardware here behind its
      // back leaves sensorFS stale (zoneMotion also keys its reference off sensorFS)
      if (playbackHandle != NULL) {
        // the user's rate survives a size switch, clamped to the new size's ceiling -
        // the updateFPS response re-ranges the slider and carries this value back
        if (captureFPS > fpsCeiling((framesize_t)fsizePtr)) captureFPS = fpsCeiling((framesize_t)fsizePtr);
        char fpsStr[8];
        snprintf(fpsStr, sizeof(fpsStr), "%u", captureFPS);
        updateConfigVect("fps", fpsStr);
      }
    }
  }
  else if (!strcmp(variable, "fps")) {
    // An over-ceiling user request has already been clamped by updateStatus() in prefs.cpp,
    // before this handler AND the generic config-vector store see it. Clamping here was
    // tried first (2 Sep 2026): the store at the end of updateStatus() then overwrote the
    // clamped value with the original request, so the sensor ran the ceiling while the UI
    // and /status still said 99
    if (isCapturing && fromUser) {
      // mid-recording: defer, so the clip keeps the timing its AVI header promises
      pendingFPS = intVal;
      LOG_INF("FPS %d takes effect when the recording stops", intVal);
    } else {
      // same reasoning as the framesize handler: asking for a rate ends a long exposure session,
      // whose whole point is that the sensor is NOT running at the requested rate
      if (nightFrameMs > 0 && fromUser) nightExit(true, false); // their rate, the session's size back
      FPS = intVal;
      captureFPS = intVal; // the user's rate for the capture size, preserved across switches
      if (playbackHandle != NULL) setFPS(FPS);
      // with tuned timing the fps choice drives the sensor's own timing too (VTS on most
      // sizes, the PLL clock on VGA/QVGA); the capture task owns sensor writes, so only
      // the flag is raised here
      if (tunedFps) retimePending = true;
    }
  }
  else if (s) {
    if (!strcmp(variable, "quality")) { res = s->set_quality(s, intVal); govRebaseQuality(intVal); }
    else if (!strcmp(variable, "contrast")) res = s->set_contrast(s, intVal);
    else if (!strcmp(variable, "brightness")) res = s->set_brightness(s, intVal);
    else if (!strcmp(variable, "saturation")) res = s->set_saturation(s, intVal);
    else if (!strcmp(variable, "denoise")) res = s->set_denoise(s, intVal);    
    else if (!strcmp(variable, "sharpness")) res = s->set_sharpness(s, intVal);    
    // The gainceiling_t enum is a lie on this part. Disassembling the precompiled driver shows
    // OV5640's set_gainceiling splitting the raw argument into 0x3A18[1:0]/0x3A19[7:0], which is
    // the 10 bit REAL GAIN field - value/16 is the multiplier - not an enum ordinal. So the
    // OV2640 scale of 0..6 meaning 2x..128x does not apply here, and the web UI already knows
    // that: the OV5640 branch sets this slider to 0..511. Passing the enum ordinal 6 would ask
    // for 0.38x. The shipped default used to be 0, which overwrote the 15.5x the driver's own
    // reset table programs and left the ceiling at 0.00x - confirmed by reading it back
    else if (!strcmp(variable, "gainceiling")) res = s->set_gainceiling(s, (gainceiling_t)intVal);
    else if (!strcmp(variable, "colorbar")) res = s->set_colorbar(s, intVal);
    else if (!strcmp(variable, "awb")) res = s->set_whitebal(s, intVal);
    else if (!strcmp(variable, "agc")) res = s->set_gain_ctrl(s, intVal);
    else if (!strcmp(variable, "aec")) res = s->set_exposure_ctrl(s, intVal);
    else if (!strcmp(variable, "hmirror")) res = s->set_hmirror(s, intVal);
    else if (!strcmp(variable, "vflip")) res = s->set_vflip(s, intVal);
    else if (!strcmp(variable, "awb_gain")) res = s->set_awb_gain(s, intVal);
    else if (!strcmp(variable, "agc_gain")) res = s->set_agc_gain(s, intVal);
    else if (!strcmp(variable, "aec_value")) res = s->set_aec_value(s, intVal);
    // aec2 is night mode, 0x3A00[2]. Datasheet 4.6.1.3: it buys exposure in the dark by
    // slowing the frame rate down, extending the frame by up to the ceiling in
    // {0x3A02,0x3A03}. That ceiling is 984 lines, not the 15744 recorded here before - 15744 is
    // the datasheet power-on value, and the driver's own reset table overwrites it with 0x03D8
    // during esp_camera_init, so the part never runs at the power-on figure. Confirmed by
    // reading the register back off both boards.
    // The extension appears as the AEC extra lines dumpCamRegs() reports, so it is visible.
    // Defaulted OFF, and it spent one day (27 Aug 2026) defaulted on - that was a wrong fix
    // for the low-fps gain-parking, which is really the AEC engine handed an exposure ceiling
    // beyond its 1964-row range (see applyAecLimits, which now clamps it). Night mode papers
    // over that case by abandoning band quantisation, so the AEC picks exposures that are no
    // multiple of the mains half cycle and mains-lit scenes get rolling flicker bands - the
    // "vertical striping" that took a firmware A/B against 6624230 to pin down. Leave it off
    // unless exposure beyond 1964 rows matters more than flicker-free light
    else if (!strcmp(variable, "aec2")) res = s->set_aec2(s, intVal);
    // "dcw" is the driver's name and the web page's label, and both are wrong: on the OV5640 this
    // writes 0x5183[7], which selects SIMPLE (0) against ADVANCED (1) AWB - nothing downsizes.
    // Default 0 since 5 Sep 2026 on the user's decision: simple measured the more neutral of the
    // two on the star chart at QSXGA, R/G 0.965 and B/G 0.918 against advanced's 0.882 / 0.882
    // (BOARD_TESTING §38.5). Still owed: the same comparison in a dark room
    else if (!strcmp(variable, "dcw")) res = s->set_dcw(s, intVal);
    else if (!strcmp(variable, "bpc")) res = s->set_bpc(s, intVal);
    else if (!strcmp(variable, "wpc")) res = s->set_wpc(s, intVal);
    else if (!strcmp(variable, "raw_gma")) res = s->set_raw_gma(s, intVal);
    else if (!strcmp(variable, "lenc")) res = s->set_lenc(s, intVal);
    else if (!strcmp(variable, "special_effect")) res = s->set_special_effect(s, intVal);
    else if (!strcmp(variable, "wb_mode")) res = s->set_wb_mode(s, intVal);
    else if (!strcmp(variable, "ae_level")) res = s->set_ae_level(s, intVal);
    else return false;
  }
#endif // AUXILIARY
  else return false;
  if (res != ESP_OK && fromUser) LOG_WRN("Value %d for setting %s not supported for camera type %s", intVal, variable, camModel);
  return true;
}

static bool extractKeyVal(const char* wsMsg) {
  // extract key 
  strncpy(variable, wsMsg, FILE_NAME_LEN - 1); 
  char* endPtr = strchr(variable, '=');
  if (endPtr != NULL) {
    *endPtr = 0; // split variable into 2 strings, first is key name
    strcpy(value, endPtr + 1); // value is now second part of string
    return true;
  } else LOG_ERR("Invalid query string: %s", wsMsg);
  return false;
} 

esp_err_t appSpecificWebHandler(httpd_req_t *req, const char* variable, const char* value) {
  // update handling requiring response specific to mjpeg2sd
  if (!strcmp(variable, "sfile")) {
    // get folders / files on SD, save received filename if has required extension
    strcpy(inFileName, value);
    if (!forceRecord) doPlayback = listDir(inFileName, jsonBuff, JSON_BUFF_LEN, AVI_EXT); // browser control
    else strcpy(jsonBuff, "{}");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, jsonBuff);
  } 
  else if (!strcmp(variable, "zoneStats")) {
    // detector snapshot as JSON for threshold tuning - pollable, unlike the logged avgZones
    zoneStatsJson(jsonBuff, JSON_BUFF_LEN);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, jsonBuff);
  }
  else if (!strcmp(variable, "updateFPS")) {
    // Report the capture resolution's fps range and headroom. Must not go through
    // setFPSlookup(), which calls setFPS() and so retunes the live frame timer on a UI poll.
    // fpsCeil: the top of the fps slider - the tuned ceiling, or the driver-clock default
    // when the size is untuned. aecMax: the manual exposure ceiling in lines, VTS-4 read
    // from the LIVE sensor so the slider tracks whatever timing is actually in force.
    // budgetKBs: the measured storage ceiling for the bus clock currently set - 4458
    // measured at 53.3MHz, 3643 at the stock 40 (sdBudgetKBs, 2 Sep 2026)
    int aecMax = 1200; // the historical slider cap, kept when the sensor cannot be read
    sensor_t* sen = esp_camera_sensor_get();
    if (sen != NULL && sen->get_reg != NULL) {
      int hi = sen->get_reg(sen, 0x380E, 0xFF), lo = sen->get_reg(sen, 0x380F, 0xFF);
      if (hi >= 0 && lo >= 0) aecMax = ((hi << 8) | lo) - 4; // datasheet 4.6.2
      if (aecMax > 1964) aecMax = 1964; // the AEC engine cap (applyAecLimits) - lines above it are unreachable
    }
    // captureFPS, not the row default: on a size change the framesize handler has already
    // clamped captureFPS to the new ceiling before the UI asks, and on a plain page load
    // the device's actual rate is the truth - a default would overwrite it in the browser.
    // frameKB / govBoost: live SD-governor telemetry, both 0 outside a recording, so the
    // badge falls back to its static estimate exactly when there is nothing better
    sprintf(jsonBuff, "{\"fps\":\"%u\",\"fpsCeil\":\"%u\",\"aecMax\":\"%d\",\"budgetKBs\":\"%u\",\"frameKB\":\"%u\",\"govBoost\":\"%u\",\"frameCapKB\":\"%u\"}",
      captureFPS, fpsCeiling((framesize_t)fsizePtr), aecMax, sdBudgetKBs(), sdGovFrameKB, sdGovBoost, frameWindowKB(fsizePtr));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, jsonBuff);
  }
  else if (!strcmp(variable, "nightExp")) {
    // Night (long exposure) mode: "<framesize index>,<ms>", or ms 0 to leave. Only the two
    // full-resolution sizes are offered - a binned size's line cost flips above HTS 2277, so
    // 1280X960 tops out near 0.45s and is not what this mode is for (BOARD_TESTING §37)
    int idx = atoi(value);
    const char* comma = strchr(value, ',');
    int ms = comma ? atoi(comma + 1) : 0;
    if (ms <= 0) {
      nightExit();
      // the retime happens in the capture task, so wait for it or the reply reports the timing
      // the sensor is about to leave rather than the one it lands on
      uint32_t startTime = millis();
      while ((sensorFS != (framesize_t)fsizePtr || retimePending) && millis() - startTime < 8000) delay(100);
      delay(200);
    }
    else if (idx < 0 || idx >= (int)(sizeof(frameData) / sizeof(frameData[0]))
             || (strcmp(frameData[idx].frameSizeStr, "QSXGA") && strcmp(frameData[idx].frameSizeStr, "FHDNARROW"))) {
      LOG_WRN("Night mode: %s is not a long-exposure size (QSXGA or FHDNARROW only)",
        (idx >= 0 && idx < (int)(sizeof(frameData) / sizeof(frameData[0]))) ? frameData[idx].frameSizeStr : "index");
    }
    else if (nightEnter(idx, ms)) {
      // the capture task owns sensor writes, so wait for it to apply the retime before
      // reporting - bounded, and reporting whatever is there if it does not land
      uint32_t startTime = millis();
      while ((sensorFS != (framesize_t)fsizePtr || retimePending) && millis() - startTime < 8000) delay(100);
      delay(200);
    }
    nightStatus(jsonBuff, JSON_BUFF_LEN);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, jsonBuff);
  }
  else if (!strcmp(variable, "nightStatus")) {
    nightStatus(jsonBuff, JSON_BUFF_LEN);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, jsonBuff);
  }
  else if (!strcmp(variable, "still")) {
    // send single jpeg to browser
    uint32_t startTime = millis();
    doKeepFrame = true;
    // the wait follows the frame: at a 3s frame the stock 1.2s misses most requests
    while (doKeepFrame && millis() - startTime < stillWaitMs()) delay(100);
    if (!doKeepFrame && alertBufferSize) {
      httpd_resp_set_type(req, "image/jpeg");
      httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
      httpd_resp_send(req, (const char*)alertBuffer, alertBufferSize);
      uint32_t jpegTime = millis() - startTime;
      // report the size actually delivered - sensorFS is the hardware truth, and naming
      // fsizePtr would confidently report a size that was never captured on any mismatch
      LOG_INF("%s JPEG: %uB in %lums", frameData[sensorFS].frameSizeStr, alertBufferSize, jpegTime);
      alertBufferSize = 0;
    } else LOG_WRN("Failed to get still");
  } 
  else if (!strcmp(variable, "formatSD")) {
    if (formatSDcard()) doRestart("user requested format of SD card");
  } 
  else return ESP_FAIL;
  return ESP_OK;
}

static bool setPeripheral(char cmd, int controlVal, bool fromUart) {
  bool res = true;
  switch (cmd) {
#if INCLUDE_PERIPH
    case 'L':
      // lights
      setLightsRC((bool)controlVal);
    break;
    case 'P':
      // camera pan servo
      setCamPan(controlVal);
    break;
    case 'T':
      // camera tilt servo
      setCamTilt(controlVal);
    break;
#endif
    case 'K':
      // cam browser conn closed
      stopRC();
    break;
    default:
      res = false;
    break;
  }
  return res;
}

void appSpecificWsHandler(const char* wsMsg) {
  // message from web socket
  int wsLen = strlen(wsMsg) - 1;
  char cmd = (char)wsMsg[0];
  int controlVal = atoi(wsMsg + 1); // skip first char
  // the auxiliary-over-UART branch was removed with the rest of the auxiliary feature -
  // useUart is no longer settable, so this always took the direct path
  if (!setPeripheral(cmd, controlVal, false)) {
    switch (cmd) {
      case 'X':
#if INCLUDE_AUDIO
        // stop remote mic stream
        stopAudio = true;
#endif
      break;
      case 'C':
        // control request
        if (extractKeyVal(wsMsg + 1)) updateStatus(variable, value);
      break;
      case 'S':
        // status request
        buildJsonString(wsLen); // required config number
        LOG_SEND("%s\n", jsonBuff);
      break;
      case 'U':
        // update or control request
        memcpy(jsonBuff, wsMsg + 1, wsLen); // remove 'U'
        parseJson(wsLen);
      break;
      case 'H':
        // browser keepalive heartbeat
        heartBeatDone = true;
      break;
      case 'K':
        // kill websocket connection
        killSocket();
      break;
      default:
        LOG_WRN("unknown command %s", wsMsg);
      break;
    }
  }
}

void appSpecificWsBinHandler(uint8_t* wsMsg, size_t wsMsgLen) {
  // no binary websocket input is used - the browser microphone path was removed
  // as this board has no I2S amplifier to play it out of
}

char* buildAppJsonString(bool filter) {
  // build app specific part of json string
  char* p = jsonBuff + 1;
  p += sprintf(p, "\"llevel\":%u,", lightLevel);
  p += sprintf(p, "\"night\":%s,", nightTime ? "\"Yes\"" : "\"No\"");
  float aTemp = readTemperature(true);
  if (aTemp > -127.0) p += sprintf(p, "\"atemp\":\"%0.1f\",", aTemp);
  else p += sprintf(p, "\"atemp\":\"n/a\",");
  p += sprintf(p, "\"camModel\":\"%s\",", camModel);
#if INCLUDE_PERIPH
  p += sprintf(p, "\"SVactive\":\"%d\",", SVactive);
#endif
  p += sprintf(p, "\"sustainId\":\"%u\",", sustainId);
  // Supply telemetry. With no battery divider the comparator band is the only rail
  // reading there is, and on a battery run over wifi it is the only way to watch the
  // descent - so it goes in the status poll, not just the log. band 0 = healthy
  // Advertise which optional modules are compiled OUT, so the web UI can hide their
  // controls instead of offering sliders that fail with "required cpp file not included"
  p += sprintf(p, "\"builtOut\":\"%s%s%s%s%s\",",
    INCLUDE_FTP_HFS ? "" : "fs,", INCLUDE_SMTP ? "" : "smtp,", INCLUDE_TGRAM ? "" : "tgram,",
    INCLUDE_MQTT ? "" : "mqtt,", INCLUDE_PERIPH ? "" : "periph,");
  p += sprintf(p, "\"battMv\":\"%u\",", battMv);
  // display-ready battery voltage for the web UI footer, following the atemp idiom of
  // formatting on the firmware side; n/a until the battery monitor is enabled and reading
  if (battUse && battMv > 0) p += sprintf(p, "\"battV\":\"%u.%02uV\",", battMv / 1000, (battMv % 1000) / 10);
  else p += sprintf(p, "\"battV\":\"n/a\",");
  // power-control ACTUALS, read back from the hardware/driver rather than echoing the
  // config - the bench A/B trusts these, not what we asked for
  p += sprintf(p, "\"cpuFreqNow\":\"%u\",", (uint8_t)getCpuFrequencyMhz());
  p += sprintf(p, "\"wifiSleepNow\":\"%d\",", WiFi.getSleep() ? 1 : 0);
  p += sprintf(p, "\"txPwrNow\":\"%d.%d\",", WiFi.getTxPower() / 4, (WiFi.getTxPower() % 4) * 25 / 10);
  p += sprintf(p, "\"sagBand\":\"%u\",", brownoutProbeBand());
  p += sprintf(p, "\"sagTrips\":\"%lu\",", sagTripCount);
  p += sprintf(p, "\"sagParked\":\"%u\",", supplyParked ? 1 : 0);
  p += sprintf(p, "\"bodLevel\":\"%u\",", brownoutArmedLevel());
  // Extend info
#ifndef AUXILIARY
  // the sensor changes size on its own now, and nothing else in the UI reports that
  p += sprintf(p, "\"sensorState\":\"%s\",", sensorStateStr());
  // UI_REVIEW C2: what the automatics actually chose, for the page's read-only line. camLiveLine
  // caches its register reads, so this costs the poll nothing beyond the string
  p += sprintf(p, "\"camLive\":\"%s\",", camLiveLine());
  // UI_REVIEW C4: Show Motion was write-only - the firmware pushed refusals over the websocket but
  // never reported the state, so a page reload drew the checkbox from its HTML default and the
  // bench could not read it back at all
  p += sprintf(p, "\"dbgMotion\":\"%d\",", dbgMotion ? 1 : 0);
  uint8_t cardType = 99; // not MMC
  if ((fs::SDMMCFS*)&STORAGE == &SD_MMC) cardType = SD_MMC.cardType();
  if (cardType == CARD_NONE) p += sprintf(p, "\"card\":\"%s\",", "NO card");
  else {
    if (!filter) {
      if (cardType == CARD_MMC) p += sprintf(p, "\"card\":\"%s\",", "MMC");
      else if (cardType == CARD_SD) p += sprintf(p, "\"card\":\"%s\",", "SDSC");
      else if (cardType == CARD_SDHC) p += sprintf(p, "\"card\":\"%s\",", "SDHC");
      else if (cardType == 99) p += sprintf(p, "\"card\":\"%s\",", "LittleFS");
    }
    if ((fs::SDMMCFS*)&STORAGE == &SD_MMC) p += sprintf(p, "\"card_size\":\"%s\",", fmtSize(SD_MMC.cardSize()));
    p += sprintf(p, "\"used_bytes\":\"%s\",", fmtSize(STORAGE.usedBytes()));
    p += sprintf(p, "\"free_bytes\":\"%s\",", fmtSize(STORAGE.totalBytes() - STORAGE.usedBytes()));
    p += sprintf(p, "\"total_bytes\":\"%s\",", fmtSize(STORAGE.totalBytes()));
  }
  p += sprintf(p, "\"free_psram\":\"%s\",", fmtSize(ESP.getFreePsram()));
  // Memory by capability, not just the aggregate. lwip pbufs PREFER psram on this core
  // (mem_clib_malloc is heap_caps_malloc_prefer with SPIRAM first), so a transport buffer
  // change shows up in psram, while a fallback to internal shows up in the internal
  // figures. The MINIMUM-ever values are the point: a burst that briefly squeezes memory
  // is invisible to instantaneous polling, and the failure it causes (camera frame buffer
  // allocation refused) only appears on the NEXT boot
  p += sprintf(p, "\"int_free\":\"%u\",", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  p += sprintf(p, "\"int_block\":\"%u\",", heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  p += sprintf(p, "\"int_min\":\"%u\",", heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
  p += sprintf(p, "\"psram_min\":\"%u\",", heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
  // Transport ceiling of the core this image was BUILT against. Reads the macros, never
  // redefines them - a sketch-side redefinition would not change the prebuilt liblwip.a
  // and would only produce an ABI mismatch. Caveat: this proves the app's sdkconfig.h,
  // not the library, so it is evidence about which SDK tree the build used
  p += sprintf(p, "\"lwipSndBuf\":\"%d\",", CONFIG_LWIP_TCP_SND_BUF_DEFAULT);
  p += sprintf(p, "\"lwipWnd\":\"%d\",", CONFIG_LWIP_TCP_WND_DEFAULT);
  p += sprintf(p, "\"lwipMss\":\"%d\",", CONFIG_LWIP_TCP_MSS);
  p += sprintf(p, "\"idfVer\":\"%s\",", esp_get_idf_version());
  // Negotiated PHY, not the configured one. Without this we are blind to whether the
  // link came up as 11b/11g/HT20/HT40 - and an 11g association would cap the PHY at
  // 54Mbit/s and explain a low ceiling entirely. phyMode is what we actually got;
  // apBw is what the AP advertises (1 = 20MHz, 2 = 40MHz), so the pair says whether
  // a wider channel is even on offer before any effort is spent trying to use one
  wifi_phy_mode_t phyMode;
  if (esp_wifi_sta_get_negotiated_phymode(&phyMode) == ESP_OK) {
    const char* phyStr = phyMode == WIFI_PHY_MODE_LR ? "LR" : phyMode == WIFI_PHY_MODE_11B ? "11B"
      : phyMode == WIFI_PHY_MODE_11G ? "11G" : phyMode == WIFI_PHY_MODE_HT20 ? "HT20"
      : phyMode == WIFI_PHY_MODE_HT40 ? "HT40" : "other";
    p += sprintf(p, "\"phyMode\":\"%s\",", phyStr);
  } else p += sprintf(p, "\"phyMode\":\"n/a\",");
  wifi_ap_record_t apInfo;
  if (esp_wifi_sta_get_ap_info(&apInfo) == ESP_OK) {
    p += sprintf(p, "\"apChan\":\"%u\",", apInfo.primary);
    p += sprintf(p, "\"apBw\":\"%d\",", (int)apInfo.bandwidth);
    p += sprintf(p, "\"ap11n\":\"%u\",", apInfo.phy_11n ? 1 : 0);
    p += sprintf(p, "\"apAuth\":\"%d\",", (int)apInfo.authmode);
    p += sprintf(p, "\"apBssid\":\"%02X%02X%02X%02X%02X%02X\",", apInfo.bssid[0],
      apInfo.bssid[1], apInfo.bssid[2], apInfo.bssid[3], apInfo.bssid[4], apInfo.bssid[5]);
  }
  // Live transport rate for slot 0, pollable MID-stream rather than only at the end.
  // sendBusy is the share of wall clock actually inside the send calls
  if (streamSendUs[0] > 0) {
    p += sprintf(p, "\"sendKBs\":\"%llu\",", streamSentBytes[0] * 1000000ULL / streamSendUs[0] / 1024ULL);
    p += sprintf(p, "\"sendUs\":\"%llu\",", streamSendUs[0]);
  }
  p += sprintf(p, "\"streamSkipped\":\"%lu\",", streamSkipped[0]);
#endif
#if INCLUDE_FTP_HFS
  p += sprintf(p, "\"progressBar\":%d,", percentLoaded);
  if (percentLoaded == 100) percentLoaded = 0;
#endif
  //p += sprintf(p, "\"vcc\":\"%i V\",", ESP.getVcc() / 1023.0F; );
  return p;
}

/******************************************************************/

void externalAlert(const char* subject, const char* message) {
  // alert any configured external servers
#if INCLUDE_TGRAM
  if (tgramUse) tgramAlert(subject, message);
#endif
#if INCLUDE_SMTP
  if (smtpUse) emailAlert(subject, message);
#endif
}

void displayAudioLed(int16_t audioSample) {}

void setupAudioLed() {}

#if !INCLUDE_PERIPH
float readTemperature(bool isCelsius, bool onlyDS18) {
  return readInternalTemp();
}
#endif

void setInputPeripheral(uint8_t cmd, uint32_t controlVal) {
  // set data on client for data received from auxiliary input peripheral
  // not used
  //if ((char)cmd == 'I') memcpy(&pirVal, &controlVal, sizeof(pirVal));  // set PIR status
}

bool appDataFiles() {
  // callback from setupAssist.cpp, for any app specific files 
  return true;
}

void currentStackUsage() {
  checkStackUse(captureHandle, 0);
#if INCLUDE_SMTP
  checkStackUse(emailHandle, 2);
#endif
#if INCLUDE_FTP_HFS
  checkStackUse(fsHandle, 3);
#endif
  checkStackUse(logHandle, 4);
#if INCLUDE_AUDIO
  checkStackUse(audioHandle, 5);
#endif
#if INCLUDE_MQTT
  checkStackUse(mqttTaskHandle, 6);
#endif
  // 7: pingtask
  checkStackUse(playbackHandle, 8);
#if INCLUDE_PERIPH
 #if INCLUDE_DS18B20
  checkStackUse(DS18B20handle, 1);
 #endif
  checkStackUse(servoHandle, 9);
  checkStackUse(stickHandle, 10);
  checkStackUse(heartBeatHandle, 14);
  checkStackUse(battHandle, 15);
#endif
#if INCLUDE_TGRAM
  checkStackUse(telegramHandle, 11);
#endif
  // 16: http webserver
  for (int i=0; i < numStreams; i++) checkStackUse(sustainHandle[i], 17 + i);
}

static void stopRC() {
  // stop RC movement if connection lost
#if INCLUDE_PERIPH
  setLightsRC(false);
#endif
}

#if INCLUDE_PERIPH
static void heartBeatTask (void *pvParameter) {
  // check on aux that ws and / or uart connection available
  while (true) {
    delay((heartbeatRC + 1) * 1000); // 1 sec more than browser heartbeat rate
    if (!heartBeatDone) stopRC(); // stop RC as no heartbeat received
    heartBeatDone = false;
  }
}
 
void startHeartbeat() {
  // start heartbeat to check websocket and / or uart connectivity for RC control
  if (RCactive || useUart) {
    if (heartBeatHandle == NULL) xTaskCreateWithCaps(&heartBeatTask, "heartBeatTask", HB_STACK_SIZE, NULL, HB_PRI, &heartBeatHandle, STACK_MEM);
  }
}
#endif

#define EXT_NOCT_HOST "api.sunrise-sunset.org"
#define EXT_NOCT_PATH "/json?lat=%0.6f&lng=%0.6f&formatted=0"
void getNocturnal() {
  // Get length of current location night time in secs
  if (doGetExtIP) { 
    NetworkClientSecure hclient;
    if (remoteServerConnect(hclient, EXT_NOCT_HOST, HTTPS_PORT, "", GETEXTNOCT)) {
      HTTPClient http;
      int httpCode = HTTP_CODE_NOT_FOUND;
      char extNoctPath[100];
      sprintf(extNoctPath, EXT_NOCT_PATH, latLon[0], latLon[1]);
      if (http.begin(hclient, EXT_NOCT_HOST, HTTPS_PORT, extNoctPath)) {
        httpCode = http.GET();
        if (httpCode == HTTP_CODE_OK) {
          String payload = http.getString();
          char jsonVal[FILE_NAME_LEN] = "";
          if (getJsonValue(payload.c_str(), "day_length", jsonVal)) deepSleepTimer = DAY_LENGTH - atoi(jsonVal);
          else LOG_WRN("'day_length' field not present");
        } else LOG_WRN("Noctural duration request failed, error: %s", http.errorToString(httpCode).c_str());
        http.end();
      }
      remoteServerClose(hclient);
    }
  }
  if (deepSleepTimer) LOG_INF("Night time duration: %lu secs at Lat: %0.6f, Lon: %0.6f", deepSleepTimer, latLon[0], latLon[1]);
}

void doAppPing(bool timeSynced) {
  if (DEBUG_MEM) {
    currentStackUsage();
    checkMemory();
  }
  if (checkAlarm()) {
    remoteServerReset();
    getExtIP();
#if INCLUDE_SMTP
    if (smtpUse) {
      emailCount = 0;
      LOG_INF("Reset daily email allowance");
    }
#endif
    LOG_INF("Daily rollover");
  }
  // check for night time actions
  static bool atNight = false;
  if (wakeUse && wakePin < 0 && deepSleepTimer == 0 && timeSynced) getNocturnal();
  if (isNight(nightSwitch)) {
    if (wakeUse) {
#ifndef AUXILIARY
      digitalWrite(PWDN_GPIO_NUM, 1); // power down camera
#endif
      goToSleep(true);
    }
#if INCLUDE_PERIPH
    if (relayPin && relayMode && !atNight) {
      // turn on relay at night
      digitalWrite(relayPin, HIGH);
      atNight = true; 
    }
  } else if (relayPin && relayMode && atNight) {
    // turn off relay if day
    digitalWrite(relayPin, LOW); 
    atNight = false; 
#endif
  }
}

/************** telegram app specific **************/

void tgramAlert(const char* subject, const char* message) {
  // send motion alert to Telegram
  const char* pos1 = strchr(subject + 1, '/'); // extract filename
  const char* pos2 = strrchr(subject + 1, '.'); // remove extension
  // make filename into command
  if (pos1 != NULL && pos2 != NULL) {
    strncpy(alertCaption, pos1, pos2 - pos1);
    alertCaption[pos2 - pos1] = 0;
    strcat(alertCaption, " from ");
    strncat(alertCaption, hostName, sizeof(alertCaption) - strlen(alertCaption) - 1);
    if (alertBufferSize) alertReady = true; // return image
  } else LOG_WRN("Unable to send motion alert");
}

#if INCLUDE_TGRAM
// calls into telegram.cpp, so it must be gated the same way as its only caller.
// It compiled before only because it is static and provably unreferenced, so GCC
// dropped it before the linker ever saw the undefined sendTgram* symbols
static bool downloadAvi(const char* userCmd) {
  char* pos = strchr(userCmd, '_'); // if contains '_', assume filename
  if (pos != NULL) {
    // add folder name and avi extension to incoming file name
    char fileName[FILE_NAME_LEN];
    strncpy(fileName, userCmd, FILE_NAME_LEN - 1);
    pos = strchr(fileName, '_');
    memmove(pos, fileName, sizeof(fileName) - (pos - fileName));
    strncat(fileName, ".avi", sizeof(fileName) - 1 - strlen(fileName)); 
    if (STORAGE.exists(fileName)) sendTgramFile(fileName, "video/x-msvideo", "");
    else sendTgramMessage("AVI file not found: ", fileName, "");
  }
  return (bool)pos;
}
#endif

void appSpecificTelegramTask(void* p) {
#if INCLUDE_TGRAM
  // process Telegram interactions
  snprintf(tgramHdr, FILE_NAME_LEN - 1, "%s\n Ver: " APP_VER "\n\n/snap\n\n/log\n\n/extIP", hostName); 
  sendTgramMessage("Rebooted", "", "");
  char userCmd[FILE_NAME_LEN];
  
  while (true) {
    // service requests from Telegram
    if (getTgramUpdate(userCmd)) {     
      if (!strcmp(userCmd, "/snap")) {
        uint32_t startTime = millis();
        doKeepFrame = true;
        while (doKeepFrame && (millis() - startTime < MAX_FRAME_WAIT)) delay(100);
        if (!doKeepFrame && alertBufferSize) {
          sprintf(userCmd, "/snap from %s", hostName);
          sendTgramPhoto(alertBuffer, alertBufferSize, userCmd);
        }
      } else if (!strcmp(userCmd, "/log")) {
        // build unique ram log file name using time 
        char ramLogName[FILE_NAME_LEN];
        sprintf(ramLogName, "%s/ramlog_", DATA_DIR);
        time_t currEpoch = getEpoch();
        strftime(ramLogName + strlen(ramLogName), FILE_NAME_LEN - strlen(ramLogName), "%H%M%S", localtime(&currEpoch));
        strcat(ramLogName, TEXT_EXT);
        saveRamLog(ramLogName);   
        sprintf(userCmd, "/log from %s", hostName);
        sendTgramFile(ramLogName, "text/plain", userCmd);
        deleteFolderOrFile(ramLogName);
      } else if (!strcmp(userCmd, "/extIP")) {
        char extIpPt[24];
        strcpy(extIpPt, extIP);
        if (strlen(portFwd)) strcat(extIpPt, portFwd);
        sendTgramMessage("Ext IP: ", extIpPt, "");
      } else {
        // initially assume it is an avi file download request
        if (!downloadAvi(userCmd)) sendTgramMessage("Request not recognised: ", userCmd, "");
      }
    } else {
      // send out any outgoing alerts from app
      if (alertReady) {
        alertReady = false;
        sendTgramPhoto(alertBuffer, alertBufferSize, alertCaption);
        alertBufferSize = 0;
      } else delay(5000); // avoid thrashing
    }
  }
#endif
  vTaskDelete(NULL);
}

/************** default app configuration **************/
const char* appConfig = R"~(
ST_SSID~~99~~na
fsPort~21~99~~na
fsServer~~99~~na
ftpUser~~99~~na
fsWd~~99~~na
fsUse~~99~~na
smtp_port~465~99~~na
smtp_server~smtp.gmail.com~99~~na
smtp_login~~99~~na
smtp_email~~99~~na
Auth_Name~~99~~na
useHttps~~99~~na
useSecure~~99~~na
useFtps~~99~~na
extIP~~99~~na
restart~~99~~na
sdLog~0~99~~na
battUse~0~99~~Monitor battery voltage
battPin~1~99~~Battery divider ADC pin
battScale~2000~99~~Cell mV per 1000 tap mV
battWarnMv~3400~99~~Land recording below this cell mV
idleFps~0~99~~Idle sensor FPS when nothing needs frames (0 = off)
idleSecs~10~99~~Seconds idle before sensor throttles
wifiSleep~1~99~~Wifi modem sleep when idle
cpuFreqMhz~240~99~~CPU MHz (80 160 240)
wifiTxDbm~20~99~~Wifi transmit power dBm (2-20)
xclkMhz~20~98~~na
ae_level~-2~98~~na
banding~0~98~~na
aec~1~98~~na
tunedFps~0~98~~na
sdBusDiv~4~98~~na
zoneMask~65535~98~~na
aec2~0~98~~na
aec_value~204~98~~na
agc~1~98~~na
agc_gain~0~98~~na
autoUpload~0~98~~na
deleteAfter~0~98~~na
awb~1~98~~na
awb_gain~1~98~~na
bpc~1~98~~na
brightness~0~98~~na
colorbar~0~98~~na
contrast~0~98~~na
dcw~0~98~~na
enableMotion~0~98~~na
fps~20~98~~na
framesize~10~98~~na
gainceiling~1023~98~~na
hmirror~0~98~~na
lenc~1~98~~na
lswitch~10~98~~na
micGain~5~98~~na
motionVal~8~98~~na
quality~12~98~~na
raw_gma~1~98~~na
record~0~98~~na
saturation~0~98~~na
sharpness~0~98~~na
denoise~0~98~~na
special_effect~0~98~~na
timezone~GMT0~98~~na
vflip~0~98~~na
wb_mode~0~98~~na
wpc~1~98~~na
ST_ip~~0~T~Static IP address
ST_gw~~0~T~Router IP address
ST_sn~255.255.255.0~0~T~Router subnet
ST_ns1~~0~T~DNS server
ST_ns2~~0~T~Alt DNS server
AP_Pass~~0~T~AP Password
AP_ip~~0~T~AP IP Address if not 192.168.4.1
AP_sn~~0~T~AP subnet
AP_gw~~0~T~AP gateway
allowAP~1~0~C~Allow simultaneous AP 
doGetExtIP~1~0~C~Enable get external IP
wifiTimeoutSecs~30~0~N~WiFi connect timeout (secs)
logType~0~99~N~Output log selection
ntpServer~pool.ntp.org~0~T~NTP Server address
alarmHour~1~2~N~Hour of day for daily actions
refreshVal~5~2~N~Web page refresh rate (secs)
responseTimeoutSecs~10~2~N~Server response timeout (secs)
maxFrames~3600~1~N~Max frames in recording
dashCamOn~0~98~~na
moveStartChecks~5~1~N~Checks per second for start motion
moveStopChecks~2~1~N~Checks per second while recording
moveStopSecs~10~98~~na
detectMotionFrames~3~1~N~Num changed checks to start motion
detectNightFrames~10~1~N~Min dark frames to indicate night
zoneCount~2~1~N~Zones changed at once to signal motion
streamVid~0~8~C~Enable NVR Video stream: /sustain?video=1
streamAud~0~8~C~Enable NVR Audio stream: /sustain?audio=1
smtpUse~0~2~C~Enable email sending
smtpMaxEmails~10~2~N~Max daily alerts
sdMinCardFreeSpace~100~2~N~Min free MBytes on SD before action
sdFreeSpaceMode~1~2~S:No Check:Delete oldest:Ftp then delete~Action mode on SD min free
formatIfMountFailed~0~2~C~Format file system on failure
wakeUse~0~2~C~Deep sleep app during night
mqtt_active~0~2~C~Mqtt enabled
mqtt_broker~~2~T~Mqtt server ip to connect
mqtt_port~1883~2~N~Mqtt server port
mqtt_user~~2~T~Mqtt user name
mqtt_user_Pass~~2~T~Mqtt user password
mqtt_topic_prefix~homeassistant/~2~T~Mqtt topic path prefix
usePing~1~0~C~Use ping
tgramUse~0~2~C~Use Telegram Bot
tgramToken~~2~T~Telegram Bot token
tgramChatId~~2~T~Telegram chat identifier
)~";
