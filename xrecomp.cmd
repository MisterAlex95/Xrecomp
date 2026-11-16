@echo off
rem xrecomp for Windows (XBA-89, experimental): the command line of docs/desktop/cli.md, Xbox games only.
rem
rem   xrecomp.cmd prepare C:\Games\MyGame\default.xbe
rem   xrecomp.cmd play 12345678
rem   xrecomp.cmd list
rem
rem Needs Python 3 (the py launcher, or python on PATH) with capstone, pefile and numpy, and a C compiler: Visual
rem Studio's Build Tools with « Desktop development with C++ » (cl), or LLVM (clang-cl). Outside a developer prompt,
rem this opens one for the run with vswhere and vcvars64.bat. docs/desktop/windows.md.
setlocal
set "XRECOMP_ROOT=%~dp0"
if not defined VCINSTALLDIR call :vsenv

rem The Python: the toolkit's .venv, else python on PATH (when it runs: not the Microsoft Store alias), else py -3.
set "XRECOMP_PY="
if exist "%XRECOMP_ROOT%.venv\Scripts\python.exe" set XRECOMP_PY="%XRECOMP_ROOT%.venv\Scripts\python.exe"
if not defined XRECOMP_PY python -c "" >nul 2>nul && set "XRECOMP_PY=python"
if not defined XRECOMP_PY set "XRECOMP_PY=py -3"
%XRECOMP_PY% "%XRECOMP_ROOT%xrecomp" %*
exit /b %ERRORLEVEL%

:vsenv
set "XRECOMP_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%XRECOMP_VSWHERE%" exit /b 0
set "XRECOMP_VS="
for /f "usebackq delims=" %%i in (`"%XRECOMP_VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "XRECOMP_VS=%%i"
if not defined XRECOMP_VS exit /b 0
if exist "%XRECOMP_VS%\VC\Auxiliary\Build\vcvars64.bat" call "%XRECOMP_VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
exit /b 0
