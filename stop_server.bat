@echo off
rem Usage: stop_server.bat [port|all]
rem Default stops only the server started by start_server.bat on port 7777.
rem "all" kills EVERY game_server.exe.
setlocal
if /i "%~1"=="all" (
  taskkill /f /im game_server.exe >nul 2>nul && (echo Stopped all game_server.exe.) || (echo No game_server.exe running.)
  exit /b 0
)
set PORT=%~1
if "%PORT%"=="" set PORT=7777
tasklist /v /fi "imagename eq game_server.exe" 2>nul | findstr /c:"StealTreasure Server :%PORT%" >nul || (echo No StealTreasure server found on port %PORT%.& exit /b 0)
taskkill /f /fi "WINDOWTITLE eq StealTreasure Server :%PORT%" /im game_server.exe >nul 2>nul
echo Stopped server on port %PORT%.
endlocal
