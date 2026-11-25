@echo off
rem Burnbot Pad installer for Windows. Needs nothing beforehand: it gets uv (github.com/astral-sh/uv),
rem which brings its own Python, for this user only (no admin).
cd /d "%~dp0"
set UV=%USERPROFILE%\.local\bin\uv.exe
if not exist "%UV%" (
  echo Getting uv, which brings Python ...
  powershell -NoProfile -ExecutionPolicy Bypass -Command "irm https://astral.sh/uv/install.ps1 | iex"
)
"%UV%" run --no-project --python 3.12 install.py %*
pause
