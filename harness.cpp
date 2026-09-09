// Bench test harness. Lets one board drive and instrument the other.
//
// WHAT THIS IS FOR. COM3 could previously do exactly one thing to COM4: pulse its RESET pad
// through GPIO 2. That is an EN reset, and the notebook records it as insufficient - on
// 4 Sep 2026 COM4 came back from a pulse alive but handling every packet about a second
// late, and only a real power cycle cured it, because the card and the radio keep their
// state through EN. This module adds the power cut that EN cannot do, plus the
// instrumentation to see what the board under test is actually doing:
//
//   INA3221   bus volts and current on three rails, over I2C
//   2 relays  cut USB power and battery power independently
//   TS3USB30  isolate the USB data pair, by steering it to a port with nothing on it
//   MAX31850  OV5640 temperature from a thermocouple bonded to the sensor package.
//             This project has never had one: "camera temp" everywhere else is the
//             ESP32-S3 die, and the OV5640 exposes no temperature at all
//   LED       PWM scene light, so a measurement stops tracking the weather
//
// IT IS NOT peripherals.cpp. That module is 688 lines of servos, steppers, joysticks, LED
// bars, PIR and RC lights, of which this needs two small patterns - the lamp's ledc calls
// and the 1-Wire read. Enabling it measured +10,344 bytes of flash and +856 of RAM to get
// them, and would reintroduce subsystems this fork deliberately stripped, whose config rows
// were all deleted. Three of the five devices here have no equivalent in it anyway.
//
// TWO TRAPS FOUND BEFORE A LINE OF THIS WAS WRITTEN, both worth keeping in view:
//
//  1. I2C PORT 0 IS TAKEN. mjpeg2sd.cpp sets config.sccb_i2c_port = 0, so the camera's SCCB
//     owns port 0, which is what Arduino calls Wire. This module uses Wire1. Putting the
//     INA3221 on Wire would fight the sensor bus, and since otaConfirm() gates on the camera
//     being present, that failure would also stop every future OTA confirming.
//  2. THE LIBRARY GUARD IN peripherals.cpp DOES NOT WORK HERE. It tests
//     __has_include("../libraries/DallasTemperature/DallasTemperature.h"), a path relative
//     to the sketch folder, so it only resolves when the sketch sits inside the Arduino
//     sketchbook. This one uses the angle-bracket form, which works wherever the libraries
//     live, and degrades to "no thermocouple" with a loud boot warning rather than an
//     #error that takes the other four devices down with it.
//
// FAIL SAFE. Every switch line means "connected" when it reads low, so the pull-downs, a
// reboot, a crash and an unprogrammed pin all leave the board under test POWERED. A harness
// that can strand the board it exists to rescue is worse than no harness. Wire the relays
// through their normally-closed contacts to match.

#include "appGlobals.h"

#if INCLUDE_HARNESS

#include <Wire.h>

// Angle-bracket test, so it resolves from the real library path rather than a sibling
// folder. Missing libraries cost the thermocouple only; the rest of the harness still runs
#if __has_include(<DallasTemperature.h>)
  #include <OneWire.h>
  #include <DallasTemperature.h>
  #define HARNESS_HAS_1WIRE true
#else
  #define HARNESS_HAS_1WIRE false
#endif

// config, all persisted through appConfig rows. Pin defaults are the COM3 map; a pin of 0
// disables that device, which is how this module stays inert on a board without the hardware
bool harnessUse = false;
int hLampPin = 1;          // D0, freed from the battery divider
int hLampFreq = 20000;     // Hz - see setHarnessLamp() for why this is not 50
int hLampBits = 10;        // ledc duty resolution
int hRelayUsbPin = 3;      // D2, USB power. Strapping pin, needs an external pull-down
int hRelayBattPin = 43;    // D6, battery power. UART0 TX given up for it
// D7, the TS3USB30's SELECT line, not an output enable. UART0 RX was given up for it.
// Low steers the common port to port 1, where COM4 lives; high steers it to port 2, which
// has nothing on it. So "disconnected" is really "connected to nowhere", and COM4's D+/D-
// are left floating with the host no longer able to see or feed them
int hUsbMuxPin = 44;
// 0 is the confirmed wiring above, where low means COM4 is selected. Kept as a row rather
// than assumed in code so a rewire, or a swap of ports 1 and 2, is a setting and not a build
int hUsbMuxInvert = 0;
int hTcPin = 4;            // D3, 1-Wire
int hSdaPin = 5;           // D4
int hSclPin = 6;           // D5
int hInaAddr = 0x40;       // INA3221 base address
// 0.05 ohm per channel, confirmed against the breakout 8 Sep 2026, and the same value
// Adafruit_INA3221 assumes. With the INA3221's 40 uV shunt LSB that gives 0.8 mA of
// resolution, and the 13-bit signed register saturates at +-163.84 mV, so +-3.28 A full
// scale. Both ends suit this job: a XIAO draws hundreds of mA, and an idle-current change
// of a few mA is still several counts rather than noise
int hShuntMilliOhm = 50;
int hCycleMs = 4000;       // default dead time for a power cycle
int hPollMs = 2000;        // how often the harness task refreshes its readings
int hUsbStaggerMs = 50;    // gap between the data pair and VBUS, imitating the connector's pin lengths

// live values, published in /status
float hChVolts[3] = {0};
float hChMilliAmps[3] = {0};
float hTcCelsius = NULL_TEMP;
uint8_t hLampLevel = 0;
bool hUsbPowerOn = true;
bool hBattPowerOn = true;
bool hUsbDataOn = true;
bool hInaPresent = false;
bool hTcPresent = false;

static bool harnessInit = false;
static bool lampInit = false;
static TaskHandle_t cycleHandle = NULL;
static TaskHandle_t plugHandle = NULL;
static TaskHandle_t harnessHandle = NULL;

#define INA_REG_SHUNT(ch) (0x01 + ((ch) * 2))
#define INA_REG_BUS(ch)   (0x02 + ((ch) * 2))
#define INA_REG_MANUF     0xFE
#define INA_MANUF_TI      0x5449  // "TI" - warned about, not enforced, so a relabelled part still reads

/************************ INA3221 ************************/

// Register level rather than a library, and the reason is dependencies rather than
// availability: Adafruit_INA3221 IS in the Arduino index and marked Recommended, so it
// installs in one command, but it pulls Adafruit BusIO with it and this fork's whole build
// story is the core plus one library. The part is a handful of 16-bit reads.
//
// The arithmetic below was checked against Adafruit_INA3221 rather than derived alone, and
// agrees with it exactly: bus >> 3 at 8 mV per LSB, shunt >> 3 at 40 uV, maker ID 0x5449,
// 0.05 ohm default shunt, sign carried by int16_t. Their begin() writes a config register
// that matches the chip's own reset state, which is why leaving it untouched behaves the
// same. If this ever needs averaging or a different conversion time, take the library
static bool inaRead16(uint8_t reg, uint16_t& val) {
  Wire1.beginTransmission(hInaAddr);
  Wire1.write(reg);
  if (Wire1.endTransmission(false) != 0) return false;
  if (Wire1.requestFrom(hInaAddr, 2) != 2) return false;
  val = ((uint16_t)Wire1.read() << 8) | Wire1.read();
  return true;
}

// Both shunt and bus registers hold a signed 13-bit value left aligned in 16 bits, so the
// sign has to survive the shift - an unsigned shift turns every discharge current positive
static int16_t inaSigned13(uint16_t raw) {
  return (int16_t)raw >> 3;
}

static void readIna3221() {
  if (!hInaPresent) return;
  for (int ch = 0; ch < 3; ch++) {
    uint16_t rawBus = 0, rawShunt = 0;
    if (!inaRead16(INA_REG_BUS(ch), rawBus) || !inaRead16(INA_REG_SHUNT(ch), rawShunt)) {
      hInaPresent = false; // stop reporting stale numbers as live ones
      LOG_WRN("harness: INA3221 stopped answering on channel %d", ch + 1);
      return;
    }
    hChVolts[ch] = inaSigned13(rawBus) * 0.008f;               // 8 mV per LSB
    int32_t shuntMicroVolts = inaSigned13(rawShunt) * 40;      // 40 uV per LSB
    // I(mA) = V(uV) / R(mOhm), which falls straight out of V = IR in those units
    hChMilliAmps[ch] = hShuntMilliOhm > 0 ? (float)shuntMicroVolts / hShuntMilliOhm : 0.0f;
  }
}

static void prepIna3221() {
  hInaPresent = false;
  if (hSdaPin <= 0 || hSclPin <= 0) return;
  // Wire1, never Wire - see trap 1 in the header comment
  if (!Wire1.begin(hSdaPin, hSclPin)) {
    LOG_WRN("harness: I2C would not start on SDA %d SCL %d", hSdaPin, hSclPin);
    return;
  }
  uint16_t manuf = 0;
  if (!inaRead16(INA_REG_MANUF, manuf)) {
    LOG_WRN("harness: no INA3221 at 0x%02X - check the address straps and the STEMMA chain", hInaAddr);
    return;
  }
  hInaPresent = true;
  // The config register is left at its power-on default, which already runs all three
  // channels continuously. Averaging is worth adding once someone has the datasheet open;
  // guessing at the encoding here would be the kind of assumption this project bans
  if (manuf == INA_MANUF_TI) LOG_INF("harness: INA3221 found at 0x%02X", hInaAddr);
  else LOG_WRN("harness: device at 0x%02X answered with maker ID 0x%04X, expected 0x%04X - reading it anyway", hInaAddr, manuf, INA_MANUF_TI);
}

/************************ MAX31850 thermocouple ************************/

static void readThermocouple() {
#if HARNESS_HAS_1WIRE
  if (hTcPin <= 0) return;
  // Function-local statics, so the bus object is built once on the first call, which is
  // after the config load has set hTcPin. The consequence is that CHANGING hTcPin at runtime
  // does nothing until a reboot - acceptable for a wiring fact, but it would be a silent
  // no-op rather than an error, so say it here
  static OneWire oneWire(hTcPin);
  static DallasTemperature probe(&oneWire);
  static uint8_t addr[8] = {0};
  static bool started = false;
  static bool converting = false;
  if (!started) {
    probe.begin();
    // A conversion takes up to 750 ms, and nothing here may block for that long. Request on
    // one pass and collect on the next. peripherals.cpp spends an entire task on the same
    // problem because it waits; this does not have to
    probe.setWaitForConversion(false);
    started = true;
  }
  // family code 0 is not valid, so it doubles as "not yet discovered" and keeps the bus
  // scan off every pass
  if (addr[0] == 0) {
    if (!probe.getAddress(addr, 0)) {
      hTcPresent = false;
      hTcCelsius = NULL_TEMP;
      addr[0] = 0;
      return;
    }
    // 0x3B is the MAX31850 (DallasTemperature calls it DS1825MODEL and handles both); 0x28
    // is a plain DS18B20, accepted so an ordinary probe can stand in while the thermocouple
    // is unbonded. peripherals.cpp tests 0x28 alone, which would ignore a MAX31850 in silence
    if (addr[0] != 0x3B && addr[0] != 0x28) {
      LOG_WRN("harness: 1-Wire device family 0x%02X is neither MAX31850 nor DS18B20", addr[0]);
      hTcPresent = false;
      addr[0] = 0;
      return;
    }
    hTcPresent = true;
    LOG_INF("harness: 1-Wire %s found on GPIO %d", addr[0] == 0x3B ? "MAX31850" : "DS18B20", hTcPin);
  }
  if (!converting) {
    probe.requestTemperatures();
    converting = true;
    return; // collect it next pass
  }
  converting = false;
  float c = probe.getTempC(addr);
  // The MAX31850 reports thermocouple faults through the same channel as a reading, so they
  // must not reach the log or the UI as temperatures. An open circuit is the expected one:
  // it is what a probe that has come unbonded from a hot sensor looks like
  if (c == DEVICE_FAULT_OPEN_C) LOG_WRN("harness: thermocouple open circuit - probe detached?");
  else if (c == DEVICE_FAULT_SHORTGND_C) LOG_WRN("harness: thermocouple shorted to ground");
  else if (c == DEVICE_FAULT_SHORTVDD_C) LOG_WRN("harness: thermocouple shorted to VDD");
  else if (c == DEVICE_DISCONNECTED_C) LOG_WRN("harness: 1-Wire read failed");
  else {
    hTcCelsius = c;
    return;
  }
  hTcCelsius = NULL_TEMP;
#endif
}

/************************ scene light ************************/

void setHarnessLamp(uint8_t level) {
  if (hLampPin <= 0) return;
  if (!lampInit) {
    // NOT peripherals.cpp's PWM_FREQ, which is 50 Hz because it is shared with the servo
    // code. A lamp at 50 Hz strobes every frame the sensor takes and fights the AEC, which
    // is the opposite of what a reference light is for. This runs far above the line rate
    // so a single row's exposure spans many PWM periods
    if (!ledcAttach(hLampPin, hLampFreq, hLampBits)) {
      LOG_WRN("harness: lamp PWM would not attach to GPIO %d", hLampPin);
      return;
    }
    lampInit = true;
    LOG_INF("harness: lamp on GPIO %d at %d Hz", hLampPin, hLampFreq);
  }
  hLampLevel = level > 100 ? 100 : level;
  uint32_t full = (1UL << hLampBits) - 1;
  ledcWrite(hLampPin, (full * hLampLevel) / 100);
}

/************************ power and data switching ************************/

// Every line is "low means connected", and that now holds for all three by wiring rather
// than by hope. The relays pass power through their normally-closed contacts, so an
// unenergised coil is a connected rail, and the mux selects port 1 - where COM4 is - when
// its select line is low. With the external pull-downs, an unprogrammed pin, a reboot, a
// crash and the whole window before prepHarness() runs all leave the far board powered and
// still enumerated
static void driveSwitch(int pin, bool connected, bool invert) {
  if (pin <= 0) return;
  pinMode(pin, OUTPUT);
  digitalWrite(pin, (connected != invert) ? LOW : HIGH);
}

// The VBUS relay on its own, deliberately not exported. Cutting the rail while the data pair
// is still connected is not something a cable can do, so the only route to it from outside is
// the staggered sequence below
static void setUsbRail(bool on) {
  hUsbPowerOn = on;
  driveSwitch(hRelayUsbPin, on, false);
  LOG_ALT("harness: USB power %s", on ? "connected" : "CUT");
}

void setBattPower(bool on) {
  hBattPowerOn = on;
  driveSwitch(hRelayBattPin, on, false);
  LOG_ALT("harness: battery power %s", on ? "connected" : "CUT");
}

void setUsbData(bool on) {
  hUsbDataOn = on;
  driveSwitch(hUsbMuxPin, on, hUsbMuxInvert != 0);
  LOG_ALT("harness: USB data %s", on ? "connected" : "isolated");
}

// A USB connector staggers its own pins: ground and VBUS are longer than D+/D-, so unplugging
// breaks the data pair first and plugging makes it last. These reproduce that with
// hUsbStaggerMs, because a rig that cut both at the same instant would be testing something
// that cannot physically happen, and the order is what decides whether the far board sees a
// clean detach or a rail collapsing under an active link. Ground is never switched here,
// matching the connector's longest pin.
//
// BOTH DELAY, so they may only be called from a task, never from the httpd worker
static void usbDisconnectSeq() {
  setUsbData(false);
  vTaskDelay(pdMS_TO_TICKS(hUsbStaggerMs));
  setUsbRail(false);
}

static void usbConnectSeq() {
  setUsbRail(true);
  vTaskDelay(pdMS_TO_TICKS(hUsbStaggerMs));
  setUsbData(true);
}

static void powerCycleTask(void* arg) {
  int ms = (int)(intptr_t)arg;
  // USB goes down the way a hand on the cable would take it down. The battery has no
  // connector to imitate, so it simply follows, and both rails are then off together for the
  // dead time - which is the whole point, since an EN reset leaves the card and the radio
  // holding their state and a rail that never reaches zero would too
  usbDisconnectSeq();
  setBattPower(false);
  LOG_ALT("harness: both supplies cut for %d ms", ms);
  vTaskDelay(pdMS_TO_TICKS(ms));
  // Back up in the reverse order, battery first so the board has a rail before its USB is
  // spoken to, then VBUS and the data pair with the same stagger
  setBattPower(true);
  usbConnectSeq();
  LOG_ALT("harness: supplies restored, far board should be booting");
  cycleHandle = NULL;
  vTaskDelete(NULL);
}

// Unplugging USB without a full power cycle is a test in its own right: it asks what the far
// board does when the cable goes while it is mid recording, which is an ordinary way for a
// deployed camera to lose its host
static void usbPlugTask(void* arg) {
  if ((bool)(intptr_t)arg) usbConnectSeq();
  else usbDisconnectSeq();
  plugHandle = NULL;
  vTaskDelete(NULL);
}

void harnessUsbPower(int val) {
  if (!harnessUse) {
    LOG_WRN("harness: hUsbPower refused, harnessUse is off");
    return;
  }
  if (plugHandle != NULL || cycleHandle != NULL) {
    LOG_WRN("harness: a USB sequence is already running");
    return;
  }
  if (xTaskCreate(usbPlugTask, "harnessPlug", 3072, (void*)(intptr_t)(val != 0), 1, &plugHandle) != pdPASS) {
    plugHandle = NULL;
    LOG_WRN("harness: USB sequence task not created - nothing was switched");
  }
}

void harnessPowerCycle(int val) {
  if (!harnessUse) {
    LOG_WRN("harness: powerCycle refused, harnessUse is off");
    return;
  }
  // guards against the plug sequence too: two of these interleaving would drive the same
  // three pins from two tasks and leave the far board in whichever state lost the race
  if (cycleHandle != NULL || plugHandle != NULL) {
    LOG_WRN("harness: a USB sequence is already running");
    return;
  }
  int ms = (val <= 1) ? hCycleMs : constrain(val, 250, 30000);
  if (xTaskCreate(powerCycleTask, "harnessCycle", 3072, (void*)(intptr_t)ms, 1, &cycleHandle) != pdPASS) {
    cycleHandle = NULL;
    LOG_WRN("harness: power cycle task not created - nothing was switched");
  }
}

/************************ reporting ************************/

// Its own task, at low priority. The thermocouple conversion is handled non-blocking above,
// so the remaining reason is 1-Wire's bit banging, and it is worth being exact about how bad
// that is rather than hand waving at it.
//
// Read from OneWire 2.3.8: on ESP32 the library's noInterrupts() is portENTER_CRITICAL, so
// it takes a spinlock and masks interrupts on the CURRENT CORE only, not both. The sections
// are per bit and short - the worst is 70 us inside reset(), and a zero bit holds for 65 us,
// while reset()'s own 480 us low pulse sits OUTSIDE the critical section. A scratchpad read
// is therefore a hundred or so brief masks rather than one long one, and camera DMA is
// hardware that keeps running through them.
//
// So this is a precaution, not a fix for an observed fault. It stays off the capture task
// because that task's frame timing is measured here to three decimals, and off the httpd
// worker because it would sit in the middle of a status poll. Note that pinning this to
// "the other core" is not available as a quick fix: captureTask is created with
// xTaskCreate and so has no affinity either, and isolating the two would mean pinning both
static void harnessTask(void* arg) {
  while (true) {
    readIna3221();
    readThermocouple();
    vTaskDelay(pdMS_TO_TICKS(hPollMs));
  }
}

void harnessReport() {
  LOG_INF("harness: %s, INA3221 %s, thermocouple %s",
    harnessUse ? "on" : "off", hInaPresent ? "present" : "absent", hTcPresent ? "present" : "absent");
  for (int ch = 0; ch < 3; ch++)
    LOG_INF("  channel %d: %.3f V, %.1f mA", ch + 1, hChVolts[ch], hChMilliAmps[ch]);
  if (hTcPresent) LOG_INF("  OV5640 thermocouple: %.2f C", hTcCelsius);
  // The two supplies are switched independently so a test can isolate one of them: cut USB
  // and the far board runs on battery, cut battery and its whole draw is on the USB channel,
  // cut both and it is a hard power cycle. Name the resulting combination, because that is
  // the thing a reader of this log actually wants to know
  const char* supply = hUsbPowerOn ? (hBattPowerOn ? "USB and battery" : "USB only")
                                   : (hBattPowerOn ? "battery only" : "NOTHING - far board is dead");
  LOG_INF("  supply: %s. USB data %s, lamp %u%%", supply, hUsbDataOn ? "connected" : "isolated", hLampLevel);
}

void prepHarness() {
  if (!harnessUse) {
    LOG_INF("harness: not enabled");
    return;
  }
  // Switches first and before anything can fail, so the far board is powered from the
  // earliest possible moment in this module's life
  setUsbRail(true);
  setBattPower(true);
  setUsbData(true);
  prepIna3221();
#if !HARNESS_HAS_1WIRE
  LOG_WRN("harness: built without OneWire and DallasTemperature - no OV5640 temperature");
#endif
  setHarnessLamp(hLampLevel);
  if (xTaskCreate(harnessTask, "harness", HARNESS_STACK_SIZE, NULL, 1, &harnessHandle) != pdPASS) {
    harnessHandle = NULL;
    LOG_WRN("harness: poll task not created - switches work, readings will not update");
  }
  harnessInit = true;
  harnessReport();
}

#endif // INCLUDE_HARNESS
