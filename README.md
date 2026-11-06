# Daytona USA ReXGlue Recompilation

This project is a static recompilation of Daytona USA (Xbox 360 / XBLA, 2011) using ReXGlue. No copyrighted game files are included.

This is a fork of [Subarasheese/daytona-xbla-recomp](https://github.com/Subarasheese/daytona-xbla-recomp) with the following changes:

* Uses a direct Vulkan renderer with higher efficiency than ReXGlue's included renderer
  * The game runs at 60 FPS on a Retroid Pocket 5 / Snapdragon 865
* Uses upstream ReXGlue 0.10.0 instead of a forked ReXGlue
* Supports Linux ARM64 and macOS ARM64 targets
* Tested on x86_64 Linux, aarch64 Linux and aarch64 macOS
  * On macOS, the game works in windowed mode. The MoltenVK version included with ReXGlue 0.10.0 has [an issue that prevents it from starting fullscreen](https://github.com/KhronosGroup/MoltenVK/pull/2722).
* This fork is untested on Windows, but I expect it to continue to work there as well

All fork additions were made chiefly through GPT Sol 6.1 in Pi, with me supervising direction and orchestrating verification loops.

## Quick start

1. Install the [prerequisites](#prerequisites).
2. Clone with submodules: `git clone --recurse-submodules <this repo>`
3. Place your game package in `game/` and extract it using the commands below.
4. Prepare the [native shaders](#native-shaders) in `build/shaders/`. This needs a local shader dump from your own copy of the game. Game extraction and codegen do not generate these files.
5. Configure, run codegen, build and launch from the repository root.

| Platform | Preset |
|----------|--------|
| Linux x86-64 | `linux-amd64` |
| Linux ARM64 | `linux-arm64` |
| macOS ARM64 | `mac-arm64` |
| Windows x86-64 | `win-amd64` |

**Linux and macOS**

Set `daytona_preset` to the appropriate value from the table:

```sh
daytona_preset=linux-amd64
python3 scripts/extract_game.py
cmake --preset "$daytona_preset" -S project -DDAYTONA_NATIVE_SHADER_DIR="$PWD/build/shaders"
cmake --build "project/out/build/$daytona_preset" --config Release --target daytona_codegen
cmake --build "project/out/build/$daytona_preset" --config Release
"project/out/build/$daytona_preset/Release/daytona" --game_data_root="$PWD/extracted"
```

On macOS, run in windowed mode and add `--mnk_mode=true` for keyboard controls.

**Windows PowerShell**

```powershell
python scripts/extract_game.py
cmake --preset win-amd64 -S project "-DDAYTONA_NATIVE_SHADER_DIR=$PWD/build/shaders"
cmake --build project/out/build/win-amd64 --config Release --target daytona_codegen
cmake --build project/out/build/win-amd64 --config Release
.\project\out\build\win-amd64\Release\daytona.exe "--game_data_root=$PWD/extracted"
```

## Prerequisites

### All platforms

- **CMake 3.25+**
- **Ninja** build system
- **Python 3.10+**. On Windows, the examples use `python`; on Linux and macOS, they use `python3`.

### Windows

- [**Visual Studio 2022 Community**](https://visualstudio.microsoft.com/vs/community/) with the **Desktop development with C++** workload, including:
  - **C++ Clang Compiler for Windows** (20.x or newer)
  - **MSBuild support for LLVM (clang-cl) toolset**

### Linux

- **Clang 20+**
- GTK3 development headers (`libgtk-3-dev` on Debian/Ubuntu, `gtk3` on Arch)
- X11/XCB interop library (`libx11-xcb-dev` on Debian/Ubuntu, `libx11` on Arch, `libX11-devel` on Fedora)
- XWayland or a native X11 session (the SDK renders via XCB; on Wayland, XWayland must be running)

### macOS

- Xcode 16+ or its command-line tools.
- CMake 3.25+, Ninja, and Python 3: `brew install cmake ninja python`

## Included

```text
config/                         ReXGlue manifest/config files
ppc/                            PPC metadata headers
project/                        host project sources and CMake files
scripts/extract_game.py         local STFS package extractor
thirdparty/rexglue-sdk          required ReXGlue SDK submodule
```

`project/src/daytona_symbols.h` contains the game's symbol map.

## Required local game files

To regenerate codegen or run the project, provide your own legally obtained Daytona USA XBLA package in:

```text
game/
```

The local package currently used with this tree is:

```text
game/{game_file_with_id}
type: Microsoft Xbox 360 LIVE/STFS package, Arcade Title
title/media: XA-2845, media ID 3CA562D4
size: 240058368 bytes
sha256: db2381451a15a4e537154712213a0309e9ddc33d20bdc03421d80c9939893cef
```

Extract it from the repository root with:

```sh
python3 scripts/extract_game.py
```

The script extracts the STFS package into:

```text
extracted/
```

and copies the executable required by ReXGlue to:

```text
assets/default.xex
```

`config/daytona_manifest.toml` references that executable as:

```text
../assets/default.xex
```

`game/`, `extracted/`, and `assets/` are ignored by git because they contain copyrighted game files.
A successful extraction currently produces 162 files; the root `default.xex` should be detected by `file` as:

```text
Microsoft Xbox 360 executable (XA-2845, media ID: 3CA562D4), all regions
```

## Required ReXGlue SDK

This recompilation pins unmodified upstream ReXGlue 0.10.0 at commit `f5337cdc947ff6d4c4196737e2c807a48f2a1fc2`. It is included as a git submodule at:

```text
thirdparty/rexglue-sdk
```

After cloning this repository, initialize it with:

```sh
git submodule update --init --recursive
```

## Native shaders

The renderer embeds translated SPIR-V shaders at build time. `DAYTONA_NATIVE_SHADER_DIR` must point to a directory containing the game's translated `.vs.spv` and `.ps.spv` files, plus `present.vs.spv` and `present.ps.spv`. It does not generate shaders itself.

To regenerate the shaders, you need a local dump of the game's original shader microcode. For each shader, the translator expects `.bin`, `.txt` disassembly and `.json` reflection files with matching names, such as `6433F5B8BCEDF080.vs.bin`, `6433F5B8BCEDF080.vs.txt` and `6433F5B8BCEDF080.vs.json`. The dump must cover the menus and all three tracks. These game-derived files are not included in the repository, and `scripts/extract_game.py` does not produce them.

The script builds a small C++ frontend to the pinned XenosRecomp translator using CMake, Ninja and Clang. This translates the original shader microcode into HLSL without linking the DXC library. The script then runs a native DXC executable to compile the HLSL into SPIR-V.

On Linux x86-64, Windows x86-64 and macOS ARM64, the script selects the DXC binary bundled with XenosRecomp and sets its runtime library path. Linux ARM64 needs an installed native DXC with SPIR-V support. On Ubuntu 26.10, install it with `sudo apt install dxc`. The script finds it on `PATH`; you can also pass `--dxc /path/to/dxc` to select a different native compiler on any platform.

From the repository root, with the shader dump saved in `build/shader-dump/`:

```sh
git clone --recurse-submodules https://github.com/hedge-dev/XenosRecomp.git build/XenosRecomp
git -C build/XenosRecomp checkout 990d03b28a27b50277ee5d8d942e1c5f873869d1
git -C build/XenosRecomp submodule update --init --recursive
python3 tools/compile_native_shaders.py build/shader-dump build/shaders --xenosrecomp build/XenosRecomp
```

On Windows, use PowerShell and replace `python3` with `python`. Run from a Visual Studio developer shell with Clang and Ninja on `PATH`. On macOS, the script uses Xcode's `clang++`.

The script requires that exact XenosRecomp revision and compiles both the game shaders and the present shaders. It builds its translator automatically in the output directory and does not modify XenosRecomp. Keep the dump and generated shaders local, alongside your extracted game files.
