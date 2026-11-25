// The Claude Pad's screen: a 2.8" ILI9341V turned on its side, 320 x 240, in colour, with the
// pad's four keys just below it.
//
// Plain C++ like render.h, which it builds on: the firmware sends these pixels to the display,
// and the simulator (tools/sim.cpp --pad) writes them to a picture.
//
// Version 6: three pages you swipe between (or step with keys 1 and 4), with dots at the bottom:
//   HOME   the robot, big (each art pixel 3 x 3), with its faces and motion, and over it the state
//          (LOCKED IN) and how long you have been at it (42M). No ladder, lamps or numbers.
//   USAGE  the Claude plan's 5-hour and weekly use with when they reset, and how full the latest
//          session's context is (the U line).
//   TODAY  tokens per hour of today as 24 bars, focus time, sessions and peak burn (the D line).
//
// While Claude waits for a yes, Home comes back with a yellow card over the top (who is asking and
// what it wants to do) and the key labels along the bottom: DENY and APPROVE. What the robot is
// saying (the M line) is the same kind of card, in its own colour.
//
// From version 5 the Mac can also send a whole screen (the S line, below): a question with four
// answers, the words you said for you to check, Claude's reply being read out, and so on. Two
// screens the pad draws by itself, since only it knows them as they happen: listening (while key 2
// is held) and touch calibration. Each frame also lists where a finger may tap (Hits): the key
// strip, and a question's answers.
#pragma once
#include "render.h"

namespace pd {

const int W = 320, H = 240;
const int KEYS = 28;                        // the strip of key labels at the bottom
const int KY = H - KEYS;                    // where it starts
const int PX = 4;                           // display pixels per art pixel, on Home
const int AW = W / PX, AH = H / PX;         // Home's scene: 80 x 60, the whole screen
// The robot has no legs here: its feet are below the bottom edge and its chest meets the pad's
// frame, so the pad is its body.
const int LIFT = -5;
const int HEAD = 36;                        // a page's title band
enum Page { HOME, USAGE, TODAY, PAGES };
const int COLS = 25;                        // letters across a card, at twice the size

// The pad's own fields of the B line, which other boards ignore:
//   ky=DENY,TALK,SHOW,APPROVE   what each key does now, left to right ("" for a key that does nothing)
//   pr=BASH: NPM TEST           what Claude is waiting for a yes to (absent when nothing waits); the
//                               whole request, which the Mac sends only when it fits a card; a "|" in
//                               it comes as the byte 0x7F, since "|" ends a field
//   pp=SHOP                     the project asking
//   wk=42                       minutes you have been at it, this stretch (version 6)
struct Pad {
  char keys[4][10] = {{0}, {0}, {0}, {0}};
  char ask[112] = "";                       // four rows of COLS letters and the spaces between
  char who[20] = "";
  int worked = -1;

  /** Labels from the Mac: the keys mean what the strip says, and are its to handle. */
  bool labelled() const { return keys[0][0] || keys[1][0] || keys[2][0] || keys[3][0]; }
};

inline bool parsePad(const char *line, Pad &out) {
  if (line[0] != 'B' || line[1] != '|') return false;
  Pad p;
  const char *at = line + 2;
  while (*at) {
    const char *eq = strchr(at, '=');
    if (!eq) return false;
    const char *end = strchr(eq, '|');
    if (!end) end = eq + strlen(eq);
    size_t klen = (size_t)(eq - at), vlen = (size_t)(end - eq - 1);
    const char *v = eq + 1;
    if (klen == 2 && !strncmp(at, "ky", 2)) {
      int k = 0;
      const char *from = v;
      for (const char *c = v; c <= end && k < 4; c++)
        if (c == end || *c == ',') {
          bb::copyText(p.keys[k++], sizeof(p.keys[0]), from, (size_t)(c - from));
          from = c + 1;
        }
    } else if (klen == 2 && !strncmp(at, "pr", 2)) {
      bb::copyText(p.ask, sizeof(p.ask), v, vlen);
      for (char *c = p.ask; *c; c++)
        if (*c == 0x7F) *c = '|';
    }
    else if (klen == 2 && !strncmp(at, "pp", 2)) bb::copyText(p.who, sizeof(p.who), v, vlen);
    else if (klen == 2 && !strncmp(at, "wk", 2)) p.worked = atoi(v);
    at = *end ? end + 1 : end;
  }
  out = p;
  return true;
}

inline bool samePad(const Pad &a, const Pad &b) { return !memcmp(&a, &b, sizeof(Pad)); }

// The S line: a screen the Mac chooses (version 5).
//   m=q|rev|stt|sent|say|err|dl|home   which screen (home: the robot, as the B line says)
//   t=SHOP ASKS 1/2                     the title
//   x=WHICH BRANCH?                     the body: a question, the words you said, a reply
//   o1=MAIN .. o4=                      a question's answers, one per key
//   ky=1,2,3,4                          what each key does now (as on the B line)
//   kc=rgyb                             each key's colour: r red, g green, y yellow, b blue, v violet,
//                                       - the usual
//   mc=1                                key 2 records while held (the pad then draws "listening")
//   pg=40                               progress, 0 to 100 (getting the speech model)
//   to=SHOP                             who a recording goes to
// As on the B line, a "|" inside a text comes as the byte 0x7F.
struct Screen {
  char mode[6] = "";
  char title[28] = "";
  char body[320] = "";
  char opts[4][24] = {{0}, {0}, {0}, {0}};
  char keys[4][10] = {{0}, {0}, {0}, {0}};
  char colours[5] = "----";
  bool mic = false;
  int progress = -1;
  char to[20] = "";
};

inline void unpipe(char *t) {
  for (; *t; t++)
    if (*t == 0x7F) *t = '|';
}

inline bool parseScreen(const char *line, Screen &out) {
  if (line[0] != 'S' || line[1] != '|') return false;
  Screen p;
  const char *at = line + 2;
  while (*at) {
    const char *eq = strchr(at, '=');
    if (!eq) return false;
    const char *end = strchr(eq, '|');
    if (!end) end = eq + strlen(eq);
    size_t klen = (size_t)(eq - at), vlen = (size_t)(end - eq - 1);
    const char *v = eq + 1;
    if (klen == 1 && *at == 'm') {                  // as it came: copyText would make it capitals
      size_t n = vlen < sizeof(p.mode) - 1 ? vlen : sizeof(p.mode) - 1;
      memcpy(p.mode, v, n);
      p.mode[n] = 0;
    }
    else if (klen == 1 && *at == 't') bb::copyText(p.title, sizeof(p.title), v, vlen);
    else if (klen == 1 && *at == 'x') bb::copyText(p.body, sizeof(p.body), v, vlen);
    else if (klen == 2 && at[0] == 'o' && at[1] >= '1' && at[1] <= '4')
      bb::copyText(p.opts[at[1] - '1'], sizeof(p.opts[0]), v, vlen);
    else if (klen == 2 && !strncmp(at, "ky", 2)) {
      int k = 0;
      const char *from = v;
      for (const char *c = v; c <= end && k < 4; c++)
        if (c == end || *c == ',') {
          bb::copyText(p.keys[k++], sizeof(p.keys[0]), from, (size_t)(c - from));
          from = c + 1;
        }
    } else if (klen == 2 && !strncmp(at, "kc", 2)) {
      for (size_t i = 0; i < 4 && i < vlen; i++) p.colours[i] = v[i];
    } else if (klen == 2 && !strncmp(at, "mc", 2)) p.mic = vlen && *v == '1';
    else if (klen == 2 && !strncmp(at, "pg", 2)) p.progress = vlen ? atoi(v) : -1;
    else if (klen == 2 && !strncmp(at, "to", 2)) bb::copyText(p.to, sizeof(p.to), v, vlen);
    at = *end ? end + 1 : end;
  }
  unpipe(p.title);
  unpipe(p.body);
  for (int i = 0; i < 4; i++) unpipe(p.opts[i]);
  out = p;
  return true;
}

inline bool sameScreen(const Screen &a, const Screen &b) { return !memcmp(&a, &b, sizeof(Screen)); }

// What only the pad knows, as it happens: listening, and touch calibration.
struct Live {
  bool recording = false;
  int seconds = 0;                          // how long it has been listening
  uint8_t level[40] = {0};                  // loudness, 0 to 100, one per 100 ms, newest last
  uint32_t ms = 0;                          // the pad's clock, for things that move
  int calStep = -1;                         // calibrating: the cross to tap (0 to 4), 5 when done; -1 not
  int targets[5][2] = {{0, 0}};             // the crosses (calib.h's TARGET)
  char note[28] = "";                       // calibration's result: SAVED or TRY AGAIN
};

// Where a finger may tap, and which key that is.
struct Hits {
  struct Zone { int16_t x0, y0, x1, y1; int8_t key; } zone[12];
  int n = 0;
  void clear() { n = 0; }
  void add(int x0, int y0, int x1, int y1, int key) {
    if (n < 12) zone[n++] = {(int16_t)x0, (int16_t)y0, (int16_t)x1, (int16_t)y1, (int8_t)key};
  }
  /** The key at (x, y), or 0; a tap up to `slack` pixels outside every zone counts for the nearest. */
  int at(int x, int y, int slack = 6) const {
    for (int pass = 0; pass < 2; pass++) {
      int e = pass ? slack : 0;
      for (int i = 0; i < n; i++)
        if (x >= zone[i].x0 - e && x < zone[i].x1 + e && y >= zone[i].y0 - e && y < zone[i].y1 + e) return zone[i].key;
    }
    return 0;
  }
};

// ---------------------------------------------------------------- drawing
// A frame is W x H colours, row after row.

inline void fill(uint16_t *f, int x, int y, int w, int h, uint16_t c) {
  int x1 = x + w > W ? W : x + w, y1 = y + h > H ? H : y + h;
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  for (int yy = y; yy < y1; yy++)
    for (int xx = x; xx < x1; xx++) f[yy * W + xx] = c;
}

// The 5 x 7 letters of render.h; `gap` is the space between them (one font pixel unless squeezed).
inline void text(uint16_t *f, int x, int y, const char *s, int scale, uint16_t c, int gap = -1) {
  if (gap < 0) gap = scale;
  for (int i = 0; s[i]; i++) {
    const uint8_t *rows = bb::glyph(s[i]);
    if (!rows) continue;
    for (int r = 0; r < 7; r++)
      for (int k = 0; k < 5; k++)
        if (rows[r] & (16 >> k)) fill(f, x + i * (5 * scale + gap) + k * scale, y + r * scale, scale, scale, c);
  }
}

/** Centred across x .. x + w, as large as fits: `scale`, then with the letters closer, then a size down. */
inline void fitted(uint16_t *f, int x, int w, int y, const char *s, int scale, uint16_t c) {
  int gap = scale;
  if (bb::textWidth(s, scale) > w) gap = scale > 1 ? scale / 2 : 0;
  if (bb::textWidth(s, scale, gap) > w && scale > 1) {
    scale--;
    y += 3;
    gap = bb::textWidth(s, scale) <= w ? scale : 1;
  }
  text(f, x + (w - bb::textWidth(s, scale, gap)) / 2, y, s, scale, c, gap);
}

/** Words wrapped to COLS letters, at most `most` rows; a word too long for a row is cut. Text
 *  that does not fit ends the last row with "..". */
inline int wrap(const char *s, char (*rows)[COLS + 1], int most = 4) {
  int n = 0, len = 0;
  rows[0][0] = 0;
  while (*s && n < most) {
    while (*s == ' ') s++;
    if (!*s) break;
    int wl = 0;
    while (s[wl] && s[wl] != ' ') wl++;
    int take = wl > COLS ? COLS : wl;
    if (len && len + 1 + take > COLS) {             // the next row
      if (n + 1 == most) {                          // no room: say so
        if (len > COLS - 2) len = COLS - 2;
        rows[n][len++] = '.';
        rows[n][len++] = '.';
        rows[n][len] = 0;
        return n + 1;
      }
      n++;
      len = 0;
      rows[n][0] = 0;
    }
    if (len) rows[n][len++] = ' ';
    memcpy(rows[n] + len, s, (size_t)take);
    len += take;
    rows[n][len] = 0;
    s += wl;
  }
  return len ? n + 1 : n;
}

/** A card across the top: a border in its colour with notched corners, the title, up to four
 *  lines below it (the first `head` brighter), and a tail down to the robot's antenna at `tip`.
 *  Four lines reach row 90 and the tail row 94, just over the antenna's top. */
inline void card(uint16_t *f, const char *title, const char (*lines)[COLS + 1], int n, int head, uint16_t line, int tip) {
  const uint16_t paper = bb::rgb(0x0b0f15), ink2 = bb::rgb(0xaab3c1);
  const int x = 2, y = 0, w = W - 4, k = 3;
  n = n < 0 ? 0 : n > 4 ? 4 : n;
  const int pitch = n < 4 ? 18 : 15;            // four lines only just fit above the antenna
  int h = 2 * k + 6 + 14 + (n ? 5 + n * pitch - (pitch - 14) : 0);
  fill(f, x + k, y, w - 2 * k, k, line);
  fill(f, x + k, y + h - k, w - 2 * k, k, line);
  fill(f, x, y + k, k, h - 2 * k, line);
  fill(f, x + w - k, y + k, k, h - 2 * k, line);
  fill(f, x + k, y + k, w - 2 * k, h - 2 * k, paper);
  fill(f, tip - 6, y + h, 12, 2, line);
  fill(f, tip - 3, y + h + 2, 6, 2, line);

  const int tx = x + k + 5, room = w - 2 * k - 10;
  text(f, tx, y + k + 3, title, 2, line, bb::textWidth(title, 2) <= room ? 2 : 1);
  for (int i = 0; i < n; i++)
    text(f, tx, y + k + 3 + 14 + 5 + i * pitch, lines[i], 2, i < head ? bb::INK : ink2, bb::textWidth(lines[i], 2) <= room ? 2 : 1);
}

/** What each key does, over the key: lit while it is held down. */
const uint16_t RED = bb::rgb(0xff6b5e), GREEN = bb::rgb(0x4fc9a4), YELLOW = bb::rgb(0xffd25e),
               BLUE = bb::rgb(0x5ec8ff), VIOLET = bb::rgb(0xc58cff), AMBER = bb::rgb(0xffa94d),
               PAPER = bb::rgb(0x0b0f15), INK2 = bb::rgb(0xaab3c1);

/** A key's colour by letter (r g y b v a), or `usual` for "-". */
inline uint16_t keyColour(char c, uint16_t usual) {
  switch (c) {
    case 'r': return RED;
    case 'g': return GREEN;
    case 'y': return YELLOW;
    case 'b': return BLUE;
    case 'v': return VIOLET;
    case 'a': return AMBER;
    default: return usual;
  }
}

/** The four key labels along the bottom, each over its key, in the colours `colours` names. A key
 *  that does nothing now is a dim slot; the others are also places to tap (`hits`). */
inline void strip(uint16_t *f, const char (*labels)[10], const char *colours, uint16_t usual, const bool *down,
                  Hits *hits) {
  fill(f, 0, KY, W, KEYS, PAPER);
  const int cw = W / 4;
  for (int i = 0; i < 4; i++) {
    int x = i * cw + 1, w = cw - 2, y = KY + 3, h = KEYS - 6;
    uint16_t c = keyColour(colours[i], usual);
    bool held = down && down[i];
    if (!labels[i][0]) {                            // a key that does nothing now: a dim slot
      fill(f, x, y + h - 3, w, 2, held ? bb::DIM : bb::mix(PAPER, bb::DIM, 70));
      continue;
    }
    if (hits) hits->add(i * cw, KY, (i + 1) * cw, H, i + 1);
    fill(f, x, y, w, h, held ? c : bb::mix(PAPER, c, 36));
    fitted(f, x + 1, w - 2, y + (h - 14) / 2, labels[i], 2, held ? PAPER : c);
  }
}

inline void keys(uint16_t *f, const Pad &p, const bb::State &s, bool waiting, bool asking, const bool *down,
                 Hits *hits = nullptr) {
  strip(f, p.keys, asking ? "r--g" : "----", waiting ? bb::DIM : bb::ACCENT[s.look], down, hits);
}

/** A screen's colour: its title and border. */
inline uint16_t modeColour(const char *mode, uint16_t usual) {
  if (!strcmp(mode, "q")) return YELLOW;
  if (!strcmp(mode, "rev")) return BLUE;
  if (!strcmp(mode, "stt")) return VIOLET;
  if (!strcmp(mode, "sent") || !strcmp(mode, "say")) return GREEN;
  if (!strcmp(mode, "err")) return RED;
  if (!strcmp(mode, "dl")) return BLUE;
  return usual;
}

/** The top of a screen: its title, in its colour, over a rule. */
inline void heading(uint16_t *f, const char *title, uint16_t line) {
  fill(f, 0, 0, W, KY, PAPER);
  fill(f, 0, 0, W, 3, line);
  const int room = W - 16;
  text(f, 8, 9, title, 2, line, bb::textWidth(title, 2) <= room ? 2 : 1);
  fill(f, 8, 28, W - 16, 1, bb::mix(PAPER, line, 50));
}

/** Rows of text from `y`, `pitch` apart; the first `head` in white, the rest grey. */
inline void lines(uint16_t *f, const char (*rows)[COLS + 1], int n, int y, int pitch, int head) {
  for (int i = 0; i < n; i++)
    text(f, 10, y + i * pitch, rows[i], 2, i < head ? bb::INK : INK2, bb::textWidth(rows[i], 2) <= W - 20 ? 2 : 1);
}

/** A screen the Mac chose (the S line): a question, your words to check, a reply, and so on. */
inline void panel(uint16_t *f, const Screen &sc, const Live &lv, uint16_t usual, Hits *hits) {
  const uint16_t line = modeColour(sc.mode, usual);
  heading(f, sc.title, line);
  if (!strcmp(sc.mode, "q")) {                      // the question, then its answers, one per key
    char rows[3][COLS + 1];
    int n = wrap(sc.body, rows, 3);
    lines(f, rows, n, 36, 18, n);
    for (int i = 0; i < 4; i++) {
      if (!sc.opts[i][0]) continue;
      const int y = 96 + i * 29, h = 26;
      uint16_t c = keyColour(sc.colours[i], line);
      fill(f, 4, y, W - 8, h, bb::mix(PAPER, c, 16));
      fill(f, 4, y, h, h, c);
      char digit[2] = {(char)('1' + i), 0};
      text(f, 4 + (h - 10) / 2, y + (h - 14) / 2, digit, 2, PAPER);
      const int room = W - 8 - h - 10;
      text(f, 4 + h + 8, y + (h - 14) / 2, sc.opts[i], 2, bb::INK, bb::textWidth(sc.opts[i], 2) <= room ? 2 : 1);
      if (hits) hits->add(4, y, W - 4, y + h, i + 1);
    }
    return;
  }
  if (!strcmp(sc.mode, "stt")) {                    // turning speech into text: three dots that pulse
    char rows[4][COLS + 1];
    int n = wrap(sc.body, rows, 4);
    lines(f, rows, n, 40, 19, n);
    for (int i = 0; i < 3; i++) {
      int phase = (int)((lv.ms / 140 + 6 - i * 2) % 6);
      fill(f, W / 2 - 34 + i * 26, 150, 16, 16, phase < 2 ? line : bb::mix(PAPER, line, 30));
    }
    return;
  }
  char rows[9][COLS + 1];
  const int most = !strcmp(sc.mode, "dl") ? 4 : 9;
  int n = wrap(sc.body, rows, most);
  lines(f, rows, n, 38, 19, !strcmp(sc.mode, "rev") ? n : 1);
  if (!strcmp(sc.mode, "dl") && sc.progress >= 0) { // the speech model coming down
    const int x = 10, y = 150, w = W - 20, h = 16, p = sc.progress > 100 ? 100 : sc.progress;
    fill(f, x, y, w, h, bb::mix(PAPER, line, 22));
    fill(f, x, y, w * p / 100, h, line);
    char pc[8];
    snprintf(pc, sizeof(pc), "%d%%", p);
    fitted(f, 0, W, y + 26, pc, 2, bb::INK);
  }
  if (!strcmp(sc.mode, "say")) {                    // being read out: bars that move, top right
    for (int i = 0; i < 5; i++) {
      int t = (int)((lv.ms / 90 + i * 7) % 16), h = 3 + (t < 8 ? t : 16 - t) * 2;
      fill(f, W - 52 + i * 8, 22 - h, 5, h, line);
    }
  }
}

/** Listening, while key 2 is held: who it goes to, how long so far, and how loud. */
inline void listening(uint16_t *f, const Screen *sc, const Live &lv) {
  char title[40] = "LISTENING";
  if (sc && sc->to[0]) snprintf(title, sizeof(title), "LISTENING  TO %s", sc->to);
  heading(f, title, RED);
  char secs[8];
  snprintf(secs, sizeof(secs), "%d:%02d", lv.seconds / 60 % 60, lv.seconds % 60);
  fitted(f, 0, W, 42, secs, 5, bb::INK);
  const int mid = 140;
  for (int i = 0; i < 40; i++) {
    int v = lv.level[i] > 100 ? 100 : lv.level[i], h = 1 + v * 36 / 100;
    fill(f, i * 8 + 1, mid - h, 6, 2 * h, bb::mix(bb::mix(PAPER, RED, 40), RED, v * 255 / 100));
  }
  fitted(f, 0, W, 188, "LET GO TO STOP", 2, INK2);
}

/** Touch setup: tap each cross in turn. The pad draws it itself, over the whole screen. */
inline void calibration(uint16_t *f, const Live &lv) {
  fill(f, 0, 0, W, H, bb::rgb(0x05070a));
  for (int i = 0; i < 5; i++) {
    int x = lv.targets[i][0], y = lv.targets[i][1];
    if (i == lv.calStep) {
      fill(f, x - 12, y - 1, 25, 3, GREEN);
      fill(f, x - 1, y - 12, 3, 25, GREEN);
      fill(f, x - 9, y - 9, 19, 1, GREEN);
      fill(f, x - 9, y + 9, 19, 1, GREEN);
      fill(f, x - 9, y - 9, 1, 19, GREEN);
      fill(f, x + 9, y - 9, 1, 19, GREEN);
    } else if (i > lv.calStep) {
      fill(f, x - 1, y - 1, 3, 3, bb::DIM);
    } else {
      fill(f, x - 2, y - 2, 5, 5, bb::mix(bb::rgb(0x05070a), GREEN, 120));
    }
  }
  fitted(f, 0, W, 60, "TOUCH SETUP", 2, GREEN);
  if (lv.calStep >= 0 && lv.calStep < 5) {
    fitted(f, 0, W, 82, "TAP THE GREEN CROSS", 2, bb::INK);
    char step[12];
    snprintf(step, sizeof(step), "%d OF 5", lv.calStep + 1);
    fitted(f, 0, W, 148, step, 2, INK2);
  }
  if (lv.note[0]) fitted(f, 0, W, 166, lv.note, 2, strstr(lv.note, "SAVED") ? GREEN : RED);
  fitted(f, 0, W, 190, "KEY 1: CANCEL", 2, bb::DIM);
}

// ---- Version 6: the pages -------------------------------------------------------------------

// The U line: the Claude plan's use, from Claude Code's status line. Any field may be missing.
//   U|h=46|hr=IN 2H 10M|w=62|wr=TUE 09:00|x=28|xn=SHOP|ag=3
//   h, w: 5-hour and weekly use in %; hr, wr: when they reset; x: how full the latest session's
//   context is, in %, and xn its project; ag: minutes since Claude Code last said
struct Usage {
  int hour = -1, week = -1, context = -1, age = -1;
  char hourResets[20] = "", weekResets[20] = "", project[20] = "";
  bool any() const { return hour >= 0 || week >= 0 || context >= 0; }
};

// The D line: today. D|b=0,0,3,..(24 values, 0-99 of the busiest hour)|fm=4H 20M|ss=8|pk=3.2X
struct Today {
  uint8_t bars[24] = {0};
  int hours = 0, sessions = -1;
  char focus[12] = "", peak[10] = "";
  bool any() const { return hours > 0 || sessions >= 0 || focus[0]; }
};

/** Walks the fields of a line after its "X|": calls `each(key, keyLength, value, valueLength)`. */
template <typename F> inline bool fields(const char *line, char kind, F each) {
  if (line[0] != kind || line[1] != '|') return false;
  const char *at = line + 2;
  while (*at) {
    const char *eq = strchr(at, '=');
    if (!eq) return false;
    const char *end = strchr(eq, '|');
    if (!end) end = eq + strlen(eq);
    each(at, (size_t)(eq - at), eq + 1, (size_t)(end - eq - 1));
    at = *end ? end + 1 : end;
  }
  return true;
}

inline bool key(const char *k, size_t n, const char *want) { return n == strlen(want) && !strncmp(k, want, n); }

inline bool parseUsage(const char *line, Usage &out) {
  Usage u;
  bool ok = fields(line, 'U', [&](const char *k, size_t kn, const char *v, size_t vn) {
    if (key(k, kn, "h")) u.hour = atoi(v);
    else if (key(k, kn, "w")) u.week = atoi(v);
    else if (key(k, kn, "x")) u.context = atoi(v);
    else if (key(k, kn, "ag")) u.age = atoi(v);
    else if (key(k, kn, "hr")) bb::copyText(u.hourResets, sizeof u.hourResets, v, vn);
    else if (key(k, kn, "wr")) bb::copyText(u.weekResets, sizeof u.weekResets, v, vn);
    else if (key(k, kn, "xn")) bb::copyText(u.project, sizeof u.project, v, vn);
  });
  if (ok) out = u;
  return ok;
}

inline bool parseToday(const char *line, Today &out) {
  Today t;
  bool ok = fields(line, 'D', [&](const char *k, size_t kn, const char *v, size_t vn) {
    if (key(k, kn, "b")) {
      const char *c = v, *end = v + vn;
      while (c < end && t.hours < 24) {
        int n = atoi(c);
        t.bars[t.hours++] = (uint8_t)(n < 0 ? 0 : n > 99 ? 99 : n);
        while (c < end && *c != ',') c++;
        if (c < end) c++;
      }
    } else if (key(k, kn, "ss")) t.sessions = atoi(v);
    else if (key(k, kn, "fm")) bb::copyText(t.focus, sizeof t.focus, v, vn);
    else if (key(k, kn, "pk")) bb::copyText(t.peak, sizeof t.peak, v, vn);
  });
  if (ok) out = t;
  return ok;
}

inline bool sameUsage(const Usage &a, const Usage &b) { return !memcmp(&a, &b, sizeof(Usage)); }
inline bool sameToday(const Today &a, const Today &b) { return !memcmp(&a, &b, sizeof(Today)); }

/** Sets up Home's scene: the robot alone in the middle (no ladder, lamps or bench), over a floor
 *  that burns as hard as the flame says. */
inline void homeScene(bb::Scene &scene) {
  scene.plain = true;
  scene.cx = AW / 2;
  scene.reach = 16;                         // how high the fire may climb (size() bases it on the feet)
}

const uint16_t ORANGE = bb::rgb(0xff8a3d), TRACK = bb::rgb(0x1c222c), RULE = bb::rgb(0x1a1f27);

// 7 x 7 pixel icons, a row each, the left pixel the 64s bit.
const uint8_t ICON_FLAME[7] = {8, 12, 28, 62, 54, 34, 28};
const uint8_t ICON_BARS[7] = {3, 3, 15, 15, 63, 63, 127};
const uint8_t ICON_BOLT[7] = {6, 12, 24, 62, 12, 24, 48};
const uint8_t ICON_CALENDAR[7] = {34, 127, 65, 85, 65, 85, 127};
const uint8_t ICON_CONTEXT[7] = {127, 65, 93, 65, 93, 65, 127};
const uint8_t ICON_CLOCK[7] = {62, 73, 73, 77, 65, 65, 62};
const uint8_t ICON_STACK[7] = {127, 0, 127, 0, 127, 0, 0};

inline void icon(uint16_t *f, int x, int y, const uint8_t *rows, int scale, uint16_t c) {
  for (int r = 0; r < 7; r++)
    for (int k = 0; k < 7; k++)
      if (rows[r] & (64 >> k)) fill(f, x + k * scale, y + r * scale, scale, scale, c);
}

inline void rightText(uint16_t *f, int right, int y, const char *s, int scale, uint16_t c) {
  text(f, right - bb::textWidth(s, scale), y, s, scale, c);
}

/** A page's title band: an icon, the title, and something on the right. */
inline void title(uint16_t *f, const uint8_t *ic, uint16_t icColour, const char *name, uint16_t nameColour,
                  const char *right, uint16_t rightColour) {
  int rw = right && right[0] ? bb::textWidth(right, 2) : 0;
  int x = 10;
  if (ic) {
    icon(f, x, 8, ic, 3, icColour);
    x += 30;
  }
  int room = W - 10 - (rw ? rw + 12 : 0) - x;
  int scale = bb::textWidth(name, 3) <= room ? 3 : 2;
  text(f, x, scale == 3 ? 8 : 12, name, scale, nameColour);
  if (rw) rightText(f, W - 10, 12, right, 2, rightColour);
}

/** "42M", "1H 05M". */
inline void duration(char *out, size_t size, int minutes) {
  if (minutes < 60) snprintf(out, size, "%dM", minutes);
  else snprintf(out, size, "%dH %02dM", minutes / 60, minutes % 60);
}

/** The page dots, the shown one lit. */
inline void dots(uint16_t *f, int page, uint16_t lit) {
  const int size = 6, step = 14, y = 226, x0 = W / 2 - (PAGES - 1) * step / 2 - size / 2;
  for (int i = 0; i < PAGES; i++) {
    int x = x0 + i * step;
    fill(f, x - 1, y - 1, size + 2, size + 2, PAPER);
    fill(f, x, y, size, size, i == page ? lit : bb::rgb(0x3a414c));
  }
}

/** Home: the scene at 3 x over the whole screen, the state and how long over its top. */
inline void home(uint16_t *f, const uint16_t *art, const bb::State &s, bool waiting, const Pad &p) {
  for (int y = 0; y < AH; y++) {
    uint16_t *row = f + y * PX * W;
    const uint16_t *from = art + y * AW;
    for (int x = 0; x < AW; x++)
      for (int k = 0; k < PX; k++) row[PX * x + k] = from[x];
    for (int x = AW * PX; x < W; x++) row[x] = from[AW - 1];
    for (int k = 1; k < PX; k++) memcpy(row + k * W, row, W * sizeof(uint16_t));
  }
  if (waiting) {
    fitted(f, 0, W, 12, "WAITING FOR THE COMPUTER", 2, bb::DIM);
    return;
  }
  char how[12] = "";
  if (p.worked > 0) duration(how, sizeof how, p.worked);
  const uint16_t accent = bb::ACCENT[s.look];
  title(f, ICON_FLAME, accent, s.name, accent, how, INK2);
}

/** One meter on Usage: icon, name, percent, a bar, and a line under it. */
inline void meter(uint16_t *f, int y, const uint8_t *ic, const char *name, int pct, const char *under) {
  icon(f, 10, y, ic, 2, ORANGE);
  text(f, 32, y, name, 2, bb::INK);
  char v[8];
  if (pct >= 0) snprintf(v, sizeof v, "%d%%", pct);
  else snprintf(v, sizeof v, "-");
  rightText(f, W - 10, y, v, 2, bb::INK);
  const int bx = 10, bw = W - 20, by = y + 20, bh = 10;
  fill(f, bx, by, bw, bh, TRACK);
  int p = pct < 0 ? 0 : pct > 100 ? 100 : pct;
  uint16_t c = p >= 95 ? RED : p >= 80 ? AMBER : ORANGE;
  if (p) fill(f, bx, by, bw * p / 100 < 3 ? 3 : bw * p / 100, bh, c);
  const int ex[2] = {bx, bx + bw - 1}, ey[2] = {by, by + bh - 1};   // round the ends a little
  for (int i = 0; i < 4; i++) fill(f, ex[i & 1], ey[i >> 1], 1, 1, PAPER);
  text(f, 10, y + 36, pct >= 0 ? under : "NOT SAID YET", 2, bb::DIM);
}

inline void usage(uint16_t *f, const Usage &u) {
  fill(f, 0, 0, W, H, PAPER);
  char ago[16] = "";
  if (u.age > 15) {
    duration(ago, sizeof ago, u.age);
    strncat(ago, " AGO", sizeof ago - strlen(ago) - 1);
  }
  title(f, ICON_BARS, ORANGE, "USAGE", bb::INK, ago, bb::DIM);
  fill(f, 10, HEAD + 2, W - 20, 1, RULE);
  if (!u.any()) {
    fitted(f, 0, W, 96, "USAGE SHOWS AFTER", 2, bb::DIM);
    fitted(f, 0, W, 118, "CLAUDE'S NEXT REPLY", 2, bb::DIM);
    return;
  }
  char resets[32];
  snprintf(resets, sizeof resets, "RESETS %s", u.hourResets);
  meter(f, 48, ICON_BOLT, "5-HOUR", u.hour, u.hourResets[0] ? resets : "");
  snprintf(resets, sizeof resets, "RESETS %s", u.weekResets);
  meter(f, 106, ICON_CALENDAR, "WEEKLY", u.week, u.weekResets[0] ? resets : "");
  meter(f, 164, ICON_CONTEXT, "CONTEXT", u.context, u.project);
}

inline void todayRow(uint16_t *f, int y, const uint8_t *ic, const char *name, const char *value) {
  icon(f, 10, y, ic, 2, ORANGE);
  text(f, 32, y, name, 2, INK2);
  rightText(f, W - 10, y, value[0] ? value : "-", 2, ORANGE);
}

/** Today: tokens per hour as 24 bars (this hour brightest, those to come a dim stub), then focus
 *  time, sessions and the peak burn. `hour`: the hour now, or -1 if unknown. */
inline void today(uint16_t *f, const Today &t, int hour, const char *clock) {
  fill(f, 0, 0, W, H, PAPER);
  title(f, ICON_CALENDAR, ORANGE, "TODAY", bb::INK, clock, bb::DIM);
  const int slot = 12, bw = 8, x0 = (W - 24 * slot) / 2 + (slot - bw) / 2, base = 122, tall = 74;
  for (int i = 0; i < 24; i++) {
    int x = x0 + i * slot, v = i < t.hours ? t.bars[i] : 0;
    bool later = hour >= 0 && i > hour, now = i == hour;
    if (later) {
      fill(f, x, base - 2, bw, 2, TRACK);
      continue;
    }
    int h = v * tall / 99;
    if (v > 0 && h < 2) h = 2;
    if (!h) {
      fill(f, x, base - 1, bw, 1, TRACK);
      continue;
    }
    uint16_t c = now ? bb::rgb(0xffc27a) : bb::mix(PAPER, ORANGE, 110 + v * 145 / 99);
    fill(f, x, base - h, bw, h, c);
  }
  fill(f, x0 - 2, base + 1, 24 * slot - (slot - bw) + 4, 1, RULE);
  for (int i = 0; i <= 24; i += 6) {
    char lab[4];
    snprintf(lab, sizeof lab, "%02d", i);
    int cx = x0 - (slot - bw) / 2 + i * slot;     // where hour i starts
    int tx = cx - bb::textWidth(lab, 2) / 2;
    if (tx < 4) tx = 4;
    if (tx + bb::textWidth(lab, 2) > W - 4) tx = W - 4 - bb::textWidth(lab, 2);
    text(f, tx, base + 6, lab, 2, bb::DIM);
  }
  char ss[8] = "";
  if (t.sessions >= 0) snprintf(ss, sizeof ss, "%d", t.sessions);
  todayRow(f, 150, ICON_CLOCK, "FOCUS TIME", t.focus);
  fill(f, 10, 169, W - 20, 1, RULE);
  todayRow(f, 175, ICON_STACK, "SESSIONS", ss);
  fill(f, 10, 194, W - 20, 1, RULE);
  todayRow(f, 200, ICON_FLAME, "PEAK BURN", t.peak);
}

/** The whole frame. `art`: Home's scene, already drawn by render.h (AW x AH). `tip`: the robot's
 *  column in it, for a card's tail. `down`: which keys are held (or null). `sc`: the screen the Mac
 *  chose (or null); `lv`: listening and calibration (or null); `hits` gets the places a finger may
 *  tap (or null). `page`, `usage`, `today`: which page shows, and what Usage and Today say. A card
 *  or key labels from the Mac always show on Home. */
inline void draw(uint16_t *f, const uint16_t *art, const bb::State &s, bool waiting, const bb::Moment &m, const Pad &p,
                 int tip, const bool *down, const Screen *sc = nullptr, const Live *lv = nullptr,
                 Hits *hits = nullptr, int page = HOME, const Usage *usage = nullptr,
                 const Today *today = nullptr) {
  if (hits) hits->clear();
  if (lv && lv->calStep >= 0) {
    calibration(f, *lv);
    return;
  }
  const uint16_t usual = waiting ? bb::DIM : bb::ACCENT[s.look];
  if (lv && lv->recording) {
    listening(f, sc, *lv);
    const char labels[4][10] = {"CANCEL", "LET GO", "", ""};
    strip(f, labels, "ra--", usual, down, hits);
    return;
  }
  if (sc && !waiting && sc->mode[0] && strcmp(sc->mode, "home")) {
    static const Live still;
    panel(f, *sc, lv ? *lv : still, usual, hits);
    strip(f, sc->keys, sc->colours, usual, down, hits);
    return;
  }
  bool asking = !waiting && p.ask[0], labelled = !waiting && p.labelled();
  const uint16_t lit = waiting ? bb::DIM : bb::ACCENT[s.look];
  if (!asking && !labelled && !waiting && page == USAGE) {
    static const Usage none;
    pd::usage(f, usage ? *usage : none);
    dots(f, page, ORANGE);
    return;
  }
  if (!asking && !labelled && !waiting && page == TODAY) {
    static const Today none;
    int hour = s.clock[0] && s.clock[2] == ':' ? atoi(s.clock) : -1;
    pd::today(f, today ? *today : none, hour, s.clock);
    dots(f, page, ORANGE);
    return;
  }
  home(f, art, s, waiting, p);
  tip = tip * PX + 1;
  if (asking) {
    char rows[4][COLS + 1];
    int n = wrap(p.ask, rows);
    char title[32];
    snprintf(title, sizeof(title), "%s ASKS", p.who[0] ? p.who : "CLAUDE");
    card(f, title, rows, n, n, bb::rgb(0xffd25e), tip);
  } else if (!waiting && m.on) {
    char rows[4][COLS + 1];
    int n = m.count < 0 ? 0 : m.count > 4 ? 4 : m.count;
    for (int i = 0; i < n; i++) bb::copyText(rows[i], sizeof(rows[i]), m.lines[i], strlen(m.lines[i]));
    uint16_t line = m.kind == 'w' ? bb::rgb(0xffd25e) : m.kind == 'b' ? bb::rgb(0x4fc9a4) : bb::ACCENT[s.look];
    card(f, m.title, rows, n, m.head, line, tip);
  }
  if (labelled) keys(f, p, s, waiting, asking, down, hits);
  else if (!waiting) dots(f, HOME, lit);
}

}  // namespace pd
