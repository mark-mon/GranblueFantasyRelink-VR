@echo off
rem Build dinput8.dll with Visual Studio 2019/2022. Run from an "x64 Native Tools Command Prompt for VS".
setlocal
cd /d "%~dp0"
set IM=vrkit\third_party\imgui
set MH=third_party\minhook
set OXR=third_party\openxr
if not exist obj mkdir obj
if not exist out mkdir out
cl /nologo /O2 /MT /EHsc /std:c++17 /W0 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
  /I%OXR%\include /I%MH%\include /Ivrkit /I%IM% /Foobj\ /c ^
  src\*.cpp vrkit\*.cpp %IM%\imgui.cpp %IM%\imgui_draw.cpp %IM%\imgui_tables.cpp %IM%\imgui_widgets.cpp %IM%\imgui_impl_dx11.cpp ^
  %MH%\src\hook.c %MH%\src\buffer.c %MH%\src\trampoline.c %MH%\src\hde\hde64.c || exit /b 1
link /nologo /dll /out:out\dinput8.dll /def:src\dinput8.def obj\*.obj %OXR%\lib\openxr_loader_static.lib ^
  d3d11.lib dxgi.lib d3dcompiler.lib kernel32.lib user32.lib gdi32.lib advapi32.lib shell32.lib ole32.lib uuid.lib || exit /b 1
echo built out\dinput8.dll
