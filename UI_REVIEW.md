# UI_REVIEW.md - the web UI's camera controls: findings and proposals

Written 5 Sep 2026 from the full control regression (`tools/bench/ui_regress.sh`, run
`ui_regress_full_20260905`: 214 matrix points across HD 30, 1280X960 41 and FHDNARROW 1, plus 59
scenario rows, 3.5 h, the lens held at one focus code throughout). The measurements behind every
claim are in the local bench notebook, BOARD_TESTING §38 (§38.1 source findings, §38.2 the driver
write table, §38.3 the hidden-key audit, §38.5 the run).

**The regression found no firmware regression.** No control disturbed the tuner's clock, window,
HTS/VTS or AEC limits; the sensor's JPEG quality tracked the config at all 214 points; every size
came back to its calibration; the restore diffed clean. What it found instead is a page that
misreports its own state, offers controls that do nothing in the state it is in, and cannot
express some values the firmware holds.

Each item is a proposal with its evidence, for the user to pick from. Numbering is priority order
within each group. After any UI change, `DRY=1` on the regression is the smoke test and the full run
is the gate.

**Status, 5 Sep 2026: A1-A4 and B1-B3 are done and deployed to both boards; C and D are not started,
on the user's instruction to pause before C1.** Of the B items only **B3 was a real defect** - B1 and
B2 are retracted, because I had read the static markup, which carries the OV2640 ranges, where the
page re-ranges every slider for the detected sensor on load. Part of C3 falls to the same error. Both
retractions are written up in place rather than deleted, and the measurements behind them survive as
tooltips on the controls.

The AWB algorithm decision is taken: **simple** (`dcw=0`), changed in `appConfig` and persisted on
both boards, register-verified (0x5183 = 0x94, bit 7 set = simple per the datasheet). The dark-room
comparison is still owed. Two pieces of A4 remain, both firmware rather than page: never persisting
`colorbar` and clearing it at boot.

The page files are deployed to both boards through the `startOTA` gate and served byte-exact.
Firmware is **not** flashed: the `appConfig` default change reaches a board only on the next flash,
which is why both boards were also set live and saved.

How A1-A4 were verified: the page was served from a local stub whose `/status` returned the state
that makes each defect visible (AEC manual, AGC manual, AWB gain off, colour bar on), every value a
string as the firmware sends them, and the stub recorded every `/control` it received. The
committed version was served beside it as the before case. Results:

| check | before | after |
|---|---|---|
| Manual Exposure group with AEC manual | hidden | visible |
| Gain group with AGC manual | hidden | visible |
| Gain Ceiling group with AGC manual | visible | hidden |
| AWB Mode with AWB Gain off | visible and live | visible, disabled, with a reason |
| change sent from the disabled AWB Mode | sent | not sent |
| change sent from AWB Mode once enabled | sent | sent |
| Clear NVS declined at the prompt | no prompt, sent | prompt, nothing sent |
| Clear NVS accepted | - | prompt, `clear=1` sent |
| colour bar banner with the bar on | absent | shown, its button sends `colorbar=0` and clears both |

Unchanged elsewhere: the only state-hidden group in that state is Gain Ceiling, controls on other
panels are not treated as inert, and normal controls still send.

| # | proposal | kind | why now |
|---|---|---|---|
| A1 | Fix the string-truthiness state tests | bug | the page shows the wrong controls after every load |
| A2 | Confirm before Clear NVS | bug | one click wipes wifi credentials and a calibration |
| A3 | Stop hidden controls from writing | bug | a hidden select still moved the AWB gains |
| A4 | Banner while the colour bar is on | bug | a board can boot showing test bars |
| B1 | ~~`ae_level` -5..+5~~ RETRACTED, tooltip added | range | already -5..+5 at runtime; I read the OV2640 markup |
| B2 | ~~`agc_gain` 0..63~~ RETRACTED, tooltip added | range | already 0..63 with 1x-64x labels at runtime |
| B3 | `gainceiling` to the firmware's 1023 | range | **a board at 1023 rendered as 511** - wrong by 2x |
| C1 | Disable manual controls under the automatics | layout | all three measured inert under auto |
| C2 | Show what the automatics are doing | layout | exposure, gain and live quality are invisible |
| C3 | Re-home the AWB algorithm toggle | layout | label is already right; the switch reads as a feature, not a choice |
| C4 | Gate Show Motion on motion detection | layout | firmware refuses it and warns |
| C5 | Group the low-light-only controls | layout | two controls do nothing by daylight |
| D1 | Regroup the camera panel | layout | current order mixes picture, exposure and diagnostics |
| D2 | Clean the duplicate ids and the class typo | hygiene | found by reading, cheap to fix |
| D3 | Move `xclkMhz` out of the picture controls | safety | a clock control beside brightness |

---

## A. Correctness and safety

### A1. The page tests `/status` strings for truthiness, so it shows the wrong controls

`/status` serialises every value as a string. `processStatus` then does

```
else if (key == "aec")      value ? hide($('#aec_value-group')) : showAec();
else if (key == "awb_gain") value ? show($('#wb_mode-group'))   : hide($('#wb_mode-group'));
```

In JavaScript `"0"` is truthy. So on every page load with the AEC in **manual**, the page
**hides** the Manual Exposure slider - exactly the control the user needs - and with AWB Gain
**off** it **shows** the AWB Mode select it means to hide. The from-user path passes a real
boolean and behaves; only the load and refresh path is wrong, which is why this survives casual
use.

Proposal: one helper, used by every state test in `processStatus`. Low risk, no firmware change.

**Done 5 Sep 2026**: `isOn()` in `common.js`, applied at the `aec` and `awb_gain` branches, in
`setAgc` and in `setRecIndicator` - the last was the same bug on the recording indicator, which
would have lit on a `showRecord` of "0".

### A2. Clear NVS is a single click with no confirmation

It wipes the wifi credentials and the `extDVDD` calibration key in the APP_NAME namespace, which
means a board that then reboots is off the network until someone puts a cable on it.

Proposal: a confirm dialog that names what is lost. Optionally require the board to be on USB.

**Done 5 Sep 2026**: the prompt names the WiFi credentials and the calibration keys, and says the
SD card is untouched. Declining sends nothing. The USB requirement was not added.

### A3. A hidden control still writes to the sensor

Measured (§38.5, AWB scenario step G): `wb_mode=2` sent while `awb_gain` is 0 - the state in which
the page hides that select - wrote 0x3406 = 0x01 and moved the AWB gains (R gain 1616). The
handler calls the driver regardless of the toggle the page uses to hide it.

Proposal: prefer `disabled` over `hidden` for state-dependent controls, and do not send a control
whose group is hidden. This is the general form of A1: the page's visibility state and the
firmware's state are two different things today.

**Done 5 Sep 2026**, for this control and as a general guard: AWB Mode is now shown disabled with a
reason in its tooltip rather than hidden (`setWbMode`), and `isInert()` in `common.js` stops the page
sending from any control that is hidden or disabled, itself or through its `input-group`. The
firmware half - the handler ignoring `wb_mode` while `awb_gain` is off - is **not** done, so a
request sent by hand still applies. Whether the other state-dependent groups (Manual Exposure, Gain,
Gain Ceiling) should also become disabled-in-place rather than hidden is C1's question, deliberately
left alone here.

### A4. The colour bar survives a framesize change and can be persisted

Measured: with the bar on, `framesize=25` kept 0x503D set (the still was the pattern: luma 128.1,
hdiff 0.3, ratio 0.996), because the test pattern is not in the register block `set_framesize`
reloads. `colorbar` is also persistable, so a board can boot into test bars and look broken.

Proposal: never persist `colorbar`, clear it on boot, and show a banner while it is set. Same
treatment for `special_effect` if the user wants it (that one at least looks deliberate).

**Partly done 5 Sep 2026**: the banner is in, driven by `/status` so it also catches a bar left on
by an earlier session or carried through a reboot, and its button clears the bar in one click from
anywhere in the page. **Not done**: never persisting `colorbar` and clearing it at boot, both of
which are firmware changes needing a build and a flash. The page deliberately does not write
`colorbar=0` by itself on load - it reports and offers, it does not change the camera unasked.

---

## B. Ranges the page cannot express

### B1. RETRACTED as a range bug - `ae_level` is already -5..+5 on this sensor

**I got this wrong, and B2 and part of C3 with it.** The claim came from the static HTML, which
carries the OV2640 values; on load the page re-ranges every slider for the detected sensor
(`cameraModel == "OV3660" || cameraModel == "OV5640"` branch). Verified on the board 5 Sep 2026: the
Exposure Level slider is **-5..+5 showing -2**, so the range and the round-trip were never broken.

What was actually missing is the meaning, and that is now in the control's tooltip: it biases the
AEC's target rather than the picture, and its extremes are unusable. Measured at HD 30:

| value | exposure | gain | luma | note |
|---|---|---|---|---|
| -5 | 381 lines | 19/16 | 15.5 | unusable, and the colour collapses (ratio 3.32) |
| +5 | 1252 lines | 224/16 = 14x | 187.3 | blown, the AEC pays in gain |

This is the strongest exposure lever the page exposes and the least explained.

**Done 5 Sep 2026**: a tooltip stating what it biases and what its extremes cost. No range change,
because none was needed.

### B2. RETRACTED - `agc_gain` is already 0..63 with 1x..64x labels

Same mistake as B1: the 0..30 in the markup is the OV2640 branch. On the board the slider is
**0..63** and `changeRange`'s `isX` flag already prints `1x` and `64x` at its ends.

The measurement stands and is worth knowing, so it is now in the control's tooltip: at HD 30 with
AGC off, 0 gives register 0 and luma 59.0 while **1 gives 15 and luma 59.3** - the same 1x gain - 30
gives 479 and luma 212.6, and 63 gives 1007 and luma 230.0, washed out. The earlier worry that 0
means a black frame is wrong; the floor is 1x.

**Done 5 Sep 2026**: tooltip only.

### B3. `gainceiling` could not show the value the boards carry - CONFIRMED and fixed

The one real item of the three. The runtime range for this sensor was **0..511**, i.e. 32x, while
both boards persist 1023 (the datasheet's 64x, set 4 Sep 2026). A board at 1023 therefore rendered as
**511** on the slider: not merely unrepresentable, actively wrong by a factor of two, and any drag of
that slider would have halved the board's ceiling.

Also measured: 0, 511 and 1023 all produced luma 105.7 and gain 40/16 at HD 30, because the AEC never
approaches the ceiling at a 32 ms frame. The control is inert by daylight (see C5).

**Done 5 Sep 2026**: the range is 0..1023 for the OV5640 (OV3660 keeps 511, whose ceiling this bench
has not measured), the end labels read `1x` and `64x`, and a tooltip gives the conversion,
`(value + 1) / 16` x, and says it only acts in low light. Verified against the board: the slider spans
0..1023, shows 1023, and round-trips `/status`.

**The lesson for the rest of this document**: read the sensor branch, not the markup. Static HTML in
this page is OV2640's and is overwritten on load for anything the OV5640 branch touches - ranges and
the `aec2`, `awb_gain` and `dcw` labels among them.

---

## C. Smart controls: hide or disable what cannot act

### C1. The manual controls are measurably inert under the automatics

At HD 30 with AEC, AGC and AWB auto:

| control | values sent | result |
|---|---|---|
| `aec_value` | 0 and 1252 | exposure stayed 1252 lines, luma ~108.7 |
| `agc_gain` | 0 and 63 | gain stayed 40-42/16, luma ~107 |
| `awb_gain` | 0 and 1 | luma ~104.8, no colour change |

This is the user's own example and the evidence supports it: Manual Exposure should be disabled
while AEC is auto, Gain while AGC is auto, and AWB Mode plus AWB Gain while AWB is auto. Disabled
with a reason in the title, not hidden, so the panel does not jump (and see A3).

### C2. Show what the automatics chose, and what the sensor is actually doing

Today the page shows the config's quality but never the sensor's. After the no-frame rescue steps
the sensor's quality (sticky), 0x4407 and the config disagree and nothing on the page says so.
Likewise the settled exposure and gain are the two numbers that explain every "why is it dark"
question, and they are already in `/status` and `updateFPS`.

Proposal: a read-only line under the exposure group - exposure in lines and ms, gain as a
multiplier, the sensor's live quality when it differs from the config, and the AWB gains.

### C3. The AWB algorithm toggle reads "Advanced AWB" - correct, but its sense is easy to misread

**Correction, 5 Sep 2026**: the "DCW (Downsize)" label I reported is the OV2640 branch. On this
sensor the page already relabels it **"Advanced AWB"** (and `aec2` to "Night Mode", `awb_gain` to
"Manual AWB"), which is accurate. The rename is therefore mostly unnecessary; what remains is that
the control is a two-way choice presented as a switch, so "off" reads as a feature disabled rather
than as simple white balance selected - and simple is now the default, so the page shows the chosen
algorithm as an unchecked box.

`dcw` writes 0x5183[7], which selects simple against advanced AWB. Measured on the star chart at
QSXGA:

| state | chart R/G | chart B/G |
|---|---|---|
| simple (`dcw=0`) | 0.965 | 0.918 |
| advanced (`dcw=1`, today's default) | 0.882 | 0.882 |

Simple AWB was the most neutral state measured, and the same ordering appears in the HD matrix.
That is a lead on the standing green-cast question, not a settled answer - it wants the user's
eyeball on the two stills and a dark-room check before any default changes.

Proposal: rename to "AWB algorithm: simple / advanced", move it into the AWB group next to Mode.

**The default is now simple** (user's decision, 5 Sep 2026): `appConfig` carries `dcw~0` and both
boards were set live and saved, register-verified at 0x5183 = 0x94 (bit 7 set, which the datasheet
defines as simple; the driver's flag is inverted, so `dcw=0` means simple). **The dark-room
comparison is still owed.** That makes the rename more urgent, not less: the page now shows a
control labelled "DCW (Downsize)" sitting off, which reads as a downsizing feature being disabled
rather than as simple white balance being chosen. The rename itself is still C3, not done here.

While there: the AWB checkbox is not a hold. With `awb=0` the gain registers keep their converged
values yet the chart collapses to 0.46 R/G, because clearing 0x5001 bit 0 stops the ISP applying
them. Label it "AWB (off = raw colour)" or similar, because "off" today reads as "freeze".

### C4. Show Motion is offered while motion detection is off

The firmware refuses it and logs "Show Motion needs motion detection enabled". The page also has
no way to reflect that, because `/status` carries no `dbgMotion` field at all.

Proposal: disable the control unless `enableMotion` is on, and add `dbgMotion` to `/status` so the
page and the bench can read it back.

### C5. Two controls do nothing by daylight

`gainceiling` (0, 511, 1023 all identical at HD 30) and `lswitch` (0, 10, 100 all identical) only
act in low light. A user who tries them in a lit room concludes the page is broken.

Proposal: a "Low light" group holding gain ceiling, night switch and AEC2, with a note that they
act only when the AEC is near its ceiling.

---

## D. Layout and hygiene

### D1. Regroup the camera panel

Suggested grouping, in the order a user reaches for them:

1. **Picture** - brightness, contrast, saturation, sharpness, denoise, special effect
2. **Exposure** - AEC, exposure level, manual exposure, AEC2
3. **Colour** - AWB, algorithm, mode, AWB gain
4. **Low light** - gain ceiling, night switch
5. **Geometry** - mirror, flip
6. **Diagnostics** - colour bar, show motion, and the clock control of D3

### D2. Duplicate element ids and the `.menu-pinnded` class typo

Found by reading the page (§38.1). The duplicate ids make `$('#id')` ambiguous.

### D3. `xclkMhz` sits among the picture controls

It is a clock control. The regression treats it as audit-only and never sends it, on the user's
instruction, because a wrong value there is not a picture problem. The other hidden keys
(`tunedFps`, `banding`, `idleFps`, `lencFhd`, `sdBusDiv`) are not on the page at all, which is the
right place for them.

Proposal: move it behind an advanced or diagnostics disclosure with its own confirmation.

---

## What the regression proved, for the record

Worth keeping in view while judging the proposals above: every control that should have been
harmless was harmless. The AWB enable does a read-modify-write that preserves the tuner's scaler
bit; mirror and flip re-assert the subsample and binning bytes without disturbing the window;
special effect does not clobber the brightness registers; sharpness at quality 6 stayed inside the
frame window at both QSXGA and FHDNARROW with no rescue; mid-recording rate and size changes were
deferred to the clip's close. The page's problem is that it describes the camera badly, not that
it drives it badly.
