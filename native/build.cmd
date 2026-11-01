@echo off
setlocal enabledelayedexpansion

set VSW="%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist %VSW% (
  echo vswhere not found - is Visual Studio Build Tools installed?
  exit /b 1
)

for /f "usebackq delims=" %%i in (`%VSW% -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSPATH=%%i
if not defined VSPATH (
  echo MSVC C++ tools not found - install the "Desktop development with C++" workload.
  exit /b 1
)

call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
  echo vcvars64 failed
  exit /b 1
)

if not exist out mkdir out

echo Building bcdll.dll ...
cl /nologo /LD /O2 /EHsc /W3 /DUNICODE /D_UNICODE /Fo:out\ /Fe:out\bcdll.dll src\bcdll.cpp /link /DLL /SUBSYSTEM:WINDOWS user32.lib
if errorlevel 1 exit /b 1

echo Building bcinject.exe ...
cl /nologo /O2 /EHsc /W3 /DUNICODE /D_UNICODE /Fo:out\ /Fe:out\bcinject.exe src\injector.cpp /link /SUBSYSTEM:CONSOLE advapi32.lib
if errorlevel 1 exit /b 1

echo Building bcsync.exe ...
cl /nologo /O2 /EHsc /W3 /DUNICODE /D_UNICODE /Fo:out\ /Fe:out\bcsync.exe src\bcsync.cpp /link /SUBSYSTEM:CONSOLE user32.lib dxva2.lib
if errorlevel 1 exit /b 1

echo.
echo Built:
dir /b out\*.dll out\*.exe
