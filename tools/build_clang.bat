@echo off
rem Build pandora with clang-cl and Ninja into build\clang (Release). The recompiled guest code is
rem much faster with clang than with MSVC. Run tools\recompile.sh first.
setlocal
set "VS=C:\Program Files\Microsoft Visual Studio\2022\Community"
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PATH=%VS%\VC\Tools\Llvm\x64\bin;%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
cd /d "%~dp0.."
if not exist build\clang\build.ninja (
    cmake -S . -B build\clang -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl || exit /b 1
)
cmake --build build\clang --target pandora || exit /b 1
