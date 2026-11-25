BURNBOT PAD - works with Claude Code on Windows and Mac
======================================================

1. Double-click "START HERE - Install on Windows" (or "... on Mac")
   (Windows may ask about the file: More info > Run anyway. Mac: right-click > Open the first time.)
2. Wait until it says Done (a few minutes the first time: it downloads the speech parts).
3. That's it. From now on the pad shows your level, usage and today whenever it's plugged in.

Wi-Fi: the installer offers to put the pad on your Wi-Fi, so it works without the cable (it pairs over USB first).
       Later: run the installer with --wifi (or --wifi off). Updates and pairing always need the cable.

Keys: 1 and 4 flip pages. When Claude asks to run something: 1 = Deny, 4 = Approve.
Hold 2 to talk to your latest Claude session; its answer is read out on the pad.

Remove:  run the installer with --uninstall  (py install.py --uninstall  /  python3 install.py --uninstall)
Log:     ~/.claude/burnbot-pad/bridge.log
Speech:  "voice_model": "base" (fast, default) or "small" (better for Lithuanian), in the same config.json
Volume:  "pad_volume" (0-100, default 78) in ~/.claude/burnbot-pad/config.json
Voice:   set "voice_name" in ~/.claude/burnbot-pad/config.json (e.g. "en-GB-SoniaNeural", "lt-LT-LeonasNeural")
