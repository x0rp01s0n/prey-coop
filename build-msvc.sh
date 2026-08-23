#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
IMAGE="${COOP_BUILD_IMAGE:-prey-msvc-buildtools}"
BUILD_JOBS="${BUILD_JOBS:-1}"
COMMON_PATH="${CHAIRLOADER_COMMON_PATH:-}"
DETOURS_TAG="${DETOURS_TAG:-v4.0.1}"

if [[ -z "$COMMON_PATH" ]]; then
    printf 'CHAIRLOADER_COMMON_PATH must point to Chairloader/Common.\n' >&2
    exit 2
fi

COMMON_PATH="$(realpath "$COMMON_PATH")"
if [[ ! -d "$COMMON_PATH" ]] || ! find "$COMMON_PATH" -type f -name '*.cpp' -print -quit | grep -q .; then
    printf 'No Chairloader Common sources found at %s.\n' "$COMMON_PATH" >&2
    exit 2
fi

command -v podman >/dev/null ||
    { printf 'podman is required.\n' >&2; exit 2; }
command -v git >/dev/null ||
    { printf 'git is required to fetch Microsoft Detours.\n' >&2; exit 2; }

if [[ "${COOP_REBUILD_IMAGE:-0}" == "1" ]] ||
   ! podman image exists "$IMAGE"; then
    podman build -t "$IMAGE" -f "$ROOT/BuildTools/Containerfile" \
        "$ROOT/BuildTools"
fi

mkdir -p "$ROOT/_deps" "$ROOT/_xwin-cache"
if [[ ! -f "$ROOT/_deps/detours-install/lib/detours.lib" ]] &&
   [[ ! -f "$ROOT/_deps/Detours-src/src/detours.cpp" ]]; then
    git clone --depth 1 --branch "$DETOURS_TAG" \
        https://github.com/microsoft/Detours.git \
        "$ROOT/_deps/Detours-src"
fi

podman run --rm --security-opt label=disable \
    -v "$ROOT:/work" \
    -v "$COMMON_PATH:/chairloader-common:ro" \
    -v "$ROOT/_xwin-cache:/root/.cache/cargo-xwin" \
    -w /work \
    -e "BUILD_JOBS=$BUILD_JOBS" \
    "$IMAGE" \
    bash -lc '
set -euo pipefail
ulimit -s unlimited

target=x86_64-pc-windows-msvc
toolchain=/root/.cache/cargo-xwin/cmake/clang-cl/x86_64-pc-windows-msvc-toolchain.cmake
if [[ ! -f "$toolchain" ]] ||
   [[ ! -f /root/.cache/cargo-xwin/xwin/DONE ]]; then
    /usr/local/cargo/bin/cargo-xwin env --target=$target >/dev/null
fi

if [[ ! -f _deps/detours-install/lib/detours.lib ]]; then
    sdk=/root/.cache/cargo-xwin/xwin
    src=_deps/Detours-src/src
    out=_deps/detours-install
    obj="$out/obj"
    rm -rf "$out"
    mkdir -p "$obj" "$out/include/detours" "$out/lib"
    cp "$src/detours.h" "$src/detver.h" "$out/include/detours/"

    common=(
        --target=$target
        -Wno-unused-command-line-argument
        -fuse-ld=lld-link
        /nologo /W4 /Zi /MD /Gy /O2
        /DDETOUR_DEBUG=0 /DWIN32_LEAN_AND_MEAN
        /D_WIN32_WINNT=0x501 /DNDEBUG /D_LIB /c
        /imsvc "$sdk/crt/include"
        /imsvc "$sdk/sdk/include/ucrt"
        /imsvc "$sdk/sdk/include/um"
        /imsvc "$sdk/sdk/include/shared"
        /imsvc "$sdk/sdk/include/winrt"
    )
    sources=(
        detours.cpp modules.cpp disasm.cpp image.cpp creatwth.cpp
        disolx86.cpp disolx64.cpp disolia64.cpp disolarm.cpp disolarm64.cpp
    )
    for source in "${sources[@]}"; do
        clang-cl "${common[@]}" "$src/$source" \
            "/Fo$obj/${source%.cpp}.obj"
    done
    llvm-lib /nologo "/out:$out/lib/detours.lib" "$obj"/*.obj
fi

cmake -S CoopPrototype -B _build/coop-xwin -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$toolchain" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBoost_DIR=/work/BuildTools/cmake \
    -Dfmt_DIR=/work/BuildTools/cmake \
    -DCMAKE_PREFIX_PATH=/work/_deps/detours-install \
    -DCHAIRLOADER_COMMON_PATH=/chairloader-common \
    -DMOD_DLL_PATH=/work/CoopPrototype \
    -DCMAKE_SHARED_LINKER_FLAGS=/defaultlib:advapi32.lib

cmake --build _build/coop-xwin --target CoopPrototype \
    --config Release -j "$BUILD_JOBS"
'
