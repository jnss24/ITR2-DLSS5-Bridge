@echo off
setlocal
call "%~dp0build_bridge.bat" || exit /b 1
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
cl /nologo /O2 /MT /EHsc /std:c++17 /W3 test_bridge.cpp /Foobj\ /Feobj\test_bridge.exe || exit /b 1
obj\test_bridge.exe %1 || exit /b 1
