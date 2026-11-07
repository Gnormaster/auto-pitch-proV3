@echo off
setlocal
cd /d "%~dp0"
set "VSW=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSW%" goto novs
set "VS="
for /f "usebackq delims=" %%i in (`"%VSW%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not defined VS goto novs
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul

if not exist wv2\build\native\include\WebView2.h (
  echo WebView2 SDK wird geladen...
  powershell -NoProfile -Command "[Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12; Invoke-WebRequest -UseBasicParsing -Uri 'https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/1.0.2210.55' -OutFile 'wv2.zip'; Expand-Archive -Force -Path 'wv2.zip' -DestinationPath 'wv2'; Remove-Item 'wv2.zip'"
)
if not exist wv2\build\native\include\WebView2.h goto nosdk

rc /nologo /I Source /fo AutoPitchPro.res Source\ui.rc
if errorlevel 1 goto fail
cl /nologo /O2 /std:c++17 /EHsc /MT /LD /I wv2\build\native\include Source\plugin.cpp Source\editor_win.cpp Source\PitchEngine.cpp AutoPitchPro.res /FeAutoPitchPro.dll /link /DEF:Source\exports.def wv2\build\native\x64\WebView2LoaderStatic.lib ole32.lib user32.lib advapi32.lib shell32.lib shlwapi.lib version.lib
if errorlevel 1 goto fail
del /q plugin.obj editor_win.obj PitchEngine.obj AutoPitchPro.res AutoPitchPro.lib AutoPitchPro.exp 2>nul
echo.
echo Fertig: %~dp0AutoPitchPro.dll
echo Diese Datei in den VST-Ordner von FL Studio kopieren und dort "Find installed plugins" ausfuehren.
goto end
:novs
echo Visual Studio mit C++-Workload nicht gefunden.
goto fail
:nosdk
echo WebView2 SDK konnte nicht geladen werden (Internetverbindung pruefen).
goto fail
:fail
if /i not "%1"=="nopause" pause
exit /b 1
:end
if /i not "%1"=="nopause" pause
exit /b 0
