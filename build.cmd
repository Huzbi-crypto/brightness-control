@echo off
setlocal
set CSC=%WINDIR%\Microsoft.NET\Framework64\v4.0.30319\csc.exe
if not exist "%CSC%" (
  echo .NET Framework 4.x compiler not found at %CSC%
  exit /b 1
)
"%CSC%" /nologo /target:winexe /optimize+ /out:BrightnessControl.exe ^
  /r:System.dll /r:System.Management.dll /r:System.Drawing.dll /r:System.Windows.Forms.dll ^
  src\Display.cs src\Controls.cs src\Hooks.cs src\QuickPanel.cs src\QuickSettings.cs src\Program.cs
if errorlevel 1 exit /b 1
echo Built BrightnessControl.exe
