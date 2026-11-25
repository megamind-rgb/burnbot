"""Speech for Burnbot Pad, copied as-is from the Claude Pad kit (claude_pad_kit/software/pad_voice.py):
sending words to a session, reading its reply, the computer's voice, and Whisper.
"""
import json, os, re, socket, subprocess, sys, tempfile, threading, time

DATA = os.path.join(os.path.expanduser("~"), ".claude", "burnbot-pad")
RATE = 16000


def cfg():
    try:
        return json.load(open(os.path.join(DATA, "config.json"), encoding="utf-8"))
    except Exception:
        return {}


def deliver(target, text):
    """Post one line of text into the session's message socket. Returns "" or an error text."""
    if not target.get("sock") or not target.get("token"):
        return "No message socket known for this session yet. Send it one prompt on the PC first."
    line = "[Spoken by the user through the Claude pad] " + " ".join(text.split())
    try:
        auth = (json.dumps({"type": "auth", "token": target["token"]}) + "\n").encode("utf-8")
        if not target["sock"].startswith("\\\\"):         # macOS / Linux: a Unix socket, not a Windows named pipe
            msg = json.dumps({"type": "user", "message": {"role": "user", "content": line}}) + "\n"
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as c:
                c.settimeout(5)
                c.connect(target["sock"])
                c.sendall(auth + msg.encode("utf-8"))
                time.sleep(0.3)
            return ""
        with open(target["sock"], "r+b", buffering=0) as f:
            f.write(auth)
            # the frame Claude Code names in its own messaging hint: one JSON user-message line
            f.write((json.dumps({"type": "user", "message": {"role": "user", "content": line}}) + "\n").encode("utf-8"))
            time.sleep(0.3)
        return ""
    except OSError as ex:
        return "Could not reach the session: " + str(ex)[:80]


def last_reply(transcript, offset):
    """The text of the last assistant message written to the transcript after `offset` (the turn's final answer)."""
    try:
        with open(transcript, "rb") as f:
            f.seek(offset)
            data = f.read()
    except OSError:
        return ""
    text = ""
    for line in data.decode("utf-8", "replace").splitlines():
        try:
            d = json.loads(line)
        except Exception:
            continue
        if d.get("type") != "assistant" or d.get("isSidechain"):
            continue
        blocks = (d.get("message") or {}).get("content") or []
        t = "\n".join(b.get("text", "") for b in blocks if isinstance(b, dict) and b.get("type") == "text").strip()
        if t:
            text = t
    return text


def speakable(text, limit):
    """Markdown answer -> plain sentences a voice can read, cut at a sentence end near `limit` characters."""
    text = re.sub(r"```.*?```", " (code omitted) ", text, flags=re.S)
    text = "\n".join(l for l in text.splitlines() if not l.lstrip().startswith("|"))      # drop tables
    text = re.sub(r"\[([^\]]+)\]\([^)]+\)", r"\1", text)                                    # links -> their text
    text = re.sub(r"[`*_#>]+", "", text)
    text = re.sub(r"^\s*[-\u2022]\s+", "", text, flags=re.M)
    text = " ".join(text.split())
    if len(text) > limit:
        cut = max(text.rfind(". ", 0, limit), text.rfind("? ", 0, limit), text.rfind("! ", 0, limit))
        text = text[:cut + 1 if cut > limit * 0.5 else limit] + " The rest is on your screen."
    return text


def tts(text):
    """Text -> 16 kHz 16-bit mono PCM in a natural Microsoft voice (edge-tts, needs internet); offline, the system voice.
    Pick another voice in config.json: "voice_name" (list them: python -m edge_tts --list-voices)."""
    try:
        import asyncio, io, av, edge_tts
        mp3 = io.BytesIO()

        async def get():
            async for c in edge_tts.Communicate(text, cfg().get("voice_name", "en-US-AndrewMultilingualNeural")).stream():
                if c["type"] == "audio":
                    mp3.write(c["data"])
        asyncio.run(asyncio.wait_for(get(), 8))   # the service can stall: then the system voice speaks
        mp3.seek(0)
        out = bytearray()
        rs = av.AudioResampler(format="s16", layout="mono", rate=RATE)
        with av.open(mp3) as f:
            for frame in f.decode(audio=0):
                for r in rs.resample(frame):
                    out += bytes(r.planes[0])[:r.samples * 2]
        if out:
            return bytes(out)
    except Exception as ex:
        print("voice: natural voice failed, using the system one:", ex, flush=True)
    return tts_offline(text)


def tts_offline(text):
    """Text -> 16 kHz 16-bit mono PCM: the Windows speech voice (System.Speech), or `say` on macOS."""
    d = tempfile.mkdtemp(prefix="padtts")
    txt, wav = os.path.join(d, "say.txt"), os.path.join(d, "say.wav")
    with open(txt, "w", encoding="utf-8") as fh:
        fh.write(text)
    rate = int(cfg().get("voice_reply_rate", 1))
    if sys.platform == "darwin":
        subprocess.run(["say", "-r", str(175 + 15 * rate), "-o", wav, "--data-format=LEI16@16000", "-f", txt],
                       timeout=90, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        ps = None
    else:
        ps = ("Add-Type -AssemblyName System.Speech;"
          "$s = New-Object System.Speech.Synthesis.SpeechSynthesizer;"
          "$f = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo(16000,"
          "[System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen,[System.Speech.AudioFormat.AudioChannel]::Mono);"
          f"$s.Rate = {rate}; $s.SetOutputToWaveFile('{wav}', $f);"
          f"$s.Speak([System.IO.File]::ReadAllText('{txt}', [System.Text.Encoding]::UTF8)); $s.Dispose()")
    if ps:
        subprocess.run(["powershell", "-NoProfile", "-Command", ps], timeout=90, creationflags=0x08000000,
                       stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        data = open(wav, "rb").read()
    finally:
        for p in (txt, wav):
            try:
                os.remove(p)
            except OSError:
                pass
        try:
            os.rmdir(d)
        except OSError:
            pass
    i = data.find(b"data")
    return data[i + 8:] if i >= 0 else b""


class Whisper:
    model, model_err, loading = None, "", False

    def load_model(self):
        if self.model or self.loading:
            return
        self.loading = True

        def run():
            try:
                from faster_whisper import WhisperModel
                self.model = WhisperModel(cfg().get("voice_model", "base"), device="cpu", compute_type="int8")
                print(time.strftime("%H:%M:%S"), "voice: speech model loaded", flush=True)
            except Exception as ex:
                self.model_err = str(ex)[:120]
                print("voice: model load failed:", ex, flush=True)
            self.loading = False
        threading.Thread(target=run, daemon=True).start()

    def transcribe(self, pcm):
        import numpy as np
        t0 = time.time()
        while self.loading and time.time() - t0 < 600:      # first use downloads the model
            time.sleep(0.2)
        if not self.model:
            return "", "Speech model not available: " + (self.model_err or "still loading")
        a = np.frombuffer(bytes(pcm), dtype=np.int16).astype(np.float32)
        a -= a.mean()
        peak = float(np.abs(a).max()) if len(a) else 0.0
        if peak < 150:
            return "", "Heard nothing. Hold the button and speak closer."
        a = a / max(peak, 1.0) * 0.9
        lang = cfg().get("voice_language", "auto")
        segs, info = self.model.transcribe(a, language=None if lang == "auto" else lang, beam_size=5,
                                           condition_on_previous_text=False)
        text = " ".join(s.text.strip() for s in segs).strip()
        print(time.strftime("%H:%M:%S"), f"voice: {len(a) / RATE:.1f}s peak={peak:.0f} lang={info.language} "
              f"took={time.time() - t0:.1f}s text={text!r}", flush=True)
        return text, "" if text else "Could not make out any words."

