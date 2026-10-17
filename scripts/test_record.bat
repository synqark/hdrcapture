@echo off
rem 5•b‘Ò‚Á‚Ä‚©‚ç20•bŠÔ˜^‰æ‚µ‚Ü‚·i‚»‚ÌŠÔ‚ÉƒQ[ƒ€‰æ–Ê‚ÖØ‚è‘Ö‚¦‚éjB
rem Œ‹‰Ê‚Í tests\ ‚Éo—Í‚³‚ê‚Ü‚·B
cd /d "%~dp0.."
if not exist tests mkdir tests
echo 5•bŒã‚É20•bŠÔ˜^‰æ‚µ‚Ü‚·B¡‚Ì‚¤‚¿‚ÉƒQ[ƒ€‰æ–Ê‚ÉØ‚è‘Ö‚¦‚Ä‚­‚¾‚³‚¢B
timeout /t 5 /nobreak >nul
build\hdrrec.exe -t 20 -o tests\test.mkv -timelog tests\test_times.csv %* > tests\test.log 2>&1
echo exit code %ERRORLEVEL% >> tests\test.log
rem hdrrec writes UTF-8: show it with code page 65001, then restore the console code page
for /f "tokens=2 delims=:." %%a in ('chcp') do set /a OLDCP=%%a
chcp 65001 > nul
type tests\test.log
chcp %OLDCP% > nul
pause
