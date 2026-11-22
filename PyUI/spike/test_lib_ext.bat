@echo off
rem Verify: does MSVC link.exe accept a static library whose extension is
rem not .lib?
rem
rem Why this matters: .par should use one uniform extension on every platform
rem instead of pretending to be a compiler-specific .lib. But if link.exe
rem refuses it, we have to keep .lib and say so.
rem
rem Two things are checked:
rem   1. link.exe parses the archive by CONTENT (not by extension)
rem   2. calling it through cl with /link still works (cl sniffs file types)
setlocal
call "D:\Software\VisualStudio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0.."

if not exist build\spike\libtest mkdir build\spike\libtest

> build\spike\libtest\probe.cpp echo extern "C" int probe_value() { return 42; }
> build\spike\libtest\main.cpp  echo extern "C" int probe_value();
>> build\spike\libtest\main.cpp echo int main() { return probe_value() == 42 ? 0 : 1; }

echo [1/3] compile probe.obj and archive it as probe.pylib
cl /nologo /c /std:c++17 /utf-8 /EHsc /Fo:build\spike\libtest\probe.obj build\spike\libtest\probe.cpp
if errorlevel 1 exit /b 1
rem NOTE: no /GL on purpose -- LTCG would put MSIL in the archive and the
rem linker would then also need the matching /LTCG flag.
lib /nologo /OUT:build\spike\libtest\probe.pylib build\spike\libtest\probe.obj
if errorlevel 1 exit /b 1

echo.
echo [2/3] link.exe directly with the non-.lib archive
cl /nologo /c /std:c++17 /utf-8 /EHsc /Fo:build\spike\libtest\main.obj build\spike\libtest\main.cpp
if errorlevel 1 exit /b 1
link /nologo /OUT:build\spike\libtest\main.exe ^
  build\spike\libtest\main.obj build\spike\libtest\probe.pylib
if errorlevel 1 (
  echo RESULT: link.exe FAILED with .pylib
  exit /b 1
)

echo.
echo [3/3] run it
build\spike\libtest\main.exe
if errorlevel 1 (
  echo RESULT: exe ran but probe_value was not found or returned wrong value
  exit /b 1
)
echo RESULT: OK -- link.exe accepts a static library with a non-.lib extension
