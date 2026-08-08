@echo off
REM Build the permadeath DLL with MSVC. Run from this directory.
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
  echo FAILED to init MSVC environment
  exit /b 1
)
cd /d "%~dp0"
if "%~1"=="" (
  set SRC=permadeath.cpp
  set OUT=permadeath.dll
) else (
  set SRC=%~1.cpp
  set OUT=%~1.dll
)
echo Building %SRC% -^> %OUT%
cl /nologo /LD /EHsc /O2 /std:c++17 /DNDEBUG %SRC% /Fe:%OUT% /link /OUT:%OUT%
if errorlevel 1 (
  echo BUILD FAILED
  exit /b 1
)
echo BUILD OK
dir /b %OUT%
