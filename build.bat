@echo off
setlocal

set VCVARS="C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars32.bat"
if not exist %VCVARS% set VCVARS="C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat"
if not exist %VCVARS% (
    echo Could not find vcvars32.bat - edit the path in this script.
    exit /b 1
)

call %VCVARS% >nul 2>&1
if not exist obj mkdir obj

rem safetyhook needs std::expected, hence c++latest.
cl /nologo /c /O2 /EHsc /std:c++latest /MT /DNDEBUG /Iinclude ^
   src\dllmain.cpp src\HavokPhysics.cpp src\HighCoreCpu.cpp ^
   src\VSyncRefreshRate.cpp src\UiScaling.cpp ^
   include\safetyhook\safetyhook.cpp /Foobj\
if errorlevel 1 goto fail

cl /nologo /c /O2 /MT /Iinclude include\safetyhook\Zydis.c /Foobj\
if errorlevel 1 goto fail

link /nologo /DLL /OUT:BrethrenPatch.asi obj\*.obj kernel32.lib user32.lib d3d9.lib
if errorlevel 1 goto fail

echo Built BrethrenPatch.asi
exit /b 0

:fail
echo Build failed.
exit /b 1
