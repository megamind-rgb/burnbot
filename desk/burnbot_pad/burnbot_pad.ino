// Burnbot on the Claude Pad: an LCDwiki ES3C28P (ESP32-S3, 2.8" ILI9341V, 320 x 240 on its
// side) in a case with four keys under the screen, a touch screen, a microphone and a speaker.
//
//   Upload:  desk/board.sh pad   (it copies render.h and pad.h in from burnbot_screen, and the
//                                 codec, touch and sound drivers from pad_hw)
//
// The Mac sends one line a second saying which level you are on, and this draws it (pad.h):
// the words at the top, the robot standing in its fire, and over each key what it does now.
// Between lines the board keeps the scene moving by itself. Only USB: the pad's Wi-Fi stays off.
//
// The keys, left to right, wired to GND (the kit's guide): 1 GPIO2, 2 GPIO3, 3 GPIO14, 4 GPIO21.
// Three pages, with dots at the bottom: Home (the robot), Usage (U line) and Today (D line).
// With no labels on the screen, key 1 and key 4 (or a swipe) step back and forth between them
// right here, with nothing sent; keys 2 and 3 go to the Mac. Two minutes after the last step, or
// whenever the Mac shows something, it is back on Home.
// When the Mac labels the keys (the B line's ky=, or the S line's), they do what the labels say:
// the Mac decides, and a tap on a label counts as that key, as does a tap on a question's answer.
//
// Lines from the Mac (115200, each ends with a new line):
//   V        answers "BURNBOT 6" (6: pages; 5 talks, listens and takes touch; 4 had keys)
//   P        answers "OK"
//   WF|..    Wi-Fi and pairing, WI its status (wifilink.h); USB only, like DL
//   DL       restarts into flashing mode (the chip's own USB port), for updates without the BOOT button
//   R        starts the display again, answers "OK"
//   O0 / O2  answers "OK" (the keys are fixed under the screen, so it stays the right way up)
//   B|...    what to show (render.h, and pad.h for ky=, pr=, pp=, wk=), answers "OK"
//   U|...    the Usage page (pad.h: h=46|hr=IN 2H 10M|w=62|wr=TUE 09:00|x=28|xn=SHOP|ag=3), "OK"
//   D|...    the Today page (pad.h: b=24 bars 0-99|fm=4H 20M|ss=8|pk=3.2X), answers "OK"
//   M|...    what the robot is saying (render.h), answers "OK"
//   S|...    a whole screen (pad.h: a question, your words to check, a reply...), answers "OK".
//            With mc=1 in it, key 2 records while held.
//   S0/S1/S2 answers "OK" (no knocks here)
//   L0..L9   how bright, 0 dim to 9 full, answers "OK"; remembered
//   VL|0-100 how loud the speaker is, answers "OK"; remembered
//   MG|0-7   how much the microphone is turned up, answers "OK"; remembered
//   BZ|n     n short beeps, answers "OK"
//   PL|seq|base64  30 ms of sound for the speaker (16 kHz, 16-bit, one channel): answers PK|
//   PE / PX  the end of the sound (play the rest) / stop it now: both answer PD| when it is over
//   RX       throw away the recording being made
//   TC       touch setup: tap five crosses (also: hold key 3 while the pad starts)
//   Q        answers what it is showing and what it has, for example
//            "Q|o=0|s=4|w=0|k=0|n=1|y=4|t=1|a=1|c=1|b=4150|v=60"
//            (y=4 four keys; t touch, a sound, c calibrated, b battery mV, v volume)
//   s        a line about the display, keys, touch and sound, for desk/board.sh listen
//
// And to the Mac:
//   K1d / K1u          key 1 down / up (K2.. K4 the same), from a key or a tap
//   AS|id              recording (key 2 held, on a screen that has mc=1)
//   A|id|seq|base64    20 ms of it
//   AE|id|frames|dropped|peak|why   the end: up (let go), max (45 s), cancel (key 1), lost (the
//                      Mac went quiet). Keys used for recording send no K lines.
//   PK|seq|ms buffered, PD|done / stopped / starved   the speaker
//   BT|mV              the battery, every 30 s when it changes
//   TC|ok|px / TC|fail|px / TC|cancel   touch setup's result
//
// With nothing from the Mac for 15 seconds the robot goes to sleep and the screen says it is
// waiting; after ten minutes of that the light goes out until the Mac is back or a key is pressed.
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <Preferences.h>
#include "soc/rtc_cntl_reg.h"

#include "pad.h"
#include "es8311.h"
#include "ft6336.h"
#include "padaudio.h"
#include "calib.h"
#include "wifilink.h"

// The pad's USB drive "BURNBOT": the Mac and Windows installers (drive.h, made by desk/tools/make_drive.py),
// read-only. Needs USB in OTG mode (USBMode=default), which board.sh's PAD_FQBN sets.
#if ARDUINO_USB_MODE == 0
#include "USB.h"
#include "USBMSC.h"
#include "drive.h"
USBMSC drive;
static int32_t driveRead(uint32_t lba, uint32_t offset, void *buffer, uint32_t size) {
  uint32_t at = lba * 512 + offset, n = at < DRIVE_USED ? min(size, DRIVE_USED - at) : 0;
  memcpy(buffer, DRIVE + at, n);
  memset((uint8_t *)buffer + n, 0, size - n);
  return size;
}
static int32_t driveWrite(uint32_t, uint32_t, uint8_t *, uint32_t) { return -1; }
// Before USB starts (the core starts it before setup()), so the drive is in the USB descriptor.
static struct DriveAtBoot {
  DriveAtBoot() {
    drive.vendorID("Burnbot");
    drive.productID("Pad installers");
    drive.productRevision("6");
    drive.onRead(driveRead);
    drive.onWrite(driveWrite);
    drive.isWritable(false);
    drive.mediaPresent(true);
    drive.begin(DRIVE_SECTORS, 512);
  }
} driveAtBoot;
#endif

// The board's wiring (lcdwiki.com, 2.8inch ESP32-S3 Display)
const int LCD_CS = 10, LCD_DC = 46, LCD_SCK = 12, LCD_MOSI = 11, LCD_MISO = 13, LCD_BL = 45;
const int I2C_SDA = 16, I2C_SCL = 15;       // the codec (0x18) and the touch chip (0x38)
const int BATTERY = 9;                      // through a 1:2 divider
const int KEYS[4] = {2, 3, 14, 21};

const uint32_t SPI_HZ = 26000000;
const uint32_t QUIET_MS = 15000;
const uint32_t DARK_MS = 10UL * 60 * 1000;
const uint32_t FRAME_MS = 66;               // 15 frames a second
const uint32_t DEBOUNCE_MS = 25;
const uint32_t TOUCH_MS = 16;
const uint32_t LONGEST_TALK_MS = 45000;
const int BAND = 16;                        // rows sent at a time

Adafruit_ILI9341 tft(&SPI, LCD_DC, LCD_CS, -1);
Preferences kept;

bb::State state;
bb::Moment moment;
pd::Pad pad;
pd::Screen screen;                          // the last S line
uint32_t screenAt = 0;                      // when it came (0: never); older than QUIET_MS counts as none
pd::Live live;
pd::Hits hits;
bb::Scene scene(pd::AW, pd::AH, pd::LIFT);
static uint16_t art[pd::AW * pd::AH];
uint16_t *frame = nullptr, *shown = nullptr;   // a whole screen each, in PSRAM

bool waiting = true, lit = true, allStale = true;
uint8_t brightness = 9;
int volume = 60, gain = 6;
uint32_t heardAt = 0, frameAt = 0;

bool codecOk = false, touchOk = false, audioOk = false, calibrated = false;
float affine[6];

char line[2048];
size_t lineLength = 0;

bool fresh(uint32_t now) { return screenAt && now - screenAt < QUIET_MS; }

// The pages: what Usage and Today show, and which page is up.
pd::Usage usage;
pd::Today today;
int page = pd::HOME;
uint32_t pageAt = 0;                        // the last step between pages
const uint32_t PAGE_MS = 2UL * 60 * 1000;   // then back to Home

// True when the pages are up: nothing labels the keys, nothing else is on the screen.
bool onPages(uint32_t now) {
  if (waiting || pad.labelled() || pad.ask[0]) return false;
  if (fresh(now) && screen.mode[0] && strcmp(screen.mode, "home")) return false;
  return live.calStep < 0 && !au::recording;
}

void turnPage(int by, uint32_t now) {
  page = (page + by + pd::PAGES) % pd::PAGES;
  pageAt = now;
}
bool micOn(uint32_t now) { return audioOk && codecOk && !waiting && fresh(now) && screen.mic; }

// ---------- kept across restarts ----------
void remember() {
  kept.begin("burnbot", false);
  kept.putUChar("bright", brightness);
  kept.putUChar("vol", (uint8_t)volume);
  kept.putUChar("gain", (uint8_t)gain);
  if (calibrated) kept.putBytes("tcal", affine, sizeof affine);
  kept.end();
}

void recall() {
  kept.begin("burnbot", true);
  brightness = kept.getUChar("bright", 9);
  if (brightness > 9) brightness = 9;
  volume = kept.getUChar("vol", 60);
  if (volume > 100) volume = 100;
  gain = kept.getUChar("gain", 6);
  if (gain > 7) gain = 7;
  calibrated = kept.getBytes("tcal", affine, sizeof affine) == sizeof affine;
  if (!calibrated) memcpy(affine, cal::DEFAULT_AFFINE, sizeof affine);
  kept.end();
}

// ---------- the display ----------
void light(bool on) {
  lit = on;
  analogWrite(LCD_BL, on ? 40 + brightness * 215 / 9 : 0);
}

void wake() {
  heardAt = millis();
  light(true);
}

void startDisplay() {
  SPI.begin(LCD_SCK, LCD_MISO, LCD_MOSI, LCD_CS);
  tft.begin(SPI_HZ);
  tft.setRotation(1);                       // on its side, the USB socket to the right
  tft.invertDisplay(true);                  // this ILI9341V panel shows colours inverted otherwise
  tft.fillScreen(ILI9341_BLACK);
  if (shown) memset(shown, 0, pd::W * pd::H * sizeof(uint16_t));
  allStale = true;
}

// Sends the rows that differ from what the display has, BAND rows at a time. Between bands it
// passes on the microphone, so a long redraw does not hold recording up.
void show() {
  const size_t rowBytes = pd::W * sizeof(uint16_t);
  int y = 0;
  tft.startWrite();
  while (y < pd::H) {
    if (!allStale && !memcmp(frame + y * pd::W, shown + y * pd::W, rowBytes)) {
      y++;
      continue;
    }
    int end = y;
    while (end < pd::H && end - y < BAND && (allStale || memcmp(frame + end * pd::W, shown + end * pd::W, rowBytes))) end++;
    // Copied into a strip in the chip's own memory, high byte first, and sent as plain bytes.
    static uint16_t strip[pd::W * BAND];
    const uint32_t n = (uint32_t)pd::W * (end - y);
    const uint16_t *from = frame + y * pd::W;
    for (uint32_t i = 0; i < n; i++) strip[i] = (uint16_t)((from[i] << 8) | (from[i] >> 8));
    tft.setAddrWindow(0, y, pd::W, end - y);
    tft.writePixels(strip, n, true, true);   // big-endian already: goes out through writeBytes
    memcpy(shown + y * pd::W, frame + y * pd::W, rowBytes * (end - y));
    y = end;
    if (au::recording) au::sendMic(computer, 2);
    wl::pump();                             // keep Wi-Fi sound coming in while the frame goes out
  }
  tft.endWrite();
  allStale = false;
}

// ---------- touch setup ----------
int calRaw[cal::N][2];
int calSum[2] = {0, 0}, calSamples = 0;     // the finger's raw position, averaged while it is down
int calLast[2] = {-999, -999};
uint32_t calDoneAt = 0;

void startCalibration() {
  live.calStep = 0;
  live.note[0] = 0;
  memcpy(live.targets, cal::TARGET, sizeof live.targets);
  calSamples = calSum[0] = calSum[1] = 0;
  calLast[0] = calLast[1] = -999;
  calDoneAt = 0;
  if (au::recording) au::cancelRecording(computer);
  if (!lit) light(true);
}

void endCalibration(const char *result) {
  live.calStep = -1;
  live.note[0] = 0;
  calDoneAt = 0;
  if (result) computer.println(result);
}

// A finger lifted at raw (rx, ry) during touch setup.
void calibrationTap(int rx, int ry) {
  if (live.calStep < 0 || live.calStep >= cal::N) return;
  if (abs(rx - calLast[0]) < 30 && abs(ry - calLast[1]) < 30) return;   // the same spot again
  calLast[0] = rx;
  calLast[1] = ry;
  calRaw[live.calStep][0] = rx;
  calRaw[live.calStep][1] = ry;
  if (audioOk) au::tone(1800, 40, 20);
  live.note[0] = 0;
  if (++live.calStep < cal::N) return;
  float m[6], worst = 0;
  if (cal::fit(calRaw, m, worst)) {
    memcpy(affine, m, sizeof affine);
    calibrated = true;
    remember();
    snprintf(live.note, sizeof live.note, "SAVED. OFF BY ABOUT %d PX", (int)(worst + 0.5f));
    calDoneAt = millis();                   // shows "done" for a moment, then back
    computer.printf("TC|ok|%d\n", (int)(worst + 0.5f));
  } else {
    snprintf(live.note, sizeof live.note, "TRY AGAIN");
    computer.printf("TC|fail|%d\n", (int)(worst + 0.5f));
    live.calStep = 0;
    calLast[0] = calLast[1] = -999;
  }
}

// ---------- the keys, and taps that count as keys ----------
bool keyDown[4];                            // the keys themselves, debounced
bool touchDown[4];                          // a finger on a key's label
uint32_t keyChangedAt[4];
uint32_t presses[4];
bool eaten[4];                              // this press went to waking, recording or touch setup: no K up either
uint32_t recCount = 0;

void startKeys() {
  for (int i = 0; i < 4; i++) {
    pinMode(KEYS[i], INPUT_PULLUP);
    keyDown[i] = digitalRead(KEYS[i]) == LOW;
    touchDown[i] = false;
    keyChangedAt[i] = 0;
    presses[i] = 0;
    eaten[i] = false;
  }
}

// Key i (0 to 3) went down or up, from the key or a tap.
void keyEvent(int i, bool down, uint32_t now) {
  if (!down && eaten[i]) {                  // the end of a press that was used up already
    eaten[i] = false;
    if (i == 1 && au::recording) au::stopRecording(computer, "up");
    return;
  }
  if (!down) {
    computer.printf("K%du\n", i + 1);
    return;
  }
  presses[i]++;
  eaten[i] = true;
  if (!lit) {                               // the first press only wakes the screen
    wake();
    return;
  }
  if (live.calStep >= 0) {                  // touch setup: key 1 cancels, the rest wait
    if (i == 0) endCalibration("TC|cancel");
    return;
  }
  if (au::recording) {                      // key 1 throws the recording away; the others wait
    if (i == 0) au::cancelRecording(computer);
    return;
  }
  if ((i == 0 || i == 3) && onPages(now)) { // key 1 / 4 with no labels: the previous / next page
    turnPage(i == 0 ? -1 : 1, now);
    return;
  }
  if (i == 1 && micOn(now)) {               // key 2 on a screen that listens
    au::startRecording(computer, ++recCount);
    live.seconds = 0;
    memset(live.level, 0, sizeof live.level);
    return;
  }
  eaten[i] = false;
  computer.printf("K%dd\n", i + 1);
}

// True when a key changed, so the frame shows it at once.
bool checkKeys(uint32_t now) {
  bool changed = false;
  for (int i = 0; i < 4; i++) {
    bool down = digitalRead(KEYS[i]) == LOW;
    if (down == keyDown[i] || now - keyChangedAt[i] < DEBOUNCE_MS) continue;
    keyDown[i] = down;
    keyChangedAt[i] = now;
    changed = true;
    if (touchDown[i]) continue;             // already down by a tap: the key adds nothing
    keyEvent(i, down, now);
  }
  return changed;
}

// The touch screen, read every TOUCH_MS. True when what it shows changed.
bool fingerWas = false;
int fingerKey = 0;                          // the key the finger went down on (1 to 4), or 0
int fromX = 0, fromY = 0, toX = 0, toY = 0;  // where the finger went down, and where it was last
uint32_t touchAt = 0;

bool checkTouch(uint32_t now) {
  if (!touchOk || now - touchAt < TOUCH_MS || now < 800) return false;   // 800: let it settle after starting
  touchAt = now;
  int rx = 0, ry = 0;
  bool finger = false;
  if (!ft::read(rx, ry, finger)) return false;
  bool changed = false;
  if (live.calStep >= 0) {
    if (finger) {
      calSum[0] += rx;
      calSum[1] += ry;
      calSamples++;
    } else if (fingerWas && calSamples) {
      calibrationTap(calSum[0] / calSamples, calSum[1] / calSamples);
      changed = true;
    }
    if (!finger) calSamples = calSum[0] = calSum[1] = 0;
    fingerWas = finger;
    return changed;
  }
  int sx = 0, sy = 0;
  if (finger) {
    cal::apply(affine, rx, ry, sx, sy);
    toX = sx;
    toY = sy;
  }
  if (finger && !fingerWas) {
    fromX = sx;
    fromY = sy;
    if (!lit) {
      wake();
    } else {
      int k = hits.at(sx, sy);
      if (k && !keyDown[k - 1]) {
        fingerKey = k;
        touchDown[k - 1] = true;
        keyEvent(k - 1, true, now);
        changed = true;
      }
    }
  } else if (!finger && fingerWas && fingerKey) {
    int k = fingerKey;
    fingerKey = 0;
    touchDown[k - 1] = false;
    if (!keyDown[k - 1]) keyEvent(k - 1, false, now);
    changed = true;
  } else if (!finger && fingerWas && lit && onPages(now)) {
    int dx = toX - fromX, dy = toY - fromY;   // a swipe: well sideways, and more across than down
    if (abs(dx) > 50 && abs(dx) * 2 > abs(dy) * 3) {
      turnPage(dx < 0 ? 1 : -1, now);        // to the left: the next page
      changed = true;
    }
  }
  fingerWas = finger;
  return changed;
}

// ---------- sound ----------
int beepsLeft = 0;
uint32_t beepAt = 0;
uint32_t levelAt = 0;
int levelMax = 0;

void tickSound(uint32_t now) {
  if (!audioOk) return;
  au::poll(computer);
  if (beepsLeft && now >= beepAt) {
    au::tone(2000, 70, 25);
    beepsLeft--;
    beepAt = now + 190;
  }
  live.recording = au::recording;
  if (!au::recording) return;
  au::sendMic(computer, 8);
  int l = au::level;
  if (l > levelMax) levelMax = l;
  if (now - levelAt >= 100) {               // one bar of the meter each 100 ms
    levelAt = now;
    memmove(live.level, live.level + 1, sizeof live.level - 1);
    int v = levelMax * 100 / 20000;         // speech at arm's length peaks near 24000
    live.level[sizeof live.level - 1] = (uint8_t)(v > 100 ? 100 : v);
    levelMax = 0;
  }
  live.seconds = (int)((now - au::recStarted) / 1000);
  if (now - au::recStarted > LONGEST_TALK_MS) {
    au::stopRecording(computer, "max");
    // key 2 is still held: its release should send nothing, which eaten[1] already sees to
  }
}

// ---------- the battery ----------
uint32_t batteryAt = 0;
int batteryMv = 0, batterySent = 0;

void tickBattery(uint32_t now) {
  if (batteryAt && now - batteryAt < 30000) return;
  batteryAt = now ? now : 1;
  uint32_t sum = 0;
  for (int i = 0; i < 16; i++) sum += analogReadMilliVolts(BATTERY);
  batteryMv = (int)(sum / 16 * 2);
  if (abs(batteryMv - batterySent) > 25) {
    batterySent = batteryMv;
    computer.printf("BT|%d\n", batteryMv);
  }
}

// ---------- the Mac's lines ----------
void status() {
  // No reading from the display here: its ID query (command 0xD9) leaves this panel drawing
  // every row along its top edge until it is set up again.
  computer.printf("display: ILI9341, psram %u, keys:", (unsigned)ESP.getPsramSize());
  for (int i = 0; i < 4; i++) computer.printf(" %d=%s(%lu)", i + 1, keyDown[i] ? "down" : "up", (unsigned long)presses[i]);
  computer.printf(", codec %s, touch %s%s, sound %s, vol %d, gain %d, battery %d mV\n", codecOk ? "ok" : "none",
                touchOk ? "ok" : "none", calibrated ? " (calibrated)" : "", audioOk ? "ok" : "none", volume, gain,
                batteryMv);
}

void handle(char *text) {
  if (!text[0]) return;
  const size_t len = strlen(text);
  if (!strcmp(text, "V")) { computer.println("BURNBOT 6"); return; }
  if (!strcmp(text, "P")) { computer.println("OK"); return; }
  if (!strncmp(text, "WF|", 3) || !strcmp(text, "DL")) {   // USB only: pairing and flashing
    if (wl::active) { computer.println("ERR"); return; }
  }
  if (!strncmp(text, "WF|", 3)) { wl::pair(text + 3, computer); return; }
  if (!strcmp(text, "WI")) { wl::status(computer); return; }
  if (!strcmp(text, "DL")) {                 // restart into the chip's own flashing mode: updates need no BOOT button
    computer.println("OK");
    Serial.flush();
    delay(100);
    REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    esp_restart();
  }
  if (!strcmp(text, "s")) { status(); return; }
  if (!strcmp(text, "R")) { startDisplay(); light(lit); computer.println("OK"); return; }
  if (!strcmp(text, "Q")) {
    computer.printf("Q|o=0|s=%d|w=%d|k=0|n=1|y=4|t=%d|a=%d|c=%d|b=%d|v=%d\n", (int)state.look, waiting ? 1 : 0,
                  touchOk ? 1 : 0, audioOk && codecOk ? 1 : 0, calibrated ? 1 : 0, batteryMv, volume);
    return;
  }
  if (audioOk && au::playLine(text, computer)) return;          // PL|, PE, PX
  if (!strcmp(text, "PE") || !strcmp(text, "PX")) { computer.println("PD|done"); return; }   // no sound here
  if (!strncmp(text, "PL|", 3)) return;
  if (!strcmp(text, "RX")) { au::cancelRecording(computer); computer.println("OK"); return; }
  if (!strcmp(text, "TC")) {
    if (!touchOk) { computer.println("TC|fail|0"); return; }
    startCalibration();
    computer.println("OK");
    return;
  }
  if (!strncmp(text, "S|", 2)) {
    pd::Screen next;
    if (!pd::parseScreen(text, next)) { computer.println("ERR"); return; }
    screen = next;
    screenAt = millis();
    if (!screen.mic && au::recording) au::cancelRecording(computer);
    computer.println("OK");
    return;
  }
  if (!strncmp(text, "VL|", 3)) {
    volume = constrain(atoi(text + 3), 0, 100);
    if (codecOk) es::volume(volume);
    remember();
    computer.println("OK");
    return;
  }
  if (!strncmp(text, "MG|", 3)) {
    gain = constrain(atoi(text + 3), 0, 7);
    if (codecOk) es::micGain(gain);
    remember();
    computer.println("OK");
    return;
  }
  if (!strncmp(text, "BZ|", 3)) {
    beepsLeft = constrain(atoi(text + 3), 0, 5);
    beepAt = millis();
    computer.println("OK");
    return;
  }
  if (len == 2 && ((text[0] == 'O' && (text[1] == '0' || text[1] == '2')) || (text[0] == 'S' && text[1] >= '0' && text[1] <= '2'))) {
    computer.println("OK");
    return;
  }
  if (len == 2 && text[0] == 'L' && text[1] >= '0' && text[1] <= '9') {
    brightness = text[1] - '0';
    remember();
    if (lit) light(true);
    computer.println("OK");
    return;
  }
  if (!strncmp(text, "U|", 2)) {
    pd::Usage next;
    if (!pd::parseUsage(text, next)) { computer.println("ERR"); return; }
    usage = next;
    computer.println("OK");
    return;
  }
  if (!strncmp(text, "D|", 2)) {
    pd::Today next;
    if (!pd::parseToday(text, next)) { computer.println("ERR"); return; }
    today = next;
    computer.println("OK");
    return;
  }
  if (!strncmp(text, "M|", 2)) {
    bb::Moment said;
    if (!bb::parseMoment(text, said)) { computer.println("ERR"); return; }
    moment = said;
    computer.println("OK");
    return;
  }
  if (!strncmp(text, "B|", 2)) {
    bb::State next;
    pd::Pad keys;
    if (!bb::parse(text, next) || !pd::parsePad(text, keys)) { computer.println("ERR"); return; }
    state = next;
    pad = keys;
    heardAt = millis();
    waiting = false;
    if (!lit) light(true);
    computer.println("OK");
    return;
  }
  computer.println("ERR");
}

void readMac() {
  static uint8_t buf[512];
  size_t n;
  while ((n = Serial.available()) > 0) {
    wl::active = false;                     // the computer is on USB: answer there
    n = Serial.read(buf, n < sizeof buf ? n : sizeof buf);
    for (size_t i = 0; i < n; i++) {
      char c = (char)buf[i];
      if (c == '\n' || c == '\r') {
        line[lineLength] = 0;
        handle(line);
        lineLength = 0;
      } else if (lineLength < sizeof(line) - 1) {
        line[lineLength++] = c;
      }
    }
  }
}

void setup() {
  Serial.setRxBufferSize(16384);            // sound comes in at about 43 KB a second
#if ARDUINO_USB_MODE == 1
  Serial.setTxBufferSize(16384);            // and goes out at about the same (HW CDC only)
#endif
  Serial.setTxTimeoutMs(0);                 // with nobody listening, drop what it says rather than wait
#if ARDUINO_USB_MODE == 0
  // OTG mode: TinyUSB's send buffer is 64 bytes, so a sound line (~870) needs to wait for the computer to read.
  // Safe: with no program holding the port open (DTR off), the core sends nothing and does not wait.
  Serial.setTxTimeoutMs(100);
#endif
  Serial.begin(115200);
  pinMode(LCD_BL, OUTPUT);
  startKeys();
  recall();
  frame = (uint16_t *)ps_malloc(pd::W * pd::H * sizeof(uint16_t));
  shown = (uint16_t *)ps_malloc(pd::W * pd::H * sizeof(uint16_t));
  if (!frame || !shown) {                   // no PSRAM: should not happen on this board
    frame = (uint16_t *)malloc(pd::W * pd::H * sizeof(uint16_t));
    shown = (uint16_t *)malloc(pd::W * pd::H * sizeof(uint16_t));
  }
  pd::homeScene(scene);                     // the big robot, alone, for Home
  delay(300);                               // the screen shares the chip's reset line: let it wake
  startDisplay();
  light(true);

  // Sound first: the codec needs the I2S clock running before it is set up.
  Wire.begin(I2C_SDA, I2C_SCL, 100000);
  String why;
  audioOk = au::begin(why);
  codecOk = es::begin(Wire, 0x18, volume, gain);
  touchOk = ft::begin(Wire, ft::resetPin, ft::intPin);
  if (!audioOk) computer.printf("sound: %s\n", why.c_str());
  tickBattery(millis());
  if (touchOk && keyDown[2]) {              // key 3 held while it starts: touch setup
    startCalibration();
    eaten[2] = true;
  }
  status();
  wl::load();
}

void loop() {
  readMac();
  wl::poll(millis());                      // before `now`: a line stamps heardAt, which must not be later than now
  uint32_t now = millis();
  bool pressed = checkKeys(now);
  pressed |= checkTouch(now);
  tickSound(now);
  wl::awake(au::recording || au::playing || (au::fedAt && now - au::fedAt < 1500));   // sound needs the radio awake
  tickBattery(now);

  if (!waiting && now - heardAt > QUIET_MS) {   // the Mac went quiet (asleep, or Burnbot quit)
    waiting = true;
    moment = bb::Moment();
    pad = pd::Pad();
    screenAt = 0;
    if (au::recording) au::stopRecording(computer, "lost");
    if (audioOk) au::stopPlaying();
  }
  if (calDoneAt && now - calDoneAt > 2500) endCalibration(nullptr);
  if (live.calStep == cal::N && !calDoneAt) endCalibration(nullptr);
  if (lit && waiting && live.calStep < 0 && now - heardAt > DARK_MS) light(false);
  if (page != pd::HOME && (!onPages(now) || now - pageAt > PAGE_MS)) {
    page = pd::HOME;
    pressed = true;
  }

  const uint32_t every = au::recording ? 100 : FRAME_MS;   // while listening only the meter moves
  if (lit && frame && shown && (pressed || now - frameAt >= every)) {
    float dt = frameAt ? (now - frameAt) / 1000.0f : FRAME_MS / 1000.0f;
    if (dt > 0.25f) dt = 0.25f;
    frameAt = now;
    live.ms = now;
    bool down[4];
    for (int i = 0; i < 4; i++) down[i] = keyDown[i] || touchDown[i];
    scene.step(dt, state, waiting);
    scene.draw({art, pd::AW, pd::AH}, state, waiting);
    pd::draw(frame, art, state, waiting, moment, pad, scene.cx, down, fresh(now) ? &screen : nullptr, &live, &hits,
             page, &usage, &today);
    show();
  }
  delay(1);
}
