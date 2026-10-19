#!/bin/sh
# Regenerate the recompiled C sources in build/recomp from orig/SX3P01/main.dol.
# Builds DolRecomp from extern/DolRecomp on first use.
set -e
cd "$(dirname "$0")/.."

# Local fixes to DolRecomp live in patches/dolrecomp; apply any that are not applied yet.
for patch in patches/dolrecomp/*.patch; do
    [ -e "$patch" ] || continue
    if ! git -C extern/DolRecomp apply --reverse --check "../../$patch" 2>/dev/null; then
        git -C extern/DolRecomp apply "../../$patch"
        echo "applied $patch"
    fi
done

DOLRECOMP=build/dolrecomp/Release/dolrecomp.exe
if [ ! -d build/dolrecomp ]; then
    cmake -S extern/DolRecomp -B build/dolrecomp -G "Visual Studio 17 2022" -A x64
fi
cmake --build build/dolrecomp --config Release --target dolrecomp > /dev/null

# Call across chunks directly instead of returning to the dispatcher for every cross-chunk bl
# (halves dispatches). Safe here: hooks are markers inside the code, not dispatch-time checks.
export DOLRECOMP_UNSAFE_DIRECT_CALLS=1

mkdir -p build/SX3P01
python -I tools/export_map.py config/SX3P01/symbols.txt build/SX3P01.map
python -I tools/patch_hooks.py orig/SX3P01/main.dol config/SX3P01/symbols.txt config/SX3P01/hooks.txt     build/SX3P01/main_hooked.dol
rm -rf build/recomp
"$DOLRECOMP" --cpu broadway --map build/SX3P01.map -j8 build/SX3P01/main_hooked.dol SX3P01 build/recomp
