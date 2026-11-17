@echo off
rem Usage: start_server.bat [port]   (default 7777)
setlocal enabledelayedexpansion
cd /d "%~dp0"
set PORT=%~1
if "%PORT%"=="" set PORT=7777
if not exist build\bin\game_server.exe (
  echo Server not built - building with build_script.sh ...
  where bash >nul 2>nul && bash build_script.sh
  if not exist build\bin\game_server.exe (
    echo Build failed or bash not found. Run build_script.sh from an MSYS2 shell.
    exit /b 1
  )
)
tasklist /v /fi "imagename eq game_server.exe" 2>nul | findstr /c:"StealTreasure Server :%PORT%" >nul && (
  echo Server already running on port %PORT%.
  exit /b 1
)
start "StealTreasure Server :%PORT%" /min build\bin\game_server.exe %PORT%
echo Server started (window "StealTreasure Server :%PORT%", minimized) on port %PORT%.
echo.
echo Friends on your LAN can join with one of:
for /f "tokens=2 delims=:" %%a in ('ipconfig ^| findstr /c:"IPv4"') do (
  set IP=%%a
  set IP=!IP: =!
  echo     !IP!:%PORT%
)
echo     (same PC: 127.0.0.1:%PORT%)
echo.
echo Stop with: stop_server.bat [port]   or   stop_server.bat all
endlocal
