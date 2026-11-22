@echo off
rem Spike build: measure VM size and speed. ASCII only - no Chinese here,
rem because cmd.exe parses this file with the OEM code page.
rem
rem Produces four numbers that drive the architecture decisions in
rem docs/ARCHITECTURE.md:
rem   bench.exe  - full VM loop, extension call cost
rem   size.exe   - sizeof(Value) and copy cost per payload type
rem   split.exe  - pure dispatch cost vs py::add cost (where the time goes)
rem   slim.exe   - theoretical floor (no interpreter at all)
setlocal
call "D:\Software\VisualStudio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0.."

if not exist build\spike mkdir build\spike

echo [1/5] compile vm.cpp + py_value.cpp + utf8.cpp
cl /nologo /c /std:c++17 /utf-8 /EHsc /W4 /O2 /D_CRT_SECURE_NO_WARNINGS /Iinclude /Ispike\vm ^
  /Fo:build\spike\ spike\vm\vm.cpp src\py_value.cpp src\utf8.cpp
if errorlevel 1 exit /b 1

echo [2/5] build bench.exe
cl /nologo /std:c++17 /utf-8 /EHsc /O2 /D_CRT_SECURE_NO_WARNINGS /Iinclude /Ispike\vm ^
  spike\vm\bench.cpp build\spike\vm.obj build\spike\py_value.obj build\spike\utf8.obj ^
  /Fe:build\spike\bench.exe
if errorlevel 1 exit /b 1

echo [3/5] build size.exe (sizeof Value / copy cost)
cl /nologo /std:c++17 /utf-8 /EHsc /W4 /O2 /Iinclude /Ispike\vm ^
  build\spike\py_value.obj build\spike\utf8.obj spike\vm\size.cpp ^
  /Fe:build\spike\size.exe
if errorlevel 1 exit /b 1

echo [4/5] build split.exe (dispatch vs arithmetic) + slim.exe (floor)
cl /nologo /std:c++17 /utf-8 /EHsc /W4 /O2 /Iinclude /Ispike\vm ^
  build\spike\py_value.obj build\spike\utf8.obj spike\vm\split.cpp ^
  /Fe:build\spike\split.exe
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /utf-8 /EHsc /W4 /O2 spike\vm\slim.cpp /Fe:build\spike\slim.exe
if errorlevel 1 exit /b 1

echo.
echo [5/5] run
echo.
echo --- bench (full VM) ---
build\spike\bench.exe
echo.
echo --- size (value representation) ---
build\spike\size.exe
echo.
echo --- split (where the time goes) ---
build\spike\split.exe
echo.
echo --- slim (floor: no interpreter) ---
build\spike\slim.exe
