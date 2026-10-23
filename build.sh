#!/bin/bash
# Cross-build dinput8.dll on Linux with clang-cl + lld-link (LLVM 16+) against the MSVC CRT and Windows SDK.
# Get the CRT/SDK with xwin (https://github.com/Jake-Shadle/xwin):  xwin --accept-license splat --output ~/xwin
# then:  XWIN=~/xwin ./build.sh      (or set the CRT_INC / SDK_INC / CRT_LIB / SDK_LIB variables yourself)
# On Windows, use build_msvc.cmd from a "x64 Native Tools Command Prompt for VS".
set -e
cd "$(dirname "$0")"
CLANG=${CLANG:-$(command -v clang-cl || command -v clang-cl-18)}
LLD=${LLD:-$(command -v lld-link || command -v lld-link-18)}
XWIN=${XWIN:-$HOME/xwin}
CRT_INC=${CRT_INC:-$XWIN/crt/include}
SDK_INC=${SDK_INC:-$XWIN/sdk/include}          # contains ucrt/ um/ shared/
CRT_LIB=${CRT_LIB:-$XWIN/crt/lib/x86_64}
SDK_LIB_UM=${SDK_LIB_UM:-$XWIN/sdk/lib/um/x86_64}
SDK_LIB_UCRT=${SDK_LIB_UCRT:-$XWIN/sdk/lib/ucrt/x86_64}

IM=vrkit/third_party/imgui; MH=third_party/minhook; OXR=third_party/openxr
mkdir -p obj out
"$CLANG" --target=x86_64-pc-windows-msvc /nologo /O2 /MT /EHsc /std:c++17 -Wno-everything \
  -D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH -D_MSVC_STL_USE_ABORT_AS_DOOM_FUNCTION -DWIN32_LEAN_AND_MEAN -DNOMINMAX \
  -imsvc "$CRT_INC" -imsvc "$SDK_INC/ucrt" -imsvc "$SDK_INC/um" -imsvc "$SDK_INC/shared" \
  -I$OXR/include -I$MH/include -Ivrkit -I$IM /Foobj/ -c \
  src/*.cpp vrkit/*.cpp $IM/imgui.cpp $IM/imgui_draw.cpp $IM/imgui_tables.cpp $IM/imgui_widgets.cpp $IM/imgui_impl_dx11.cpp \
  $MH/src/hook.c $MH/src/buffer.c $MH/src/trampoline.c $MH/src/hde/hde64.c
"$LLD" /nologo /dll /out:out/dinput8.dll /def:src/dinput8.def \
  "/libpath:$CRT_LIB" "/libpath:$SDK_LIB_UM" "/libpath:$SDK_LIB_UCRT" \
  obj/*.obj $OXR/lib/openxr_loader_static.lib \
  d3d11.lib dxgi.lib d3dcompiler.lib kernel32.lib user32.lib gdi32.lib advapi32.lib shell32.lib ole32.lib uuid.lib
echo "built out/dinput8.dll"
