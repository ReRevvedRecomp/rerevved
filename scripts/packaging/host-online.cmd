@echo off
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0host-online.ps1" %*
if errorlevel 1 pause
