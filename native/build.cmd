@echo off
REM Builds d2fmh.dll (32-bit). Needs the VS 2022 Build Tools C++ workload.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul 2>&1
cd /d "%~dp0"
cl /nologo /O2 /W3 /std:c11 /D_CRT_SECURE_NO_WARNINGS /LD d2fmh.c /link /OUT:d2fmh.dll user32.lib kernel32.lib
if errorlevel 1 exit /b 1
del /q d2fmh.obj d2fmh.exp d2fmh.lib 2>nul
echo built d2fmh.dll
