@echo off
setlocal EnableDelayedExpansion
title Status Sphere Editor Server
cd /d "%~dp0.."

where python >nul 2>&1
if errorlevel 1 (
    echo Python nicht gefunden. Bitte Python 3.9+ installieren.
    pause
    exit /b 1
)

REM --- Virtuelle Umgebung ---
set "VENV_DIR=tools\editor\.venv"

echo Erstelle virtuelle Umgebung...
python -m venv "%VENV_DIR%"
call "%VENV_DIR%\Scripts\activate.bat"

echo Installiere Abhaengigkeiten...
pip install -q -r tools\editor\requirements.txt

echo.
echo ========================================
echo   Status Sphere Editor Server
echo   http://127.0.0.1:5000
echo ========================================
echo.

python tools\editor\server.py

REM --- Aufraeumen ---
echo.
echo Server beendet. Raeume venv auf...
call deactivate 2>nul
rmdir /s /q "%VENV_DIR%" 2>nul
echo Fertig.
pause
