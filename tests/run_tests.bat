@echo off
setlocal
cd /d "%~dp0"

where cl.exe >nul 2>nul
if %errorlevel% neq 0 (
    if exist "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" (
        call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
    ) else if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" (
        call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
    )
)

echo Compiling and executing test_governor.cpp...
cl.exe /nologo /EHsc /std:c++17 test_governor.cpp /Fetest_governor.exe
if errorlevel 1 (
    echo [ERROR] Test compilation failed.
    exit /b 1
)

test_governor.exe
set TEST_RESULT=%errorlevel%

del /q test_governor.obj test_governor.exe >nul 2>nul
exit /b %TEST_RESULT%
