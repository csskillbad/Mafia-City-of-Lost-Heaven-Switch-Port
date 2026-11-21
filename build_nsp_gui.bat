@echo off
rem The Familia - Switch NSP builder (Windows). Needs Python 3 and Docker Desktop.
cd /d "%~dp0"
where py >nul 2>nul && (py -3 tools\nsp_builder.py & goto :eof)
where python >nul 2>nul && (python tools\nsp_builder.py & goto :eof)
echo Python 3 is not installed. Get it from https://www.python.org/downloads/
echo (tick "Add python.exe to PATH" in the installer), then run this again.
pause
