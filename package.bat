@echo off
rem Builds the proxy and assembles release\ - exactly what a player copies into the game
rem folder. Everything except the DLL comes from package\; release\ is wiped first, so a
rem file only placed there does not survive the next build.
setlocal
cd /d "%~dp0" || exit /b 1

call "%~dp0build.bat" release || exit /b 1

set "OUT=%~dp0release"
if exist "%OUT%" rmdir /s /q "%OUT%"
mkdir "%OUT%" || exit /b 1

copy /y "%~dp0build\bin\release\d3d9.dll"     "%OUT%\" >nul || exit /b 1
copy /y "%~dp0package\remix-comp-proxy.ini"   "%OUT%\" >nul || exit /b 1
rem The Remix settings and texture tags this game needs.
copy /y "%~dp0package\rtx.conf"               "%OUT%\" >nul || exit /b 1
rem The Remix mod: emissive maps for the fire and flame frames and the material overrides
rem for water, glass and chrome, as one replacement layer (mod.usd) with its textures.
xcopy "%~dp0package\rtx-remix" "%OUT%\rtx-remix\" /e /i /q /y >nul || exit /b 1
rem The bridge keeps the Remix API switched off unless .trex\bridge.conf turns it on, and
rem no Remix release ships that file - without it the headlights have no API to call.
mkdir "%OUT%\.trex" || exit /b 1
copy /y "%~dp0package\.trex\bridge.conf"      "%OUT%\.trex\" >nul || exit /b 1
rem Not README.txt: the GOG game folder has a ReadMe.txt, and Windows would treat the two
rem as the same file, so copying the release in would overwrite the game's.
copy /y "%~dp0package\carma2-rtx_README.txt"  "%OUT%\" >nul || exit /b 1
rem What passing the DLL on requires: our own licence, and the notices of the code
rem compiled into it.
copy /y "%~dp0LICENSE"                        "%OUT%\" >nul || exit /b 1
copy /y "%~dp0THIRD_PARTY_NOTICES.md"         "%OUT%\" >nul || exit /b 1

echo.
echo Release assembled in %OUT%
dir /b /a "%OUT%"
