"""Burnbot Pad bridge for macOS and Windows: what app/DeskScreen.swift, PadKeys.swift and PadPages.swift do
for the Claude Pad, without the Mac app. Started at login by install.py; log in ~/.claude/burnbot-pad/bridge.log.

Once a second it sends the pad its B line (level), every 10 s the U (plan usage) and D (today) lines.
Keys: Approve / Deny answer a permission prompt that hook.py hands over. Key 2 held: talk to your latest
session (Whisper on this computer); its reply is read out on the pad's speaker.
"""
import base64, getpass, glob, hashlib, hmac, json, os, queue, re, secrets, socket, subprocess, sys, threading, time
import serial
from serial.tools import list_ports

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import metrics, voice

DATA = voice.DATA
LETTERS = set("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -.,!%/:+&\"()?=_*#;<>@$[]~")
PAD_LETTERS = set("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-.,!%/:+&'\"()?=_*#;<>@$|[]~`^{}\\")


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


# ---------------------------------------------------------------- text the pad can draw (PadPages.swift)
def pad_text(text, limit):
    out = ""
    for c in str(text).upper():
        c = {"|": "/", "'": '"', "`": '"', "{": "(", "}": ")"}.get(c, c)
        c = c if c in LETTERS else " "
        if not (c == " " and out.endswith(" ")):
            out += c
    return out.strip()[:limit]


def duration(m):
    m = max(0, int(m))
    return f"{m // 60}H {m % 60}M" if m >= 60 else f"{m}M"


def tokens(n):
    for size, unit in ((1e9, "B"), (1e6, "M"), (1e3, "K")):
        if n >= size:
            return f"{n / size:.1f}{unit}" if n < size * 10 and unit != "B" else f"{n / size:.0f}{unit}" if unit != "B" else f"{n / size:.1f}B"
    return str(int(round(n)))


def rate(n):
    for size, unit, wide, mid in ((1e9, "B", "%.2f", "%.2f"), (1e6, "M", "%.0f", "%.1f"), (1e3, "K", "%.0f", "%.1f")):
        if n >= size:
            return ((wide if n >= size * 100 else mid if n >= size * 10 else "%.2f") % (n / size)) + unit
    return str(int(n))


# ---------------------------------------------------------------- the lines
def b_line(a, ask):
    s = a["state"]
    lv = s if s < 6 else -1
    t = time.localtime()
    f = ["B", f"s={s}", f"lv={lv}", "tp=5", "u=-1", f"i={a['intensity']}", f"f={round(a['flame'] * 100)}",
         "c=%d,%d,%d" % metrics.CUTS, "nm=" + metrics.NAMES[s], "cp=" + ("OFF LADDER" if lv < 0 else f"LEVEL {lv} OF 5"),
         "rt=" + rate(a["rate"]), f"tm={t.tm_hour:02d}:{t.tm_min:02d}", f"wk={a['streak']}"]
    line = "|".join(f)
    if ask and ask["whole"]:
        line += "|ky=DENY,,,APPROVE|pr=" + " ".join(ask["shown"].split()).replace("|", "\x7f") + "|pp=" + pad_text(ask["project"], 18)
    return line


def read_json(path):
    try:
        return json.load(open(path, encoding="utf-8")), os.path.getmtime(path)
    except Exception:
        return None, 0


def resets(at, now):
    left = at - now
    if left < 86400:
        return "IN " + duration(-(-left // 60))
    return time.strftime("%a %H:%M", time.localtime(at)).upper()


def u_line(now):
    f = ["U"]
    rates, mod = read_json(os.path.join(DATA, "rates.json"))
    limits = (rates or {}).get("rate_limits") or {}
    any_ = False
    for key, label in (("five_hour", "h"), ("seven_day", "w")):
        lim = limits.get(key) or {}
        used, at = lim.get("used_percentage"), float(lim.get("resets_at") or 0)
        if used is None or (0 < at <= now):
            continue
        f.append(f"{label}={round(used)}")
        if at > 0:
            f.append(f"{label}r=" + resets(at, now))
        any_ = True
    if any_:
        f.append(f"ag={max(0, int((now - mod) / 60))}")
    st, mod = read_json(os.path.join(DATA, "status.json"))
    used = ((st or {}).get("context_window") or {}).get("used_percentage")
    if st and now - mod < 12 * 3600 and used is not None:
        f.append(f"x={round(used)}")
        ws = st.get("workspace") or {}
        name = pad_text(os.path.basename((ws.get("project_dir") or ws.get("current_dir") or st.get("cwd") or "").rstrip("/\\")), 18)
        if name:
            f.append("xn=" + name)
    return "|".join(f)


def d_line(a):
    most = max(a["hours"])
    bars = [0 if h <= 0 or most <= 0 else max(1, round(h / most * 99)) for h in a["hours"]]
    f = ["D", "b=" + ",".join(map(str, bars)), "fm=" + duration(a["engaged"]), f"ss={a['sessions']}"]
    if a["peak"] > 0:
        x = a["peak"] / max(a["usual"], 1)
        f.append("pk=" + ((f"{x:.0f}X" if x >= 10 else f"{x:.1f}X") if a["usual"] > 0 else tokens(a["peak"]) + "/H"))
    return "|".join(f)


# ---------------------------------------------------------------- permission prompts (PadKeys.swift)
def fits(text, cols=25, rows=4):
    full = length = 0
    for w in text.split():
        if len(w) > cols:
            return False
        if length and length + 1 + len(w) > cols:
            full, length = full + 1, 0
        length += (1 if length else 0) + len(w)
    return full < rows


def request(raw):
    """What the pad shows for a permission prompt, and whether that is all of it (only then may it be approved)."""
    tool, inp, cwd = raw.get("tool_name", "?"), raw.get("tool_input") or {}, (raw.get("cwd") or "").replace("\\", "/")
    what, whole = "", True
    if isinstance(inp.get("command"), str):
        what = inp["command"]
        whole = "\n" not in what and "\r" not in what
    elif isinstance(inp.get("file_path") or inp.get("notebook_path") or inp.get("path"), str):
        path = (inp.get("file_path") or inp.get("notebook_path") or inp.get("path")).replace("\\", "/")
        if cwd and path.lower().startswith(cwd.lower() + "/"):
            what = path[len(cwd) + 1:]
        else:
            what, whole = path, False
    elif isinstance(inp.get("url"), str):
        what = inp["url"]
    elif inp:
        whole = False
    shown = (tool + ": " + what if what else tool).replace("\t", " ")
    if any(c != " " and c not in PAD_LETTERS and not ("a" <= c <= "z") for c in shown):
        whole = False
    shown = shown.upper()
    return shown, whole and fits(shown)


def pending_ask(now):
    newest = None
    for p in glob.glob(os.path.join(DATA, "asks", "*.json")):
        try:
            mod = os.path.getmtime(p)
            if now - mod > 65:
                os.remove(p)
                continue
            rid = os.path.basename(p)[:-5]
            if os.path.exists(os.path.join(DATA, "answers", rid + ".json")):
                continue
            raw = json.load(open(p, encoding="utf-8"))
        except Exception:
            continue
        if not newest or mod > newest["t"]:
            shown, whole = request(raw)
            newest = {"id": rid, "t": mod, "shown": shown, "whole": whole,
                      "project": os.path.basename((raw.get("cwd") or "").rstrip("/\\"))}
    return newest


def answer(ask, allow):
    if allow is None:
        body = ""                           # let go: Claude asks on the computer as usual
    else:
        dec = {"behavior": "allow"} if allow else {"behavior": "deny", "message": "Denied from the desk pad."}
        body = json.dumps({"hookSpecificOutput": {"hookEventName": "PermissionRequest", "decision": dec}})
    path = os.path.join(DATA, "answers", ask["id"] + ".json")
    with open(path + ".tmp", "w", encoding="utf-8") as fh:
        fh.write(body)
    os.replace(path + ".tmp", path)


# ---------------------------------------------------------------- the pad on USB (DeskScreen.swift)
def find_pad():
    if sys.platform == "darwin" and subprocess.run(["pgrep", "-x", "Burnbot"], capture_output=True).returncode == 0:
        return None                         # the Mac app is running and drives the pad itself
    return find_usb() or find_wifi()


def find_usb():
    for p in list_ports.comports():
        if p.vid != 0x303A:
            continue
        try:
            s = serial.Serial(p.device, 115200, timeout=0.1)
        except Exception:
            continue                        # busy: another program has it
        try:
            s.reset_input_buffer()
            s.write(b"\nV\n")
            end = time.time() + 1.5
            buf = b""
            while time.time() < end:
                buf += s.read(256)
                if b"BURNBOT" in buf:
                    log("pad found on", p.device)
                    return s
        except Exception:
            pass
        s.close()
    return None


# ---------------------------------------------------------------- the pad over Wi-Fi (desk/burnbot_pad/wifilink.h)
WIFI_PORT = 4751


def save_cfg(**changes):
    c = voice.cfg()
    c.update(changes)
    path = os.path.join(DATA, "config.json")
    with open(path + ".tmp", "w", encoding="utf-8") as f:
        json.dump(c, f, indent=2)
    os.replace(path + ".tmp", path)


def proof(key, who, nonce):
    return hmac.new(bytes.fromhex(key), f"{who}|{nonce}".encode(), hashlib.sha256).hexdigest()


class WifiPort:
    """The pad over Wi-Fi, with the few pyserial calls Pad uses. Every line both ways carries |#<tag>, signed
    with this connection's key and counted (wifilink.h), so nothing in between can add, change or replay one."""
    in_waiting = 0

    def __init__(self, sock, session, rest):
        self.s, self.k, self.sent, self.taken, self.buf = sock, session, 0, 0, rest
        sock.settimeout(0.1)

    def tag(self, who, n, data):
        return hmac.new(self.k, f"{who}|{n}|".encode() + data, hashlib.sha256).hexdigest()[:16].encode()

    def read(self, n):
        try:
            d = self.s.recv(4096)
            if not d:
                raise IOError("Wi-Fi link closed")
            self.buf += d
        except socket.timeout:
            pass
        *lines, self.buf = self.buf.split(b"\n")
        out = b""
        for l in lines:
            l = l.rstrip(b"\r")
            if not l:
                continue
            body, mark, t = l.rpartition(b"|#")
            if not mark or not hmac.compare_digest(t, self.tag("p", self.taken, body)):
                raise IOError("a line from the pad was not signed right; link dropped")
            self.taken += 1
            out += body + b"\n"
        return out

    def write(self, b):
        out = b""
        for l in b.split(b"\n"):
            if l:
                out += l + b"|#" + self.tag("b", self.sent, l) + b"\n"
                self.sent += 1
        self.s.sendall(out)

    def close(self):
        self.s.close()


def read_line(s, buf, end):
    """One short line from a socket, before `end` (a time), or OSError."""
    while b"\n" not in buf[0]:
        if time.time() > end or len(buf[0]) > 200:
            raise OSError("no proper answer")
        s.settimeout(max(0.1, end - time.time()))
        d = s.recv(256)
        if not d:
            raise OSError("closed")
        buf[0] += d
    line, _, buf[0] = buf[0].partition(b"\n")
    return line.decode(errors="replace").strip()


def find_wifi():
    """The paired pad on the network, after both sides prove they hold the key set over USB."""
    c = voice.cfg()
    key = c.get("pad_key")
    hosts = [h for h in (c.get("pad_ip"), c.get("pad_host") and c["pad_host"] + ".local") if h] if key else []
    for host in hosts:
        try:
            s = socket.create_connection((host, WIFI_PORT), timeout=2)
            s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)   # send each line at once: sound must not wait
            buf, end = [b""], time.time() + 5
            hello = read_line(s, buf, end)
            if not (hello.startswith("AU|") and len(hello) == 35):
                s.close()
                continue
            mine = secrets.token_hex(16)
            s.sendall(f"AU|{proof(key, 'bridge', hello[3:])}|{mine}\n".encode())
            if not hmac.compare_digest(read_line(s, buf, end), "AU|" + proof(key, "pad", mine)):
                s.close()
                log("a device at", host, "did not prove it is your pad; ignored")
                continue
            session = hmac.new(bytes.fromhex(key), f"session|{hello[3:]}|{mine}".encode(), hashlib.sha256).digest()
            ip = s.getpeername()[0]
            if ip != c.get("pad_ip"):
                save_cfg(pad_ip=ip)
            log("pad found over Wi-Fi at", ip)
            return WifiPort(s, session, buf[0])
        except (OSError, ValueError):
            pass
    return None


def this_wifi():
    """This computer's Wi-Fi name and saved password (either may be None)."""
    def run(*a):
        try:
            return subprocess.run(a, capture_output=True, text=True, timeout=30).stdout
        except Exception:
            return ""
    if os.name == "nt":
        name = re.search(r"^\s*SSID\s*:\s*(.+)$", run("netsh", "wlan", "show", "interfaces"), re.M)
        name = name and name.group(1).strip()
        pw = name and re.search(r"Key Content\s*: (.*)", run("netsh", "wlan", "show", "profile", f"name={name}", "key=clear"))
        return name, pw and pw.group(1).rstrip("\r")   # shortcut: English Windows only; others are asked to type it
    name = re.search(r"^\s*SSID\s*:\s*(.+)$", run("ipconfig", "getsummary", "en0"), re.M) \
        or re.search(r"Network:\s*(.+)", run("networksetup", "-getairportnetwork", "en0"))
    name = name and name.group(1).strip()
    if name and name != "<redacted>":       # macOS asks you before it shows the saved password
        return name, run("security", "find-generic-password", "-wa", name).rstrip("\n") or None
    return None, None


def wifi_setup():
    """Pairs the pad over USB and puts it on Wi-Fi: bridge.py --wifi (bridge.py --wifi off: forget it)."""
    usb = find_usb()
    if not usb:
        return print("Plug the pad in with a USB cable first (and close anything else using it).")
    if sys.argv[2:] == ["off"]:
        usb.write(b"\nWF|off\n")
        save_cfg(pad_key=None, pad_host=None, pad_ip=None)
        return print("The pad forgot its Wi-Fi.")
    name, pw = this_wifi()
    if name and input(f'Put the pad on this computer\'s Wi-Fi "{name}"? [Y/n] ').strip().lower() not in ("", "y", "yes"):
        name = pw = None
    name = name or input("Wi-Fi name (empty: skip): ").strip()
    if not name:
        return print("Skipped: the pad works over USB. Run this again any time for Wi-Fi.")
    pw = pw if pw is not None else getpass.getpass(f'Password for "{name}": ')
    key = voice.cfg().get("pad_key") or secrets.token_hex(32)
    usb.write(f"\nWF|{name.encode().hex()}|{pw.encode().hex()}|{key}\n".encode())
    host, ip, end, buf = None, None, time.time() + 25, b""
    while time.time() < end and not ip:
        buf += usb.read(4096)
        for l in buf.decode(errors="replace").splitlines():
            if l.startswith("WF|ok|"):
                host = l[6:].strip()
            elif l.startswith("WI|1|") and host:
                ip = l.split("|")[2]
        if host and not ip:
            usb.write(b"WI\n")
            time.sleep(1)
    if not host:
        return print("The pad did not take the Wi-Fi settings. Is the newest firmware on it?")
    save_cfg(pad_key=key, pad_host=host, pad_ip=ip)
    print(f"The pad is on Wi-Fi at {ip} ({host}.local). You can unplug the USB cable." if ip else
          f'The pad saved "{name}" but has not joined it yet. Check the password, then run this again.')


def screen(**f):
    """An S line; text fields made drawable."""
    out = ["S"]
    for k, v in f.items():
        if v is None:
            continue
        if k in ("t", "x", "to"):
            v = pad_text(v, {"t": 27, "x": 300, "to": 19}[k])
        out.append(f"{k}={v}")
    return "|".join(out)


def latest_session():
    best = None
    for p in glob.glob(os.path.join(DATA, "sessions", "*.json")):
        d, _ = read_json(p)
        if d and d.get("msg_socket") and (not best or d.get("updated", 0) > best.get("updated", 0)):
            best = d
    if best:
        best["title"] = best.get("project") or "Claude"
        best["id"], best["sock"], best["token"] = best["session_id"], best["msg_socket"], best.get("msg_token", "")   # voice.deliver's names
    return best


class Pad:
    def __init__(self, port):
        self.port, self.buf, self.sending = port, b"", threading.Lock()
        self.ask_id, self.ask_since = None, 0
        self.stage, self.s_line = None, ""   # voice: None (home), stt, rev, sent, say, err
        self.pcm, self.text, self.target, self.awaiting = bytearray(), "", None, None
        self.play, self.seq, self.unanswered, self.buffered, self.more = b"", 0, 0, 0, False
        self.lock = threading.Lock()        # the speech thread adds sound while the main loop sends it
        self.whisper = voice.Whisper()
        self.whisper.load_model()           # warm up now: first use otherwise waits ~4 s (Whisper) + ~8 s (voice)
        threading.Thread(target=voice.tts, args=("ok",), daemon=True).start()
        self.inbox = queue.Queue()
        # Read all the time: in USB OTG mode the pad's send buffer is tiny and drops what is not read at once.
        threading.Thread(target=self.reader, daemon=True).start()

    def reader(self):
        buf = b""
        try:
            while True:
                buf += self.port.read(self.port.in_waiting or 1)
                *done, buf = buf.split(b"\n")
                for l in done:
                    self.inbox.put(l.decode("utf-8", "replace").strip())
        except Exception as ex:
            self.inbox.put(ex)

    def send(self, line):
        with self.sending:                  # the main loop and the speech threads both send
            self.port.write((line + "\n").encode("utf-8", "replace"))

    def lines(self):
        out = []
        while not self.inbox.empty():
            l = self.inbox.get()
            if isinstance(l, Exception):
                raise l
            out.append(l)
        return out

    def home(self):
        self.stage, self.s_line = None, screen(m="home", mc=1, to=(self.target or {}).get("title"))

    def show(self, stage, **f):
        self.stage, self.s_line, self.at = stage, screen(**f), time.time()
        self.send(self.s_line)

    # -------------------------------------------- voice
    def heard(self, line, ask):
        parts = line.split("|")
        if line == "ERR" or parts[0] in ("BT", "Q", "AE"):
            log("pad:", line)
        if parts[0] == "AS":
            self.pcm = bytearray()
            self.target = latest_session()
            self.whisper.load_model()
        elif parts[0] == "A" and len(parts) == 4:
            self.pcm += base64.b64decode(parts[3])
        elif parts[0] == "AE" and parts[-1] in ("up", "max"):
            self.target = self.target or latest_session()   # the start line can be lost while the link changes
            if not self.target:
                return self.show("err", m="err", t="NO SESSION", x="Send your Claude session one prompt first.", ky="OK,,,")
            self.show("stt", m="stt", t="TO " + self.target["title"])
            threading.Thread(target=self.transcribe, daemon=True).start()
        elif parts[0] == "PK" and len(parts) == 3:
            self.unanswered, self.buffered, self.pk_at = max(0, self.unanswered - 1), int(parts[2]), time.time()
        elif parts[0] == "PD" and self.stage == "say":
            self.home()
        elif line in ("K1d", "K4d"):
            if ask and self.stage is None:
                if time.time() - self.ask_since < 1.5:
                    return                  # it may not be on the screen yet: a press must answer what you saw
                answer(ask, line == "K4d")
                log("prompt", "approved" if line == "K4d" else "denied", "on the pad:", ask["shown"][:60])
            elif self.stage == "rev" and line == "K4d":
                err = voice.deliver(self.target, self.text)
                log("voice: sent to", self.target["title"], "->", err or "ok")
                if err:
                    return self.show("err", m="err", t="NOT SENT", x=err, ky="OK,,,")
                try:
                    size = os.path.getsize(self.target["transcript"])
                except OSError:
                    size = 0
                self.awaiting = {"sid": self.target["id"], "t": time.time(), "worked": False, "offset": size,
                                 "transcript": self.target.get("transcript", ""), "title": self.target["title"]}
                self.show("sent", m="sent", t="SENT TO " + self.target["title"])
            elif self.stage == "say" and line == "K1d":
                with self.lock:
                    self.play, self.more = b"", False
                self.send("PX")
            elif self.stage in ("rev", "err") and line == "K1d":
                self.home()

    def transcribe(self):
        text, err = self.whisper.transcribe(self.pcm)
        if err:
            self.show("err", m="err", t="TRY AGAIN", x=err, ky="OK,,,")
        else:
            self.text = text
            self.show("rev", m="rev", t="TO " + self.target["title"], x=text, ky="CANCEL,,,SEND", kc="r--g")

    def reply(self, now):
        """voice.Voice.poll_reply from the Pad kit, on this bridge's session files."""
        a = self.awaiting
        if self.stage == "sent" and now - self.at > 3:
            self.home()
        if not a:
            return
        st, _ = read_json(os.path.join(DATA, "sessions", a["sid"] + ".json"))
        st = st or {}
        if st.get("state") == "working" and st.get("updated", 0) > a["t"] - 1:
            a["worked"] = True
        if now - a["t"] > 900:
            self.awaiting = None
        if not a["worked"] or st.get("state") == "working" or now - st.get("updated", now) < 0.8:
            return
        self.awaiting = None
        text = voice.speakable(voice.last_reply(a["transcript"], a["offset"]), int(voice.cfg().get("voice_reply_max", 600)))
        log(f"voice: {a['title']} answered, {len(text)} chars to speak")
        if not text:
            return

        def run():                          # the whole reply in one go: splitting it left a pause at the join
            t = time.time()
            pcm = voice.tts(text)
            with self.lock:
                self.play, self.seq, self.unanswered, self.buffered, self.more = pcm, 0, 0, 0, False
            log(f"voice: reply {len(text)} chars, speaking after {time.time() - t:.1f}s")
            self.show("say", m="say", t=a["title"], x=text, ky="STOP,,,")
        threading.Thread(target=run, daemon=True).start()

    def stream(self):
        """Sound to the speaker, 30 ms a line, paced like desk/board.sh pad-play: at most 6 lines unanswered
        and 800 ms ahead, since the pad drops sound that does not fit its 2 s buffer."""
        # Over Wi-Fi sound reaches the pad in bursts (its radio naps), so more is kept on the way; USB's 16 KB buffer takes 6.
        most = 6 if isinstance(self.port, serial.Serial) else 24
        while self.unanswered < most and self.buffered - (time.time() - getattr(self, "pk_at", 0)) * 1000 + self.unanswered * 30 < 800:
            with self.lock:
                if not self.play:
                    if self.seq and not self.more:
                        self.send("PE")
                        self.seq = 0
                    return
                chunk, self.play = self.play[:960], self.play[960:]
            self.send(f"PL|{self.seq + 1}|" + base64.b64encode(chunk).decode())
            self.seq += 1
            self.unanswered += 1


def run_pad(port, logs):
    pad = Pad(port)
    pad.home()
    pad.send("Q")
    pad.send(f"VL|{int(voice.cfg().get('pad_volume', 78))}")   # speaker volume 0-100
    a, ask, analyzed, slow, alive = None, None, 0, 0, os.path.join(DATA, "pad-alive")
    heard = time.time()
    while True:
        now = time.time()
        for line in pad.lines():
            if line:
                heard = now
                pad.heard(line, ask)
        pad.stream()
        if now - analyzed >= 1:
            if logs.poll() or not a or now - a["at"] >= 10:
                a = metrics.analyze(logs, now)
                a["at"] = now
            open(alive, "a").close()
            os.utime(alive)
            ask = pending_ask(now)
            if ask and not ask["whole"]:
                answer(ask, None)
                ask = None
            if (ask or {}).get("id") != pad.ask_id:
                pad.ask_id, pad.ask_since = (ask or {}).get("id"), now
            pad.send(b_line(a, ask))
            pad.send(pad.s_line)
            if now - slow >= 10:
                u = u_line(now)
                if u != "U":                # a bare "U" is an ERR on the pad: nothing known yet
                    pad.send(u)
                pad.send(d_line(a))
                slow = now
            pad.reply(now)
            analyzed = now
            if now - heard > 8:
                raise IOError("the pad went quiet")
        time.sleep(0.02)


def main():
    if sys.stdout is None:                  # pythonw on Windows: no window, so the log goes to its file
        sys.stdout = sys.stderr = open(os.path.join(DATA, "bridge.log"), "w", encoding="utf-8", buffering=1)
    try:                                    # one bridge at a time
        lock = socket.socket()
        lock.bind(("127.0.0.1", 47391))
    except OSError:
        return log("another bridge is already running")
    os.makedirs(os.path.join(DATA, "asks"), exist_ok=True)
    os.makedirs(os.path.join(DATA, "answers"), exist_ok=True)
    logs = metrics.Logs()
    log("bridge started; looking for the pad")
    while True:
        port = find_pad()
        if not port:
            time.sleep(4)
            continue
        try:
            run_pad(port, logs)
        except Exception as ex:
            log("pad gone:", ex)
        try:
            os.remove(os.path.join(DATA, "pad-alive"))
            port.close()
        except OSError:
            pass
        time.sleep(2)


if __name__ == "__main__":
    if sys.argv[1:2] == ["--wifi"]:
        wifi_setup()
    elif sys.argv[1:] == ["--check"]:       # self-check of the text rules
        assert pad_text("shop's  {x}|y", 18) == 'SHOP"S (X)/Y'
        assert request({"tool_name": "Bash", "tool_input": {"command": "npm test"}}) == ("BASH: NPM TEST", True)
        assert not request({"tool_name": "Bash", "tool_input": {"command": "a\nb"}})[1]
        assert not request({"tool_name": "Write", "tool_input": {"file_path": "/etc/x"}, "cwd": "/home/me"})[1]
        assert rate(28674264) == "28.7M" and rate(950) == "950" and fits("A " * 40) is True
        assert proof("00" * 32, "pad", "ab") == hmac.new(bytes(32), b"pad|ab", hashlib.sha256).hexdigest()
        a, b = socket.socketpair()          # a signed line goes through; a changed one ends the link
        pa, pb = WifiPort(a, b"k" * 32, b""), WifiPort(b, b"k" * 32, b"")
        pa.write(b"K4d\n")
        pb.tag = lambda who, n, d: hmac.new(b"k" * 32, f"{'b' if who == 'p' else 'p'}|{n}|".encode() + d, hashlib.sha256).hexdigest()[:16].encode()
        assert pb.read(1) == b"K4d\n"
        a.sendall(b"K4d|#0000000000000000\n")
        try:
            pb.read(1)
            raise AssertionError("forged line accepted")
        except IOError:
            pass
        print("ok")
    else:
        main()
