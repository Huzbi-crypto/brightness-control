@echo off
setlocal
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSPATH=%%i
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if not exist out mkdir out
cl /nologo /O2 /EHsc /W3 /DUNICODE /D_UNICODE /Fo:out\ /Fe:out\symtool.exe tools\symtool.cpp /link /SUBSYSTEM:CONSOLE
