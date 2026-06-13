# X2GbAgent

<p align="center">
  <img src="images/welcome.png" alt="X2GbAgent product preview" width="100%">
</p>

<p align="center">
  <a href="README.md">中文</a>
</p>

<p align="center">
  <img alt="C" src="https://img.shields.io/badge/C-11-00599C?logo=c&logoColor=white">
  <img alt="CMake" src="https://img.shields.io/badge/CMake-3.16%2B-064F8C?logo=cmake&logoColor=white">
  <img alt="GB/T 28181" src="https://img.shields.io/badge/GB%2FT-28181-1f6feb">
  <img alt="License" src="https://img.shields.io/badge/License-MIT-green.svg">
</p>

X2GbAgent is a lightweight GB/T 28181 device access agent for edge and embedded deployments. It combines SIP registration, media capture, encoding, streaming, Web-based operations, local configuration storage, and runtime observability in a compact C/C++ codebase.

## Highlights

- **GB28181 connectivity**: integrates the X2/cGb28181 SDK for real SIP platform access, with multi-platform and multi-channel configuration.
- **Flexible input sources**: supports camera/microphone capture, media-file simulation, desktop screen input, and DV500 hardware media adapters.
- **Media pipeline**: FFmpeg-backed capture, decode, encode, file source, and JPEG preview; embedded targets can use hardware VI/VPSS/VENC paths.
- **Web console**: built-in Mongoose HTTP service with authentication, platform management, A/V configuration, device selection, logs, system settings, and WebSocket status updates.
- **Local persistence**: embedded SQLite stores platform, channel, input source, authentication, and log configuration.
- **Operational testing**: includes native media smoke tests, HTTP/API resilience tests, WebSocket status tests, input switching tests, and restart recovery checks.

## Project Layout

```text
.
├── CMakeLists.txt
├── include/                 # Public media interfaces
├── src/                     # Agent, Web API, media pipeline, and platform code
├── web_root/                # Built-in Web console
├── tests/                   # Smoke, reliability, and demo tests
├── third_party/             # Embedded third-party sources
├── thrid_lib/               # FFmpeg and cGb28181 SDK packages
├── data/                    # Install SQL and sample media
└── images/                  # README and marketing assets
```

## Quick Start

All CMake output must stay under `build-cmake/<platform>-<arch>`. Do not generate build files in the source tree, `build/`, `out/`, `x64/`, `Debug/`, `Release/`, or `cmake-build-debug/`.

### Windows x64 / Visual Studio

```bat
cmake -S . -B build-cmake/win-x64 -G "Visual Studio 17 2022" -A x64
cmake --build build-cmake/win-x64 --config Release
```

Run:

```bat
build-cmake\win-x64\bin\Release\gb28181-agent.exe http://0.0.0.0:8000 gb28181-agent.db web_root
```

### Windows x64 / Ninja

```bat
cmake -S . -B build-cmake/win-x64 -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-cmake/win-x64
```

### Linux x64

```bash
cmake -S . -B build-cmake/linux-x64 -DCMAKE_BUILD_TYPE=Release
cmake --build build-cmake/linux-x64 -j
```

### DV500 / Embedded Linux

```bash
cmake -S . -B build-cmake/linux-arm64 \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/linux-arm64.cmake \
  -DGBMEDIA_PLATFORM_PROFILE=dv500 \
  -DGBMEDIA_DV500_SDK_ROOT=/path/to/dv500/sdk \
  -DGBMEDIA_FFMPEG_TARGET_LIB_DIR=/path/to/target/ffmpeg/lib \
  -DCMAKE_BUILD_TYPE=Release

cmake --build build-cmake/linux-arm64 -j
```

## Runtime

The default listen address is `http://0.0.0.0:8000`. After startup, open:

```text
http://127.0.0.1:8000
```

Default account:

```text
Username: admin
Password: anyrtc
```

Command-line arguments:

```text
gb28181-agent [listen_url] [db_path] [web_root] [log_dir] [log_max_bytes] [log_rotate_count]
```

Log rotation can also be configured with environment variables:

```text
GB_AGENT_LOG_DIR
GB_AGENT_LOG_MAX_BYTES
GB_AGENT_LOG_ROTATE_COUNT
```

## CMake Options

| Option | Default | Description |
| --- | --- | --- |
| `GB_AGENT_BUILD_AGENT` | `ON` | Build the `gb28181-agent` executable. |
| `GB_AGENT_USE_X2_GBSDK` | `ON` | Enable X2/cGb28181 SDK integration for real SIP clients. |
| `GBMEDIA_USE_FFMPEG` | `ON` | Enable FFmpeg-backed capture, codec, file source, and preview support. |
| `GBMEDIA_BUILD_TESTS` | `OFF` | Build native media smoke tests. |
| `GBMEDIA_PLATFORM_PROFILE` | `generic` | Target profile: `generic` or `dv500`. |
| `GBMEDIA_HARDWARE_BACKEND` | `none` | Hardware media backend: `none` or `dv500`. |
| `GB_AGENT_PREVIEW_FPS` | `5` | JPEG preview frame rate. |

## Testing

Build test targets:

```bat
cmake -S . -B build-cmake/win-x64 -G "Visual Studio 17 2022" -A x64 -DGBMEDIA_BUILD_TESTS=ON
cmake --build build-cmake/win-x64 --config Release
```

Run the reliability suite:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests\run_reliability_suite.ps1 -Config Release
```

## License

X2GbAgent is released under the [MIT License](LICENSE). Third-party dependencies remain under their respective licenses.
