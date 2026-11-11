@echo off
setlocal
cd /d "%~dp0"
if not exist logs mkdir logs
set "SMRPG_REPLAY_RECORD=1"
set "SMRPG_REPLAY_DIR=%CD%\logs"
set "REPORT=logs\startup-%RANDOM%-%RANDOM%.log"
echo Super Mario RPG diagnostic launch > "%REPORT%"
echo Started: %DATE% %TIME% >> "%REPORT%"
ver >> "%REPORT%" 2>&1
"%~dp0SuperMarioRPGSNESRecomp.exe" %* >> "%REPORT%" 2>&1
set "RESULT=%ERRORLEVEL%"
echo Exit code: %RESULT% >> "%REPORT%"
echo Diagnostic output: %REPORT%
echo After a failure, close the game and run Collect diagnostics.cmd.
pause
exit /b %RESULT%
