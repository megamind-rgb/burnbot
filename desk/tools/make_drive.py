"""Builds the pad's USB drive (a small read-only FAT disk with the Mac and Windows installers) into
desk/burnbot_pad/drive.h. Run it after changing anything in pad/:   python desk/tools/make_drive.py
Needs pyfatfs (pip install pyfatfs).
"""
import os, tempfile
import fs
from pyfatfs.PyFat import PyFat

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
PAD = os.path.join(ROOT, "pad")
OUT = os.path.join(ROOT, "desk", "burnbot_pad", "drive.h")
FILES = ["START HERE - Install on Windows.cmd", "START HERE - Install on Mac.command", "README.txt", "install.py", "bridge.py",
         "metrics.py", "voice.py", "hook.py", "autorun.inf", "burnbot.ico"]   # autorun.inf: Windows' drive icon and name
EXTRA = {".VolumeIcon.icns": os.path.join(ROOT, "app", "Burnbot.icns")}          # the Mac's drive icon
SECTORS = 1024                              # 512 KB; only the used part goes into the firmware

img = os.path.join(tempfile.mkdtemp(), "drive.img")
with open(img, "wb") as f:
    f.truncate(SECTORS * 512)
pf = PyFat()
pf.mkfs(img, fat_type=PyFat.FAT_TYPE_FAT12, size=SECTORS * 512, label="BURNBOT")
pf.close()
with fs.open_fs(f"fat://{img}") as disk:
    for name in FILES:
        with open(os.path.join(PAD, name), "rb") as src:
            disk.writebytes("/" + name, src.read())
    for name, path in EXTRA.items():
        disk.writebytes("/" + name, open(path, "rb").read())
    assert sorted(disk.listdir("/")) == sorted(FILES + list(EXTRA))   # self-check: long names survived
data = open(img, "rb").read()
used = len(data.rstrip(b"\0"))
used = -(-used // 512) * 512
with open(OUT, "w") as h:
    h.write("// The pad's USB drive, made by desk/tools/make_drive.py from pad/. Do not edit.\n#pragma once\n")
    h.write(f"const uint32_t DRIVE_SECTORS = {SECTORS};\nconst uint32_t DRIVE_USED = {used};\n")
    h.write("const uint8_t DRIVE[] = {\n")
    for i in range(0, used, 32):
        h.write(",".join(str(b) for b in data[i:i + 32]) + ",\n")
    h.write("};\n")
print(f"{OUT}: {used // 1024} KB used of {SECTORS // 2} KB")
