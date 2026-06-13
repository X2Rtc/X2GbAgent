# X2GbAgent

<p align="center">
  <img src="images/welcome.png" alt="X2GbAgent 产品预览" width="100%">
</p>

<p align="center">
  <a href="README_EN.md">English</a>
</p>

<p align="center">
  <img alt="C" src="https://img.shields.io/badge/C-11-00599C?logo=c&logoColor=white">
  <img alt="CMake" src="https://img.shields.io/badge/CMake-3.16%2B-064F8C?logo=cmake&logoColor=white">
  <img alt="GB/T 28181" src="https://img.shields.io/badge/GB%2FT-28181-1f6feb">
  <img alt="License" src="https://img.shields.io/badge/License-MIT-green.svg">
</p>

X2GbAgent 是一个面向边缘设备和嵌入式场景的 GB/T 28181 设备接入 Agent。它把 SIP 注册、媒体采集、编码推流、Web 控制台、配置存储和运行观测整合到一个轻量级 C/C++ 工程中，适合做国标视频网关、设备模拟器、边缘接入节点和硬件 SDK 适配层。

## 核心能力

- **GB28181 接入**：集成 X2/cGb28181 SDK，可对接真实 SIP 平台，支持多平台与多通道配置。
- **多输入源**：支持摄像头/麦克风采集、媒体文件模拟输入、桌面屏幕输入，以及 DV500 硬件媒体后端。
- **媒体链路**：基于 FFmpeg 的采集、解码、编码和 JPEG 预览能力；嵌入式目标可切换到硬件 VI/VPSS/VENC。
- **Web 控制台**：内置 Mongoose HTTP 服务，提供登录、平台管理、音视频配置、设备选择、日志、系统设置和 WebSocket 状态推送。
- **本地持久化**：内置 SQLite，用于保存平台、通道、设备源、认证与日志配置。
- **可测试性**：包含原生媒体 smoke tests、HTTP/API 韧性测试、WebSocket 状态测试、输入源切换和重启恢复测试。

## 目录结构

```text
.
├── CMakeLists.txt
├── include/                 # 公共媒体接口
├── src/                     # Agent、Web API、媒体链路和平台代码
├── web_root/                # 内置 Web 控制台
├── tests/                   # Smoke、可靠性和 demo 测试
├── third_party/             # 内嵌第三方源码
├── thrid_lib/               # FFmpeg 和 cGb28181 SDK 包
├── data/                    # 安装 SQL 和示例媒体
└── images/                  # README 与宣传素材
```

## 快速开始

> 所有 CMake 构建产物必须放在 `build-cmake/<platform>-<arch>` 下。不要在源码目录、`build/`、`out/`、`x64/`、`Debug/`、`Release/` 或 `cmake-build-debug/` 中生成构建文件。

### Windows x64 / Visual Studio

```bat
cmake -S . -B build-cmake/win-x64 -G "Visual Studio 17 2022" -A x64
cmake --build build-cmake/win-x64 --config Release
```

运行：

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

## 运行与登录

默认监听地址为 `http://0.0.0.0:8000`，启动后访问：

```text
http://127.0.0.1:8000
```

默认账号：

```text
Username: admin
Password: anyrtc
```

命令行参数：

```text
gb28181-agent [listen_url] [db_path] [web_root] [log_dir] [log_max_bytes] [log_rotate_count]
```

日志也可通过环境变量配置：

```text
GB_AGENT_LOG_DIR
GB_AGENT_LOG_MAX_BYTES
GB_AGENT_LOG_ROTATE_COUNT
```

## 常用 CMake 选项

| 选项 | 默认值 | 说明 |
| --- | --- | --- |
| `GB_AGENT_BUILD_AGENT` | `ON` | 构建 `gb28181-agent` 可执行程序。 |
| `GB_AGENT_USE_X2_GBSDK` | `ON` | 启用 X2/cGb28181 SDK，对接真实 SIP 客户端。 |
| `GBMEDIA_USE_FFMPEG` | `ON` | 启用基于 FFmpeg 的采集、编解码、文件源和预览能力。 |
| `GBMEDIA_BUILD_TESTS` | `OFF` | 构建原生媒体 smoke tests。 |
| `GBMEDIA_PLATFORM_PROFILE` | `generic` | 目标平台配置：`generic` 或 `dv500`。 |
| `GBMEDIA_HARDWARE_BACKEND` | `none` | 硬件媒体后端：`none` 或 `dv500`。 |
| `GB_AGENT_PREVIEW_FPS` | `5` | JPEG 预览帧率。 |

## 测试

构建测试目标：

```bat
cmake -S . -B build-cmake/win-x64 -G "Visual Studio 17 2022" -A x64 -DGBMEDIA_BUILD_TESTS=ON
cmake --build build-cmake/win-x64 --config Release
```

运行可靠性测试套件：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests\run_reliability_suite.ps1 -Config Release
```

## 许可证

X2GbAgent 使用 [MIT License](LICENSE) 发布。第三方组件遵循其各自许可证。
