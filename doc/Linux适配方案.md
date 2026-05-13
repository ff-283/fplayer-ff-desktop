# Linux 适配方案列表

本文档列出 `fplayer-ff-desktop` 从 Windows 迁移到 Linux 需要完成的所有适配项，按优先级与依赖关系排序。

---

## 概述

项目已完成部分 Linux 基础适配（CMake 构建、CPack DEB/TGZ 打包、FFmpeg v4l2/x11grab 采集、PulseAudio/ALSA 音频探测），但仍有若干关键功能仅在 Windows 下可用或存在 TODO 标记。下文将逐项说明现状、差距和适配方案。

---

## 一、屏幕采集：Wayland 兼容

**现状：** 屏幕采集使用 FFmpeg 的 `x11grab` 输入格式，仅在 X11 下可用。检测到 `WAYLAND_DISPLAY` 环境变量时直接报错退出。

**涉及文件：**
- [screencaptureffmpeg.cpp:92-105](backend/media_ffmpeg/src/screencaptureffmpeg.cpp#L92-L105)
- [streamffmpeg.cpp:3154-3167](backend/stream_ffmpeg/src/streamffmpeg.cpp#L3154-L3167)

**问题：** Wayland 已成为 Ubuntu、Fedora 等主流发行版的默认显示协议。`x11grab` 在纯 Wayland 会话下不可用，仅依赖 XWayland 时也不稳定（窗口标题、坐标换算等）。

**适配方案：**

| 方案 | 描述 | 优点 | 缺点 |
|------|------|------|------|
| **A. PipeWire + xdg-desktop-portal**（推荐） | 通过 PipeWire ScreenCast 接口采集屏幕，使用 `xdg-desktop-portal` 获取用户授权 | 原生 Wayland 支持；安全模型符合现代桌面规范；可同时获取窗口列表 | 需要新增 PipeWire 客户端代码；依赖系统服务运行 |
| **B. FFmpeg kmsgrab** | 使用 FFmpeg 的 `kmsgrab` 输入格式采集 KMS 帧缓冲 | 无需额外依赖；性能极高（零拷贝） | 仅支持单 GPU；需要 DRM master 权限（通常需 root）；与显示服务器解耦 |
| **C. FFmpeg x11grab + XWayland 桥接** | 强制使用 `QT_QPA_PLATFORM=xcb` 运行应用，并要求用户开启 XWayland | 零代码改动 | 体验差；不是真正的 Wayland 支持；未来可能被移除 |

**建议：** 采用方案 A（PipeWire），并保留现有 x11grab 路径作为 X11 回退。PipeWire 采集可参考 `desktopcapture_dxgi` 的设计模式，新建 `backend/desktopcapture_pipewire/` 模块，实现 `IScreenCapture` 接口。

**预估工作量：** 中等（新增一个后端模块 + CMake 集成）

---

## 二、音频环回采集

**现状：** Windows 下通过 WASAPI Loopback 采集系统音频输出（"你听到的声音"）。Linux 下仅有 PulseAudio/ALSA 音频**输入**探测，无系统音频输出采集能力。

**涉及文件：**
- [wasapiloopbackcapture.cpp](backend/stream_ffmpeg/src/platform/windows/wasapiloopbackcapture.cpp) — Windows 实现
- [audioinputprobe_linux.cpp](backend/stream_ffmpeg/src/platform/linux/audioinputprobe_linux.cpp) — 仅输入探测

**问题：** 推流场景需要采集系统音频输出（如录制正在播放的视频声音），当前 Linux 下完全不可用。

**适配方案：**

| 方案 | 描述 |
|------|------|
| **A. PulseAudio monitor** | 通过 PulseAudio 的 monitor source（如 `alsa_output.pci-xxx.monitor`）获取回放音频流 |
| **B. PipeWire** | 通过 PipeWire 采集任意音频输出节点，类似 WASAPI loopback |

**建议：** 先在 `audioinputprobe_linux.cpp` 中扩展候选设备列表，加入 `pulse` 的默认 monitor source（`@DEFAULT_MONITOR@`）和常见的 ALSA loopback 设备（如 `hw:Loopback,0,0`）。PipeWire 支持可后续单独迭代。

**预估工作量：** 小（扩展现有探测逻辑即可）

---

## 三、Qt6 媒体播放器后端配置

**现状：** `playerqt6.cpp` 中 Linux 路径有两个 TODO 注释，但实际调用 `m_mediaPlayer->play()` 正常。Windows 下 Qt6 使用 WMF（Windows Media Foundation）作为后端，Linux 下默认使用 GStreamer。

**涉及文件：**
- [playerqt6.cpp:29-31](backend/media_qt6/src/playerqt6.cpp#L29-L31) — 构造时管道配置
- [playerqt6.cpp:74-77](backend/media_qt6/src/playerqt6.cpp#L74-L77) — play() 前检查

**问题：**
1. Linux 上 Qt6 Multimedia 默认依赖 GStreamer，需确保目标系统安装了必要的 GStreamer 插件（`gstreamer1.0-plugins-good/bad/ugly`、`gstreamer1.0-libav`）
2. 部分格式（如 H.264/H.265 硬解）需配合 `vaapi` 或 `vdpau` 插件
3. GStreamer 管道可能需要显式配置才能正确处理某些媒体格式

**适配方案：**

1. 在 DEB 包依赖中声明所需的 GStreamer 运行时包
2. 构造 `PlayerQt6` 时，Linux 下通过环境变量或 `QMediaPlayer` API 设置 GStreamer 后端参数：
   ```cpp
   // 示例：设置 GStreamer 的 vaapi 硬解
   qputenv("GST_PLUGIN_FEATURE_RANK", "vaapidecode:MAX,vah264dec:MAX");
   ```
3. 移除 TODO 注释，或在 `play()` 前增加 GStreamer 管道就绪检测

**预估工作量：** 小（主要是依赖声明和配置调优）

---

## 四、DXGI 桌面采集后端的 Linux 替代

**现状：** `backend/desktopcapture_dxgi/` 使用 D3D11/DXGI Desktop Duplication API 实现高性能屏幕采集（含光标叠加、HDR 检测、BGRA→YUV 转换）。该模块仅在 `WIN32` 下编译。

**涉及文件：**
- [backend/CMakeLists.txt:18](backend/CMakeLists.txt#L18) — `FPLAYER_BUILD_MEDIA_DXGI AND WIN32`
- [capturewindow.cpp:1264](widget/src/capturewindow.cpp#L1264) — `isHdrEnabledForScreenIndex()`
- [capturewindow.cpp:1491](widget/src/capturewindow.cpp#L1491) — DXGI 不可用时的 UI 回退
- [capturewindow.cpp:3753](widget/src/capturewindow.cpp#L3753) — 后端选择 UI
- [capturewindow.cpp:5281](widget/src/capturewindow.cpp#L5281) — HDR 警告
- [capturewindow.cpp:6451](widget/src/capturewindow.cpp#L6451) — 屏幕选择时的 HDR 提示

**问题：**
1. DXGI 后端在 Linux 上不可用，UI 中仍显示 "DXGI" 选项的文字（`capturewindow.cpp:1257`）
2. `isHdrEnabledForScreenIndex()` 仅在 `_WIN32` 下实现
3. 多处 UI 逻辑有 DXGI 存在的假设

**适配方案：**

1. 用 `#ifdef _WIN32` 包裹 DXGI 相关的 UI 代码路径，Linux 下隐藏 "DXGI" 选项
2. 若采用 PipeWire 方案（见第一项），可新建 `FPLAYER_BUILD_MEDIA_PIPEWIRE` 选项，在 UI 中注册为 "PipeWire" 后端，与 DXGI 并列显示
3. HDR 检测在 Linux 下返回 `false`（当前 Linux 桌面 HDR 支持尚不成熟）
4. 确保 `capturewindow.cpp` 中 `screenBackendName()` 函数在非 Windows 平台不返回 "DXGI"

**预估工作量：** 中等（若新增 PipeWire 后端则大）

---

## 五、打包脚本

**现状：** 仅有 Windows PowerShell 打包脚本 `scripts/package-windows.ps1`，包含图标生成（ImageMagick）、Qt 部署（windeployqt）、NSIS/WiX CPack 生成等步骤。

**涉及文件：**
- [scripts/package-windows.ps1](scripts/package-windows.ps1)
- [CMakeLists.txt:44-53](CMakeLists.txt#L44-L53) — Linux CPack 配置

**适配方案：**

编写 `scripts/package-linux.sh`，实现以下功能：

1. **构建配置** — 调用 `cmake` 配置构建（指定 Qt6 路径、构建类型）
2. **构建** — `cmake --build`
3. **安装暂存** — `cmake --install` 到临时目录
4. **运行时依赖自动部署** — 自动复制 FFmpeg `.so` 到安装目录（已由 `cmake/3rd.cmake` 实现）
5. **LinuxDeployQt** — 使用 `linuxdeployqt` 或 Qt 官方的 `qt6-deploy` 工具部署 Qt 运行时依赖
6. **桌面集成** — 生成/安装 `.desktop` 文件、图标文件（`/usr/share/icons/`、`/usr/share/applications/`）
7. **CPack 打包** — 调用 `cpack` 生成 `.deb` 和 `.tar.gz`
8. **AppImage 支持**（可选）— 使用 `linuxdeployqt` 生成 AppImage

命令行参数参考 `package-windows.ps1` 设计：
```bash
./scripts/package-linux.sh \
    -c Release \
    --qt6-dir /opt/Qt/6.10.2/gcc_64/lib/cmake/Qt6 \
    --cmake-prefix-path /opt/Qt/6.10.2/gcc_64 \
    --build-dir disk/package-build \
    --install-dir disk/package-install \
    --output-dir disk/linux \
    --version 0.1.0
```

**预估工作量：** 小（Bash 脚本，参考已有 PowerShell 逻辑）

---

## 六、应用程序入口与桌面集成

**现状：** `app/main.cpp` 使用 `WIN32` 子系统编译（`add_executable(${PROJECT_NAME} WIN32)`），Linux 下无对应处理。

**涉及文件：**
- [app/main.cpp](app/main.cpp)
- [app/CMakeLists.txt](app/CMakeLists.txt)
- [app/app_icon.rc](app/app_icon.rc) — Windows 资源文件

**适配方案：**

1. **CMake 调整：** `app/CMakeLists.txt` 中条件判断 `WIN32`，只在 Windows 下加 `WIN32` 子系统标志
2. **桌面文件：** 新增 `app/res/fplayer-desktop.desktop` 文件：
   ```ini
   [Desktop Entry]
   Type=Application
   Name=FPlayer Desktop
   Comment=局域网多媒体流媒体系统
   Exec=fplayer-desktop
   Icon=fplayer-desktop
   Categories=AudioVideo;Player;Network;
   Terminal=false
   ```
3. **图标安装：** 在 CMake 中增加 `install()` 规则，将 `.desktop` 和图标文件安装到 Linux 标准路径
4. **MIME 关联（可选）：** 注册支持的视频文件类型，允许从文件管理器直接打开

**预估工作量：** 小

---

## 七、运行时依赖声明

**现状：** CMake CPack 配置中设置了 `CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON`，可自动检测 `.so` 依赖，但未显式声明系统级运行时依赖。

**涉及文件：**
- [CMakeLists.txt:44-53](CMakeLists.txt#L44-L53)

**适配方案：**

在 `CMakeLists.txt` 的 Linux CPack 区块中补充 DEB 包依赖声明：

```cmake
set(CPACK_DEBIAN_PACKAGE_DEPENDS
    "libc6 (>= 2.28), "
    "libstdc++6 (>= 9), "
    "libgcc-s1 (>= 9), "
    "qt6-base (>= 6.10), "
    "qt6-multimedia (>= 6.10), "
    "gstreamer1.0-plugins-base, "
    "gstreamer1.0-plugins-good, "
    "gstreamer1.0-plugins-bad, "
    "gstreamer1.0-plugins-ugly, "
    "gstreamer1.0-libav, "
    "libpulse0, "
    "libasound2, "
    "libx11-6, "
    "libgl1"
)
```

**预估工作量：** 小（声明式配置）

---

## 八、CI/CD 增强

**现状：** GitHub Actions 在 `ubuntu-latest` 上构建并运行 `ctest`，但不生成安装包产物。

**涉及文件：**
- [.github/workflows/cmake-multi-platform.yml](.github/workflows/cmake-multi-platform.yml)

**适配方案：**

1. 在 Linux CI Job 中增加打包步骤：安装所需 Qt6/GStreamer 依赖 → `cmake --install` → `cpack -G DEB -G TGZ`
2. 使用 `actions/upload-artifact` 上传 `.deb` 和 `.tar.gz` 产物
3. 可选：增加 `ubuntu-24.04`、`fedora-latest` 等多发行版矩阵

**预估工作量：** 小

---

## 九、OpenGL YUV 渲染验证

**现状：** `FGLWidget` 使用 OpenGL 着色器渲染 YUV 帧。Windows 下存在 `canUseUnpackRowLength()` 函数返回 `false` 的已知问题（GL 驱动 bug），Linux 下通过运行时探测判断。

**涉及文件：**
- [common/src/fglwidget.cpp](common/src/fglwidget.cpp)

**问题：** Linux 下的 OpenGL 实现（Mesa、NVIDIA 专有驱动等）行为各异，需在多种驱动环境下验证：
- YUV→RGB 着色器编译和渲染正确性
- `GL_UNPACK_ROW_LENGTH` 支持情况
- 多 GPU（双显卡笔记本）下的上下文切换

**适配方案：** 在主流 Linux 发行版（Ubuntu 22.04/24.04，Fedora 40）和常见 GPU 驱动（Intel Mesa、NVIDIA 550+、AMD Mesa）组合下进行渲染验证。代码层面当前已具备跨平台条件，无需大改。

**预估工作量：** 小（主要是测试验证）

---

## 十、摄像头枚举增强

**现状：** Linux 下通过 FFmpeg 的 `avdevice_list_input_sources("v4l2", ...)` 枚举摄像头设备。Windows 下额外使用 DirectShow COM API 进行枚举。

**涉及文件：**
- [camerainfofetcher.cpp:321](backend/media_ffmpeg/src/camerainfofetcher.cpp#L321)

**问题：** `v4l2` 枚举方式依赖 FFmpeg 的设备探测能力，部分设备可能无法被正确识别（如某些虚拟摄像头、MIPI 摄像头）。

**适配方案：** 当前 v4l2 枚举路径已实现且可用。如有问题，可扩展为直接遍历 `/dev/video*` 节点并读取 V4L2 capability 获取设备名称。一般情况下不需要改动。

**预估工作量：** 无（已实现，按需修复）

---

## 十一、日志与调试支持

**现状：** Windows 下通过 `SetConsoleOutputCP(CP_UTF8)` 设置控制台 UTF-8 编码，Linux 终端默认 UTF-8 无需处理。

**涉及文件：**
- [app/main.cpp](app/main.cpp#L1)

**适配方案：** 无需改动。`#ifdef _WIN32` 已正确隔离 Windows 特定代码。

**预估工作量：** 无

---

## 十二、文档补充

**现状：** 项目文档全部以 Windows 为中心（Windows 打包说明、发包规范、Windows 音频后端开发说明等），缺少 Linux 构建、运行、打包指导。

**适配方案：**

1. 补充 `doc/Linux-构建说明.md` — 描述在 Ubuntu/Debian/Fedora 上从源码构建的完整流程
2. 补充 `doc/Linux-打包说明.md` — 描述 `package-linux.sh` 脚本使用方法（参考 `Windows-打包说明.md` 格式）
3. 更新 `doc/README.md` 导航，加入 Linux 相关文档链接
4. 在 `doc/problems/` 下新增 Linux 常见问题排障笔记

**预估工作量：** 小

---

## 优先级排序总结

| 优先级 | 项目 | 理由 |
|--------|------|------|
| **P0** | 五、打包脚本 | 无可用的 Linux 打包入口，无法产出安装包 |
| **P0** | 六、桌面集成 | 无 `.desktop` 文件，Linux 用户无法从启动器打开 |
| **P0** | 七、运行时依赖声明 | DEB 包缺少依赖声明，安装后可能无法运行 |
| **P1** | 一、Wayland 屏幕采集 | Wayland 已成主流，x11grab 无法覆盖越来越多用户 |
| **P1** | 四、DXGI 替代 / UI 适配 | Linux 下 UI 仍展示 DXGI 选项，需清理 |
| **P1** | 三、GStreamer 后端配置 | Qt6 播放器可能因缺少 GStreamer 插件而无法播放 |
| **P2** | 二、音频环回采集 | 推流场景需要，但可先通过文档说明替代方案（PulseAudio monitor） |
| **P2** | 八、CI/CD 增强 | 已有 Linux 构建，打包产物上传是锦上添花 |
| **P3** | 九、OpenGL 验证 | 代码已支持，主要是多环境测试 |
| **P3** | 十二、文档补充 | 随其他适配项逐步补充 |
