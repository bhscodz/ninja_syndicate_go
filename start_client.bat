@echo off
rem Usage: start_client.bat [host] [port] [name]
cd /d "%~dp0"
set H=%~1
set P=%~2
set N=%~3
if "%H%"=="" set H=127.0.0.1
if "%P%"=="" set P=7777
if "%N%"=="" set N=Player
if not exist build\bin\game_client.exe (echo Client not built. Run build_script.sh first.& exit /b 1)
build\bin\game_client.exe %H% %P% %N%
