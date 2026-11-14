@echo off
chcp 65001 >nul 2>&1
title SpaceSift selftest
cd /d "%~dp0"
echo.
echo   SpaceSift self-test (zi jian)
echo   ------------------------------------------------------------------
echo   Builds 12 sample files in %%TEMP%%, extracts text from each format,
echo   builds an index, searches it, then cleans everything up.
echo   It does not touch any of your own files.
echo.
echo   Optionally health-check one folder:  selftest.bat "%%USERPROFILE%%\Documents"
echo   ------------------------------------------------------------------
echo.
"%~dp0SpaceSift.exe" --selftest %*
set RC=%ERRORLEVEL%
echo.
if "%RC%"=="0" (
  echo   RESULT: all checks passed - this package is usable.
) else (
  echo   RESULT: some checks FAILED ^(exit code %RC%^) - see the output above.
)
echo.
pause
