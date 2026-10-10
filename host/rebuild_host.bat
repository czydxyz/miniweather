@echo off
rem ============================================================
rem  rebuild_host.bat  -  rebuild miniweather_host.exe
rem  Place this file in the "host" folder (next to the .pro).
rem  It rebuilds Release and copies the exe into host\bin.
rem ============================================================

set QT_ROOT=D:/Qt/Qt5.12.9/5.12.9/mingw73_64
set MW_ROOT=D:/Qt/Qt5.12.9/Tools/mingw730_64

if not exist "%QT_ROOT%\bin\qmake.exe" (
    echo [ERROR] qmake not found: %QT_ROOT%\bin\qmake.exe
    echo         Fix QT_ROOT at the top of this file.
    pause & exit /b 1
)

set PATH=%QT_ROOT%\bin;%MW_ROOT%\bin;%PATH%

cd /d "%~dp0"
if not exist build mkdir build
cd build

echo [1/3] qmake ...
qmake.exe ..\miniweather_host.pro "CONFIG+=release"
if errorlevel 1 ( echo BUILD FAILED at qmake & pause & exit /b 1 )

echo [2/3] mingw32-make ...
mingw32-make.exe -f Makefile.Release -j4
if errorlevel 1 ( echo BUILD FAILED at make & pause & exit /b 1 )

echo [3/3] copy exe to bin ...
copy /Y release\miniweather_host.exe ..\bin\miniweather_host.exe
if errorlevel 1 ( echo COPY FAILED & pause & exit /b 1 )

echo.
echo OK - bin\miniweather_host.exe updated
pause
