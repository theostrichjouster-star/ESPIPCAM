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
//   MCP9601   OV5640 temperature from a thermocouple bonded to the sensor package, on
//             the same I2C chain. This project has never had one: "camera temp"
//             everywhere else is the ESP32-S3 die, and the OV5640 exposes none at all
//   LED       PWM scene light, so a measurement stops tracking the weather
//
// IT IS NOT peripherals.cpp. That module is 688 lines of servos, steppers, joysticks, LED
// bars, PIR and RC lights, of which this needs one small pattern - the lamp's ledc calls.
// Enabling it measured +10,344 bytes of flash and +856 of RAM to get that, and would
// reintroduce subsystems this fork deliberately stripped, whose config rows were all
// deleted. Three of the five devices here have no equivalent in it anyway.
//
// THREE TRAPS FOUND BEFORE THIS WAS TRUSTED, all worth keeping in view:
//
//  1. I2C PORT 0 IS TAKEN. mjpeg2sd.cpp sets config.sccb_i2c_port = 0, so the camera's SCCB
//     owns port 0, which is what Arduino calls Wire. This module uses Wire1. Putting either
//     device on Wire would fight the sensor bus, and since otaConfirm() gates on the camera
//     being present, that failure would also stop every future OTA confirming.
//  2. THE MCP9601 NAKs A ZERO-LENGTH WRITE, which is exactly what an I2C bus scan sends, so
//     a scan reports it missing however well it is wired. Adafruit's guide says so outright.
//     Presence here is a real register read of the device ID, never an address probe.
//  3. AN UNCONNECTED THERMOCOUPLE STILL READS A PLAUSIBLE TEMPERATURE. The part does have
//     open and short circuit detection, but it works through the VSENSE pin and the RA/RB
//     divider of datasheet figure 1-1, and this breakout fits neither - Adafruit's own guide
//     warns "There will not be an error!". The status bits are read and reported anyway,
//     since they cost nothing and would work on a board that wires VSENSE, but the real
//     guard is publishing the cold junction beside the hot one: a probe bonded to a working
//     sensor reads warmer than the board it sits on, and one that has fallen off does not.
//
// FAIL SAFE. Every switch line means "connected" when it reads low, so the pull-downs, a
// reboot, a crash and an unprogrammed pin all leave the board under test POWERED. A harness
// that can strand the board it exists to rescue is worse than no harness. Wire the relays
// through their normally-closed contacts to match.

#include "appGlobals.h"

#if INCLUDE_HARNESS

#include <Wire.h>

// config, all persisted through appConfig rows. Pin defaults are the COM3 map; a pin of 0
// disables that device, which is how this module stays inert on a board without the hardware
bool harnessUse = false;
int hLampPin = 4;          // D3, freed by the thermocouple moving to I2C
int hLampFreq = 20000;     // Hz - see setHarnessLamp() for why this is not 50
int hLampBits = 10;        // ledc duty resolution
int hRelayUsbPin = 3;      // D2, USB power. Strapping pin, needs an external pull-down
int hRelayBattPin = 1;     // D0, battery power. Freed by unhooking the battery divider
// D7, the TS3USB30's SELECT line, not an output enable. UART0 RX was given up for it.
// Low steers the common port to port 1, where COM4 lives; high steers it to port 2, which
// has nothing on it. So "disconnected" is really "connected to nowhere", and COM4's D+/D-
// are left floating with the host no longer able to see or feed them
int hUsbMuxPin = 44;
// 0 is the confirmed wiring above, where low means COM4 is selected. Kept as a row rather
// than assumed in code so a rewire, or a swap of ports 1 and 2, is a setting and not a build
int hUsbMuxInvert = 0;
// MCP9601 thermocouple amplifier, on the same Wire1 chain as the INA3221 - it needs no pin
// of its own, which is what freed D3. 0x67 is the breakout with its ADDR pin unconnected;
// the part answers anywhere in 0x60-0x67 depending on how ADDR is strapped
int hTcAddr = 0x67;
int hTcType = 0;           // 0 = type K, the thermocouple Adafruit ships. Datasheet order: K J T N S E B R
int hTcFilter = 4;         // 0 = off, 7 = heaviest. Sensor config bits 2:0
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
float hTcColdC = NULL_TEMP;
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

// MCP9601 registers, Microchip DS20005426F table 5-1
#define MCP_REG_HOTJUNC   0x00
#define MCP_REG_COLDJUNC  0x02
#define MCP_REG_STATUS    0x04
#define MCP_REG_SENSORCFG 0x05
#define MCP_REG_DEVICEID  0x20
// STATUS bits, datasheet register 5-6. Bit 4 carries two meanings across the family: input
// range exceeded on an MCP9600, thermocouple disconnected on an MCP9601. Both tell a caller
// the same thing - do not trust this reading - so one name covers it
#define MCP_STATUS_OPEN   0x10
#define MCP_STATUS_SHORT  0x20
#define MCP_ID_9600       0x40
#define MCP_ID_9601       0x41

/************************ shared I2C ************************/

// Both devices sit on Wire1. A register read is the pointer byte written with a repeated
// start - endTransmission(false) is what holds the bus - then the read
static bool hI2cRead(uint8_t addr, uint8_t reg, uint8_t* buf, size_t len) {
  Wire1.beginTransmission(addr);
  Wire1.write(reg);
  if (Wire1.endTransmission(false) != 0) return false;
  if (Wire1.requestFrom((int)addr, (int)len) != len) return false;
  for (size_t i = 0; i < len; i++) buf[i] = Wire1.read();
  return true;
}

static bool hI2cWrite8(uint8_t addr, uint8_t reg, uint8_t val) {
  Wire1.beginTransmission(addr);
  Wire1.write(reg);
  Wire1.write(val);
  return Wire1.endTransmission() == 0;
}

// One bus, started once, before either device is asked anything. Wire1, never Wire - trap 1
static bool prepI2c() {
  if (hSdaPin <= 0 || hSclPin <= 0) {
    LOG_WRN("harness: I2C pins are not set - no current readings and no temperature");
    return false;
  }
  if (!Wire1.begin(hSdaPin, hSclPin)) {
    LOG_WRN("harness: I2C would not start on SDA %d SCL %d", hSdaPin, hSclPin);
    return false;
  }
  return true;
}

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
  uint8_t b[2];
  if (!hI2cRead((uint8_t)hInaAddr, reg, b, 2)) return false;
  val = ((uint16_t)b[0] << 8) | b[1];
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

/************************ MCP9601 thermocouple ************************/

// Register level for the same reason as the INA3221: Adafruit_MCP9600 is in the index and
// would do this, but it pulls Adafruit BusIO and this is four register reads. Every constant
// is off Microchip DS20005426F rather than assumed, and agrees with the Adafruit library
// where the two overlap - TH and TC signed 16-bit at 0.0625 C per LSB (table 5-1), device ID
// high byte 0x41 against the MCP9600's 0x40 (register 5-13), thermocouple type in sensor
// config bits 6:4 with the filter coefficient in bits 2:0.
//
// There is no conversion handshake to run. At the reset default of 18-bit resolution the
// part samples at 3 SPS, about 320 ms, and hPollMs is 2000, so every read collects a
// completed conversion. The 1-Wire part this replaced needed a request pass and a collect
// pass on alternate polls to avoid blocking for 750 ms.
static void readThermocouple() {
  if (!hTcPresent) return;
  uint8_t status = 0, hot[2], cold[2];
  if (!hI2cRead((uint8_t)hTcAddr, MCP_REG_STATUS, &status, 1)
   || !hI2cRead((uint8_t)hTcAddr, MCP_REG_HOTJUNC, hot, 2)
   || !hI2cRead((uint8_t)hTcAddr, MCP_REG_COLDJUNC, cold, 2)) {
    hTcPresent = false; // stop reporting a stale temperature as a live one
    hTcCelsius = hTcColdC = NULL_TEMP;
    LOG_WRN("harness: MCP9601 stopped answering at 0x%02X", hTcAddr);
    return;
  }
  // Reported, never relied on - trap 3. On this breakout neither bit can assert, so silence
  // here is not evidence that the probe is still attached. Logged on the EDGE and not on
  // every pass: a standing fault would otherwise put a line in the log every hPollMs, and
  // the RTC ring holds only a couple of minutes of chatter as it is
  static uint8_t lastFault = 0;
  uint8_t fault = status & (MCP_STATUS_OPEN | MCP_STATUS_SHORT);
  if (fault != lastFault) {
    if (fault & MCP_STATUS_SHORT) LOG_WRN("harness: thermocouple shorted to VDD or ground");
    if (fault & MCP_STATUS_OPEN) LOG_WRN("harness: thermocouple open circuit, or out of range for the configured type");
    if (!fault) LOG_INF("harness: thermocouple fault cleared");
    lastFault = fault;
  }
  // The cold junction is the part's own on-die sensor and stays valid through a thermocouple
  // fault, so it is published either way. The hot junction is not, and must not be handed out
  // as a temperature when the part has just said it is untrustworthy
  hTcColdC = (float)(int16_t)(((uint16_t)cold[0] << 8) | cold[1]) * 0.0625f;
  hTcCelsius = fault ? NULL_TEMP : (float)(int16_t)(((uint16_t)hot[0] << 8) | hot[1]) * 0.0625f;
}

static void prepMcp9601() {
  hTcPresent = false;
  hTcCelsius = hTcColdC = NULL_TEMP;
  if (hTcAddr <= 0) return;
  uint8_t id[2];
  // A REGISTER read, never an address probe - trap 2
  if (!hI2cRead((uint8_t)hTcAddr, MCP_REG_DEVICEID, id, 2)) {
    LOG_WRN("harness: no MCP9601 at 0x%02X - check the ADDR strap and the STEMMA chain", hTcAddr);
    return;
  }
  if (id[0] == MCP_ID_9601) LOG_INF("harness: MCP9601 at 0x%02X, revision %u.%u", hTcAddr, id[1] >> 4, id[1] & 0x0F);
  else if (id[0] == MCP_ID_9600) LOG_WRN("harness: device at 0x%02X is an MCP9600, not a 9601 - it reads temperature but has no fault detection", hTcAddr);
  else {
    LOG_WRN("harness: device at 0x%02X answered with device ID 0x%02X, expected 0x%02X", hTcAddr, id[0], MCP_ID_9601);
    return;
  }
  // Written rather than left at the reset default, so the thermocouple type is a stated fact
  // and not an assumption about what the last person plugged in
  if (!hI2cWrite8((uint8_t)hTcAddr, MCP_REG_SENSORCFG, (uint8_t)(((hTcType & 0x07) << 4) | (hTcFilter & 0x07))))
    LOG_WRN("harness: MCP9601 sensor config write failed - type and filter are whatever the part booted with");
  hTcPresent = true;
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
// still enumerated.
//
// NOTHING MAY LIVE ON D6 / GPIO 43, and it is deliberately left unconnected. That pad is
// UART0 TX, and this core builds with CONFIG_ESP_CONSOLE_UART_DEFAULT and
// CONFIG_ESP_CONSOLE_UART_NUM 0, so the ROM, the bootloader and the app all drive it and it
// idles HIGH - which on a line meaning "connected when low" would be a rail held open. A
// pull-down cannot win that: it only decides what a high-impedance pin floats to, and the
// datasheet's I_OH is 40 mA typical against the 0.33 mA a 10k draws. Claiming the pad earlier
// does not fix it either, because prepHarness() returns without touching a pin while
// harnessUse is 0, and even with it set the console owns the pad from reset until setup() -
// the ROM banner, the bootloader, PSRAM, the SD mount and the camera probe, which is long
// enough for a mechanical relay to follow. Leaving it empty also keeps COM3's UART0 panic
// console, which matters on the one board that has no remote reset of its own
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

// Its own task, at low priority. Both devices are on Wire1, a hardware I2C peripheral, so a
// pass is a handful of short bus transactions with no bit banging and no interrupt masking
// anywhere. That was not true of the 1-Wire thermocouple this replaced, which drove its bus
// in software and took a spinlock per bit; moving to an I2C part removed the whole concern.
//
// It keeps a task of its own regardless: off the capture task, whose frame timing is
// measured here to three decimals, and off the httpd worker, where it would otherwise run
// inside a status poll
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
  // Both junctions, because the hot one alone cannot say whether the probe is still bonded -
  // see trap 3. A working probe on a warm sensor sits well above the board's own temperature
  if (hTcPresent) LOG_INF("  OV5640 thermocouple: %.2f C (MCP9601 cold junction %.2f C)", hTcCelsius, hTcColdC);
  // The two supplies are switched independently so a test can isolate one of them: cut USB
  // and the far board runs on battery, cut battery and its whole draw is on the USB channel,
  // cut both and it is a hard power cycle. Name the resulting combination, because that is
  // the thing a reader of this log actually wants to know
  const char* supply = hUsbPowerOn ? (hBattPowerOn ? "USB and battery" : "USB only")
                                   : (hBattPowerOn ? "battery only" : "NOTHING - far board is dead");
  LOG_INF("  supply: %s. USB data %s, lamp %u%%", supply, hUsbDataOn ? "connected" : "isolated", hLampLevel);
}

// Seven of the eight usable pads are spoken for and two of them moved this week, so a pin
// collision is a live risk rather than a theoretical one - and every form of it fails quietly.
// Two harness pins on one net just fight each other. battPin is worse: battMonitor() analogReads
// it and publishes the answer as a battery voltage, so a divider left on a pin the harness now
// drives reports a plausible number that means nothing. PEER_RESET_PIN is worse still, because
// losing it costs the only remote lever this board has over the other one
static void checkHarnessPins() {
  const int pins[] = {hLampPin, hRelayUsbPin, hRelayBattPin, hUsbMuxPin, hSdaPin, hSclPin};
  const char* names[] = {"lamp", "USB relay", "battery relay", "USB mux", "SDA", "SCL"};
  const int count = sizeof(pins) / sizeof(pins[0]);
  for (int i = 0; i < count; i++) {
    if (pins[i] <= 0) continue;
    for (int j = i + 1; j < count; j++)
      if (pins[i] == pins[j]) LOG_WRN("harness: %s and %s are both on GPIO %d", names[i], names[j], pins[i]);
    if (pins[i] == PEER_RESET_PIN)
      LOG_WRN("harness: %s is on GPIO %d, the peer reset line - COM4 loses its only remote reset", names[i], pins[i]);
    if (battUse && pins[i] == battPin)
      LOG_WRN("harness: %s is on GPIO %d, which battUse is also reading as the battery divider", names[i], pins[i]);
    if (pins[i] == 43)
      LOG_WRN("harness: %s is on GPIO 43, which the console drives - see driveSwitch()", names[i]);
  }
}

void prepHarness() {
  if (!harnessUse) {
    LOG_INF("harness: not enabled");
    return;
  }
  checkHarnessPins();
  // Switches first and before anything can fail, so the far board is powered from the
  // earliest possible moment in this module's life
  setUsbRail(true);
  setBattPower(true);
  setUsbData(true);
  if (prepI2c()) {
    prepIna3221();
    prepMcp9601();
  }
  setHarnessLamp(hLampLevel);
  if (xTaskCreate(harnessTask, "harness", HARNESS_STACK_SIZE, NULL, 1, &harnessHandle) != pdPASS) {
    harnessHandle = NULL;
    LOG_WRN("harness: poll task not created - switches work, readings will not update");
  }
  harnessInit = true;
  harnessReport();
}

#endif // INCLUDE_HARNESS
