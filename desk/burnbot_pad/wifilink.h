// The pad over Wi-Fi: the same lines as over USB, on TCP port 4751, after both sides prove they were
// paired over USB (from the old Claude Pad firmware's Wi-Fi, plus pairing).
//
// Pairing (USB only):  WF|<ssid hex>|<password hex>|<key hex, 32 bytes>  ->  WF|ok|<host>      WF|off forgets it
// Status:              WI  ->  WI|<on Wi-Fi 0/1>|<ip>|<host>|<computer linked 0/1>
// On each TCP connection the pad sends AU|<nonce>; the computer answers AU|<HMAC(key, "bridge|"+nonce)>|<its nonce>;
// the pad checks it and answers AU|<HMAC(key, "pad|"+its nonce)>, which the computer checks. Until then the pad
// ignores the connection, so nothing else on the network can show things on it or press its keys.
// Then every line both ways ends with |#<tag>, signed with a key made from both nonces and counted, so a device
// in the middle can neither change, add, replay nor drop a line (a key press that approves) unnoticed.
// Replies go to whichever link (USB or Wi-Fi) last sent a line.
#pragma once
#include <WiFi.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include "mbedtls/md.h"
#include "esp_random.h"

void handle(char *text);                    // burnbot_pad.ino

namespace wl {
const uint16_t PORT = 4751;
const uint32_t AUTH_MS = 5000;              // to prove itself, or the connection is closed
const uint32_t QUIET_MS = 20000;            // a linked computer this quiet may be replaced by another

inline String ssid, pass;
inline uint8_t key[32];
inline bool paired = false, serving = false, authed = false, active = false;
inline WiFiServer server(PORT);
inline WiFiClient client;
inline uint32_t connectedAt = 0, heardAt = 0;
inline char nonce[33], host[24], line[2048], out[2048];
inline size_t length = 0, outLength = 0;
// What has come in over Wi-Fi and is not handled yet. The chip's own TCP buffer holds about 0.13 s of sound, and
// a frame takes longer than that to draw, so pump() empties it into this one between the strips of a frame.
const size_t BOX = 32768;
inline uint8_t *box = nullptr;
inline size_t boxIn = 0, boxOut = 0;
inline uint8_t session[32];                 // per connection, from both nonces: signs every line after the handshake
inline uint32_t sentN = 0, takenN = 0;      // lines signed each way, so none can be replayed or dropped unnoticed

inline void hex(const uint8_t *b, size_t n, char *out) {
  for (size_t i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", b[i]);
  out[2 * n] = 0;
}
inline int unhex(const char *s, size_t len, uint8_t *out, size_t most) {   // bytes, or -1
  if (len % 2 || len / 2 > most) return -1;
  for (size_t i = 0; i < len / 2; i++) {
    char two[3] = {s[2 * i], s[2 * i + 1], 0};
    if (!isxdigit(two[0]) || !isxdigit(two[1])) return -1;
    out[i] = (uint8_t)strtol(two, nullptr, 16);
  }
  return (int)(len / 2);
}
inline void proof(const char *who, const char *n, char *out) {   // HMAC-SHA256(key, who|n) as 64 hex
  String m = String(who) + "|" + n;
  uint8_t d[32];
  mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), key, 32, (const uint8_t *)m.c_str(), m.length(), d);
  hex(d, 32, out);
}
// A line's signature: the first 8 bytes of HMAC-SHA256(session, who|n|line), as 16 hex.
inline void tag(const char *who, uint32_t n, const char *text, size_t len, char *hex16) {
  const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_context_t c;
  char head[16];
  int h = snprintf(head, sizeof head, "%s|%lu|", who, (unsigned long)n);
  uint8_t d[32];
  mbedtls_md_init(&c);
  mbedtls_md_setup(&c, md, 1);
  mbedtls_md_hmac_starts(&c, session, sizeof session);
  mbedtls_md_hmac_update(&c, (const uint8_t *)head, h);
  mbedtls_md_hmac_update(&c, (const uint8_t *)text, len);
  mbedtls_md_hmac_finish(&c, d);
  mbedtls_md_free(&c);
  hex(d, 8, hex16);
}
inline bool same(const char *a, const char *b) {   // in constant time
  size_t n = strlen(b);
  if (strlen(a) != n) return false;
  uint8_t diff = 0;
  for (size_t i = 0; i < n; i++) diff |= a[i] ^ b[i];
  return !diff;
}

inline void drop() {
  client.stop();
  authed = active = false;
  length = outLength = 0;
  boxIn = boxOut = 0;
}

inline void pump() {
  int n;
  while (box && client && (n = client.available()) > 0) {
    size_t at = boxIn % BOX, room = BOX - (boxIn - boxOut);
    size_t most = min((size_t)n, min(room, BOX - at));
    if (!most) return;
    int got = client.read(box + at, most);
    if (got <= 0) return;
    boxIn += got;
  }
}

inline void start() {
  drop();
  serving = false;
  if (!ssid.length() || !paired) {
    WiFi.mode(WIFI_OFF);
    return;
  }
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(host);
  WiFi.setSleep(true);                      // modem sleep: much less battery, still fine for sound
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid.c_str(), pass.c_str());
}

// While sound plays or records the radio stays awake: asleep, sound arrives in bursts too small to keep the
// speaker fed (measured: 9 s of speech took 12 s). The rest of the time it naps, for the battery.
inline void awake(bool on) {
  static bool is = false;
  if (on == is || !serving) return;
  is = on;
  WiFi.setSleep(!on);
}

inline void load() {
  uint64_t mac = ESP.getEfuseMac();
  snprintf(host, sizeof host, "burnbot-%02x%02x", (uint8_t)(mac >> 32), (uint8_t)(mac >> 40));
  box = (uint8_t *)ps_malloc(BOX);
  Preferences p;
  p.begin("burnbot", true);
  ssid = p.getString("ssid", "");
  pass = p.getString("pass", "");
  paired = p.getBytes("key", key, sizeof key) == sizeof key;
  p.end();
  start();
}

// WF|... from USB. Answers what it did.
inline void pair(char *t, Print &out) {
  Preferences p;
  if (!strcmp(t, "off")) {
    p.begin("burnbot", false);
    p.remove("ssid");
    p.remove("pass");
    p.remove("key");
    p.end();
    ssid = pass = "";
    paired = false;
    start();
    out.println("WF|off");
    return;
  }
  char *a = t, *b = strchr(a, '|'), *c = b ? strchr(b + 1, '|') : nullptr;
  uint8_t s[33] = {0}, w[65] = {0}, k[32];
  if (!b || !c || unhex(a, b - a, s, 32) < 1 || unhex(b + 1, c - b - 1, w, 64) < 0 || unhex(c + 1, strlen(c + 1), k, 32) != 32) {
    out.println("WF|bad");
    return;
  }
  ssid = (char *)s;
  pass = (char *)w;
  memcpy(key, k, sizeof key);
  paired = true;
  p.begin("burnbot", false);
  p.putString("ssid", ssid);
  p.putString("pass", pass);
  p.putBytes("key", key, sizeof key);
  p.end();
  WiFi.disconnect();
  start();
  out.printf("WF|ok|%s\n", host);
}

inline void status(Print &out) {
  bool up = WiFi.status() == WL_CONNECTED;
  out.printf("WI|%d|%s|%s|%d\n", up ? 1 : 0, up ? WiFi.localIP().toString().c_str() : "", host, authed && client.connected() ? 1 : 0);
}

// A line from the computer over Wi-Fi.
// After the handshake each line ends with |#<tag> (see tag()); a line with a wrong or missing tag ends the link.
inline void take(char *t, uint32_t now) {
  if (authed) {
    size_t n = strlen(t);
    char want[17];
    if (n < 18 || t[n - 18] != '|' || t[n - 17] != '#') return drop();
    tag("b", takenN++, t, n - 18, want);
    if (!same(t + n - 16, want)) return drop();
    t[n - 18] = 0;
    heardAt = now;
    active = true;
    handle(t);
    return;
  }
  char *bar = !strncmp(t, "AU|", 3) ? strchr(t + 3, '|') : nullptr;
  if (!bar || bar - t - 3 != 64 || strlen(bar + 1) != 32) return drop();
  *bar = 0;
  char want[65];
  proof("bridge", nonce, want);
  if (!same(t + 3, want)) return drop();
  proof("pad", bar + 1, want);
  client.printf("AU|%s\n", want);
  String s = String("session|") + nonce + "|" + (bar + 1);
  mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), key, 32, (const uint8_t *)s.c_str(), s.length(), session);
  sentN = takenN = 0;
  authed = true;
  heardAt = now;
}

// What the pad says over Wi-Fi: gathered into whole lines, each sent with its tag.
inline size_t send(const uint8_t *b, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (b[i] != '\n') {
      if (outLength < sizeof out - 1) out[outLength++] = (char)b[i];
      continue;
    }
    char t[17];
    tag("p", sentN++, out, outLength, t);
    client.write((const uint8_t *)out, outLength);
    client.printf("|#%s\n", t);
    outLength = 0;
  }
  return n;
}

inline void poll(uint32_t now) {
  if (!paired || !ssid.length()) return;
  if (WiFi.status() != WL_CONNECTED) {
    if (client) drop();
    serving = false;
    return;
  }
  if (!serving) {
    server.begin();
    server.setNoDelay(true);
    MDNS.end();
    MDNS.begin(host);
    MDNS.addService("burnbot", "tcp", PORT);
    serving = true;
  }
  WiFiClient fresh = server.accept();
  if (fresh) {
    if (authed && client.connected() && now - heardAt < QUIET_MS) {
      fresh.stop();                         // one computer at a time
    } else {
      drop();
      client = fresh;
      client.setNoDelay(true);
      connectedAt = now;
      uint8_t r[16];
      esp_fill_random(r, sizeof r);
      hex(r, sizeof r, nonce);
      client.printf("AU|%s\n", nonce);
    }
  }
  if (!client) return;
  if (!client.connected() || (!authed && now - connectedAt > AUTH_MS)) return drop();
  if (authed && now - heardAt > QUIET_MS) return drop();   // the computer slept or left: do not block on it
  pump();
  while (client && boxOut != boxIn) {
    char c = (char)box[boxOut++ % BOX];
    if (c == '\n' || c == '\r') {
      line[length] = 0;
      size_t had = length;
      length = 0;
      if (had) take(line, now);
    } else if (length < sizeof line - 1) {
      line[length++] = c;
    }
  }
}
}  // namespace wl

// What the pad says goes to the computer on the link it last heard from.
struct Link : public Print {
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t *b, size_t n) override {
    if (wl::active && wl::authed && wl::client.connected()) return wl::send(b, n);
    return Serial.write(b, n);
  }
};
inline Link computer;
