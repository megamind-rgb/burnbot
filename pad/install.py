"""Burnbot Pad installer for Windows and macOS (from the Claude Pad kit's install.py).

  Double-click "START HERE - Install on Windows" or "... on Mac": they need no Python (uv brings it).
  --wifi        puts the pad (on USB) on Wi-Fi again, or: --wifi off
  --uninstall   takes the hooks, the status line and the autostart out again

It makes its own Python environment in ~/.claude/burnbot-pad, copies the bridge there, adds its hooks
to ~/.claude/settings.json (yours are kept; a backup is made once), and starts the bridge at login.
"""
import json, os, shutil, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
HOME = os.path.expanduser("~")
DATA = os.path.join(HOME, ".claude", "burnbot-pad")
SETTINGS = os.path.join(HOME, ".claude", "settings.json")
VENV = os.path.join(DATA, "venv")
WIN, MAC = os.name == "nt", sys.platform == "darwin"
VPY = os.path.join(VENV, "Scripts", "python.exe") if WIN else os.path.join(VENV, "bin", "python")
VPYW = os.path.join(VENV, "Scripts", "pythonw.exe") if WIN else VPY
STARTUP_DIR = os.path.join(os.environ.get("APPDATA", ""), "Microsoft", "Windows", "Start Menu", "Programs", "Startup")
STARTUP = os.path.join(STARTUP_DIR, "Burnbot Pad.lnk")
STARTUP_OLD = os.path.join(STARTUP_DIR, "Burnbot Pad.cmd")   # before 2026-11-25: showed a window
PLIST = os.path.join(HOME, "Library", "LaunchAgents", "com.burnbot.pad.plist")
FILES = ["bridge.py", "metrics.py", "voice.py", "hook.py"]
PACKAGES = ["pyserial", "numpy", "faster-whisper", "edge-tts"]
MARK = "burnbot-pad/hook.py"
HOOKS = [("UserPromptSubmit", "event", 10), ("Stop", "event", 10), ("SessionEnd", "event", 10),
         ("Notification", "event", 10), ("PermissionRequest", "ask", 60)]


def say(msg):
    print(msg, flush=True)


def cmd(job):
    return f'"{VPY}" "{os.path.join(DATA, "hook.py")}" {job}'.replace("\\", "/")


def ours(entry):
    return MARK in str(entry.get("command", "")).replace("\\", "/")


def settings():
    try:
        return json.load(open(SETTINGS, encoding="utf-8"))
    except FileNotFoundError:
        return {}


def save(d):
    os.makedirs(os.path.dirname(SETTINGS), exist_ok=True)
    json.dump(d, open(SETTINGS + ".tmp", "w", encoding="utf-8"), indent=2)
    os.replace(SETTINGS + ".tmp", SETTINGS)


def without_ours(d):
    for ev, entries in list(d.get("hooks", {}).items()):
        kept = []
        for e in entries:
            e = dict(e, hooks=[h for h in e.get("hooks", []) if not ours(h)])
            if e["hooks"]:
                kept.append(e)
        d["hooks"][ev] = kept
    if ours(d.get("statusLine") or {}):
        del d["statusLine"]
    return d


def install_hooks():
    d = settings()                          # a broken settings.json stops here, untouched
    backup = SETTINGS + ".before-burnbot-pad"
    if os.path.exists(SETTINGS) and not os.path.exists(backup):
        shutil.copy2(SETTINGS, backup)
    d = without_ours(d)
    for ev, job, timeout in HOOKS:
        d.setdefault("hooks", {}).setdefault(ev, []).append({"hooks": [{"type": "command", "command": cmd(job), "timeout": timeout}]})
    if "statusLine" not in d:               # yours is kept; then the pad's Usage page stays empty
        d["statusLine"] = {"type": "command", "command": cmd("status")}
    d["crossSessionInbound"] = "accept"     # lets the pad's spoken words reach a session
    save(d)
    say("Claude Code hooks added (" + SETTINGS + ")")


def stop_bridge():
    if WIN:
        ps = "Get-CimInstance Win32_Process | ? { $_.CommandLine -match 'burnbot-pad.bridge\\.py' } | % { Stop-Process -Id $_.ProcessId -Force }"
        subprocess.run(["powershell", "-NoProfile", "-Command", ps], capture_output=True)
    elif MAC:
        subprocess.run(["launchctl", "unload", PLIST], capture_output=True)
    time.sleep(1)


def autostart():
    bridge, logf = os.path.join(DATA, "bridge.py"), os.path.join(DATA, "bridge.log")
    if WIN:
        os.makedirs(STARTUP_DIR, exist_ok=True)
        if os.path.exists(STARTUP_OLD):
            os.remove(STARTUP_OLD)
        # A shortcut to pythonw: no window at login, nothing to close by accident
        ps = (f"$s = (New-Object -ComObject WScript.Shell).CreateShortcut('{STARTUP}'); $s.TargetPath = '{VPYW}'; "
              f"$s.Arguments = '\"{bridge}\"'; $s.WorkingDirectory = '{DATA}'; $s.Description = 'Burnbot Pad bridge'; $s.Save()")
        subprocess.run(["powershell", "-NoProfile", "-Command", ps], check=True)
        subprocess.Popen([VPYW, bridge], cwd=DATA, creationflags=0x00000008 | 0x00000200)   # detached
    elif MAC:
        os.makedirs(os.path.dirname(PLIST), exist_ok=True)
        with open(PLIST, "w", encoding="utf-8") as fh:
            fh.write(f"""<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>Label</key><string>com.burnbot.pad</string>
  <key>ProgramArguments</key><array><string>{VPY}</string><string>-u</string><string>{bridge}</string></array>
  <key>StandardOutPath</key><string>{logf}</string><key>StandardErrorPath</key><string>{logf}</string>
  <key>RunAtLoad</key><true/><key>KeepAlive</key><true/>
</dict></plist>
""")
        subprocess.run(["launchctl", "load", PLIST], capture_output=True)
    say("The bridge runs now and at every login (log: " + logf + ")")


def wifi(*extra):
    say("")
    say("Wi-Fi: so the pad works without the cable (it pairs with this computer over USB first).")
    subprocess.run([VPY, os.path.join(DATA, "bridge.py"), "--wifi", *extra])


def install():
    if sys.version_info < (3, 9):
        sys.exit("Python 3.9 or newer is needed.")
    stop_bridge()
    os.makedirs(DATA, exist_ok=True)
    if not os.path.exists(VPY):
        say("Making Burnbot Pad's own Python environment ...")
        subprocess.run([sys.executable, "-m", "venv", VENV], check=True)
    say("Installing " + ", ".join(PACKAGES) + " (a few minutes the first time) ...")
    subprocess.run([VPY, "-m", "pip", "install", "-q", "--upgrade"] + PACKAGES, check=True)
    for f in FILES:
        shutil.copy2(os.path.join(HERE, f), os.path.join(DATA, f))
    install_hooks()
    wifi()
    autostart()
    say("\nDone. Plug in the pad; it shows your level within a few seconds. New Claude sessions get the keys and voice.")


def uninstall():
    stop_bridge()
    save(without_ours(settings()))
    for p in (STARTUP, STARTUP_OLD, PLIST):
        if os.path.exists(p):
            os.remove(p)
    say("Burnbot Pad's hooks and autostart were removed. Its folder stays: " + DATA)


if __name__ == "__main__":
    if "--check" in sys.argv:               # self-check: our hooks come out again, yours stay
        d = {"hooks": {"Stop": [{"hooks": [{"command": "mine"}, {"command": cmd("event")}]}]}, "statusLine": {"command": cmd("status")}}
        assert without_ours(d) == {"hooks": {"Stop": [{"hooks": [{"command": "mine"}]}]}}
        print("ok")
    elif "--wifi" in sys.argv:
        stop_bridge()
        wifi(*sys.argv[sys.argv.index("--wifi") + 1:])
        autostart()
    elif "--uninstall" in sys.argv:
        uninstall()
    else:
        install()
