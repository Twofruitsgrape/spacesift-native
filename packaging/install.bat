@echo off
chcp 65001 >nul 2>&1
title SpaceSift installer
echo.
echo   SpaceSift - one click install (current user, no admin needed)
echo   -----------------------------------------------------------------
echo.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1"
echo.
pause
