@echo off
rem Builds bin\itr2_dlss5_bridge.dll. Uses nothing from src\ or third_party\.
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist bin mkdir bin
if not exist obj mkdir obj

echo [1/1] itr2_dlss5_bridge.dll
cl /nologo /O2 /MT /EHsc /std:c++17 /W3 /GS /Zi /LD /D_CRT_SECURE_NO_WARNINGS bridge.cpp ^
   /Foobj\ /Fdobj\bridge.pdb /Febin\itr2_dlss5_bridge.dll ^
   /link /DEBUG /OPT:REF /OPT:ICF /PDB:bin\itr2_dlss5_bridge.pdb /PDBALTPATH:itr2_dlss5_bridge.pdb version.lib user32.lib psapi.lib || exit /b 1
del /q bin\*.exp bin\*.lib 2>nul
echo bridge build done.
