
/*
 Support globals and the day/night classifier for motion detection.

 Detection itself lives in mjpeg2sd.cpp (zoneMotion): the OV5640's own 4x4 AEC zone
 luminance grid is compared between checks, ~20 SCCB reads with no frame decode. The
 JPEG-decode background-subtraction detector that used to live here was removed with it:
 decoding at or above ~1.3MP intermittently hard hung the board, capped detection at HD,
 forced a VGA round-trip on every recording, and required suspending detection while
 capturing - which produced a deterministic false retrigger after every motion recording
 (see BOARD_TESTING.md for the measurements behind all of this).

 s60sc 2020, 2023, 2025
*/

#include "appGlobals.h"

// motion recording parameters
bool dbgMotion = false; // stream the zone overlay instead of the live image
// 3 at the 5/s check rate = 0.6s of sustained movement. Measured on the bench: a walk
// across half the frame peaked at 4 consecutive (5 missed it), while single-zone LED
// blinks never exceed 1 - see BOARD_TESTING.md zone-only detection session
int detectMotionFrames = 3; // min sequence of tripped checks to confirm motion
int detectNightFrames = 10; // frames of sequential darkness to avoid spurious day / night switching

uint8_t lightLevel; // current ambient light level, from the sensor's YAVG
uint8_t nightSwitch = 20; // initial white level % for night/day switching
float motionVal = 8.0; // motion sensitivity setting, maps to the per-zone delta threshold
uint8_t* motionJpeg = NULL; // zone overlay image for the Show Motion stream
size_t motionJpegLen = 0;

#ifndef AUXILIARY

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

#else
// dummies
bool isNight(uint8_t nightSwitch) {return false;}

#endif // AUXILIARY
