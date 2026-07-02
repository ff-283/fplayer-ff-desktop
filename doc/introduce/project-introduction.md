# FPlayer Desktop — 项目全面分析

## 1. 项目概述

**FPlayer Desktop** 是一个跨平台局域网流媒体播放系统 (C++/Qt)，核心功能覆盖以下场景：

- **摄像头采集**（含虚拟摄像头）
- **本地文件播放**（支持逐帧/变速/调试信息）
- **屏幕共享**（DXGI Desktop Duplication / FFmpeg gdigrab）
- **局域网推拉流**（RTMP/RTSP/HTTP-FLV，可选 service 网关编排）
- **组合模式**（多源画布合成 + YUV 混合编码）
- **图库 + AI 图像分析**（OpenAI 兼容 SSE 流式返回）

支持 **P2P 直连** 和 **service 编排** 两种工作模式，可与 `fplayer-ff-mobile` (Android) 互联。

---

## 2. 技术栈

| 层        | 技术                                                       |
| --------- | ---------------------------------------------------------- |
| 语言      | C++17                                                      |
| UI        | Qt 6.10.2 (Widgets / OpenGL / Network / Designer)          |
| 编解码    | FFmpeg 8.1（主），Qt6::Multimedia（备）                     |
| 屏幕采集  | DXGI Desktop Duplication (Win) / FFmpeg gdigrab            |
| 推流协议  | RTMP / RTSP / SRT / HTTP-FLV                               |
| 低延迟    | WebRTC（可选，默认关闭）                                    |
| 构建      | CMake 3.15+，CPM.cmake 管理三方库                          |
| 日志      | spdlog via Logger (v1.1.4)                                 |
| 配置      | YAML via yaml-tool (v1.0.2)                                |
| 打包      | CPack (NSIS .exe / WiX .msi / DEB .tgz)                    |
| 设计系统  | Apple 风格 Dark/Light 双主题                               |

---

## 3. 目录结构

```
fplayer-ff-desktop/
├── CMakeLists.txt              # 根构建文件，编排所有子模块
├── README.md                   # 项目概述、构建说明、架构图
├── cmake/                      # CMake 辅助脚本
│   ├── 3rd.cmake               # 第三方依赖管理 (Logger, yaml-tool, FFmpeg)
│   ├── utils.cmake             # 宏定义：config_project, add_standard_module, qt_deploy
│   └── CPM.cmake               # CPM 包管理器脚本
├── 3rd/                        # 本地捆绑的第三方库
│   ├── ffmpeg_v8.1/            # FFmpeg 8.1 (头文件、msvc/mingw/gcc 预编译库、DLL)
│   └── SDL_v3.4.2/             # SDL2 (可选，备用渲染)
├── doc/                        # 大量文档 (22+)
│   ├── man/                    # 用户手册
│   ├── problems/               # 问题记录与排查
│   └── img/                    # 文档图片
├── scripts/                    # 构建/打包脚本 (package-windows.ps1)
├── app/                        # 应用层 — 可执行入口
│   ├── CMakeLists.txt
│   ├── main.cpp                # main(): QApplication, CLI 解析, CaptureWindow 实例化
│   ├── app_icon.rc             # Windows 图标资源
│   └── res/icon/               # 应用图标
├── widget/                     # UI 层 — Qt Widgets
│   ├── CMakeLists.txt
│   ├── include/fplayer/widget/ # 公共头文件
│   │   ├── capturewindow.h     # 主窗口 (UI 中枢)
│   │   ├── fvideoview.h        # 视频预览控件 (Qt/OpenGL 双模式)
│   │   ├── imagepoolsidebar.h  # 图库侧栏控件
│   │   ├── imageviewerdialog.h # 全屏图片查看器
│   │   ├── aichatdialog.h      # AI 聊天分析对话框
│   │   └── chatbubblewidget.h  # 聊天气泡自定义样式
│   ├── src/                    # 实现文件 (.cpp)
│   └── uis/capturewindow.ui    # Qt Designer UI 文件
├── service/                    # 业务层 — 业务逻辑外观
│   ├── CMakeLists.txt
│   ├── include/fplayer/service/
│   │   ├── service.h           # Service 类：编排 camera/player/screen/stream
│   │   ├── aiservice.h         # AI 分析服务 (OpenAI 兼容 SSE)
│   │   ├── imagepoolservice.h  # 图库服务 (文件监听、缩略图)
│   │   └── systemsettingsrepository.h  # 配置持久化 (YAML)
│   └── src/                    # 实现
├── runtime/                    # 运行时层 — 工厂模式
│   ├── CMakeLists.txt
│   ├── include/fplayer/runtime/
│   │   └── runtime.h           # RunTime 类：创建 ICamera/IPlayer/IScreenCapture/IStream
│   └── src/runtime.cpp         # 工厂实现 (switch on MediaBackendType)
├── api/                        # 接口层 — 抽象接口
│   ├── CMakeLists.txt
│   ├── include/fplayer/api/
│   │   ├── media/              # 媒体接口
│   │   │   ├── iplayer.h       # IPlayer: openFile/play/pause/stop/seek/volume
│   │   │   ├── icamera.h       # ICamera: select/refresh/pause/resume/getDescriptions
│   │   │   ├── iscreencapture.h # IScreenCapture: refresh/select screen, cursor capture, FPS
│   │   │   ├── ifvideoview.h   # IFVideoView: previewTarget()
│   │   │   ├── mediabackendtype.h # 枚举: Qt6, FFmpeg, Dxgi
│   │   │   └── previewtarget.h # NativeWindow + backend_hint 渲染目标
│   │   └── net/
│   │       └── istream.h       # IStream: startPush/startPull/stop/isRunning/recording
│   └── src/                    # 默认实现 (析构函数等)
├── backend/                    # 后端层 — 具体实现
│   ├── CMakeLists.txt          # 条件子目录，基于 CMake 选项
│   ├── media_ffmpeg/           # FFmpeg 媒体后端：CameraFFmpeg, PlayerFFmpeg, ScreenCaptureFFmpeg
│   ├── media_qt6/              # Qt6 媒体后端：CameraQt6, ScreenCaptureQt6
│   ├── stream_ffmpeg/          # FFmpeg 流媒体后端：StreamFFmpeg (推流/拉流/录制)
│   ├── desktopcapture_dxgi/    # Windows DXGI：ScreenCaptureDxgi (Desktop Duplication)
│   ├── net_qt6/                # Qt6 网络后端：StreamQt6 (空桩，默认 OFF)
│   └── rtc_webrtc/             # WebRTC (桩代码，当前为空)
├── common/                     # 公共层 — 共享工具
│   ├── CMakeLists.txt
│   ├── include/fplayer/common/
│   │   ├── designtokens.h      # 设计系统：ThemeColors, QSS 生成, 间距/圆角/字体 tokens
│   │   ├── screenframebus/     # ScreenFrameBus: YUV 帧发布订阅 (通道式)
│   │   ├── cameraframebus/     # CameraFrameBus: YUV 帧发布订阅
│   │   ├── fglwidget/          # OpenGL YUV 渲染控件
│   │   ├── imagepool/          # ImageMeta 数据结构
│   │   ├── qtloggeradapter/    # Qt -> Logger 桥接
│   │   └── programframebus/    # (桩代码/空)
│   └── src/                    # 实现 (4 个 .cpp 文件)
└── build/                      # 构建输出 (gitignore)
```

---

## 4. 分层架构

```
app/          ── 可执行入口 (main.cpp → QApplication → CaptureWindow)
widget/       ── Qt UI 层 (CaptureWindow, FVideoView, ImagePoolSidebar, AiChatDialog)
service/      ── 业务外观层 (Service — 统一编排)
runtime/      ── 工厂层 (RunTime — 按 MediaBackendType 创建具体实现)
api/          ── 抽象接口层 (ICamera, IPlayer, IScreenCapture, IStream, IFVideoView)
backend/      ── 具体实现层 (media_ffmpeg, media_qt6, stream_ffmpeg, desktopcapture_dxgi, rtc_webrtc)
common/       ── 公共工具层 (DesignTokens, FrameBus, FGLWidget, ImagePool, LoggerAdapter)
```

严格自上而下依赖，仅 Runtime 层知晓所有后端实现（**DIP 原则**）。

---

## 5. 关键组件及职责

### 5.1 主窗口 (`CaptureWindow`)
- **文件**: `widget/src/capturewindow.cpp` (~5000+ 行)
- **角色**: 应用的唯一主窗口，统筹整个 UI
- 管理四种采集模式：
  - **摄像头模式**: 选择摄像头/格式、预览、截图、录制
  - **文件播放模式**: 打开视频文件、进度/速度控制、调试统计
  - **屏幕采集模式**: 屏幕选择、光标采集、FPS 控制
  - **组合模式**: 多源画布，MDI 子窗口，拖拽/缩放/裁剪，Z 排序，YUV 合成
- 管理推拉流对话框与网关 service 集成
- 管理图库侧栏和 AI 聊天对话框
- 管理系统托盘图标
- 通过 `Service` 加载/保存系统配置

### 5.2 业务外观层 (`Service`)
- **文件**: `service/src/service.cpp`
- 封装 `RunTime` 工厂的统一外观
- 为 Widget 层提供统一 API
- 将 UI 操作翻译成后端调用

### 5.3 运行时工厂 (`RunTime`)
- **文件**: `runtime/src/runtime.cpp`
- 基于 `MediaBackendType` 枚举创建后端实例
- 使用 switch 语句实例化正确的实现：
  - `Qt6` → `CameraQt6` / `ScreenCaptureQt6`
  - `FFmpeg` → `CameraFFmpeg` / `PlayerFFmpeg` / `ScreenCaptureFFmpeg` / `StreamFFmpeg`
  - `Dxgi` → `ScreenCaptureDxgi` (仅 Windows)

### 5.4 后端实现

| 模块                    | 说明                                                         |
| ----------------------- | ------------------------------------------------------------ |
| `media_ffmpeg/`         | FFmpeg 视频采集/播放 (CameraFFmpeg, PlayerFFmpeg, ScreenCaptureFFmpeg) |
| `media_qt6/`            | Qt6::Multimedia 替代方案 (CameraQt6, ScreenCaptureQt6)       |
| `stream_ffmpeg/`        | FFmpeg 推拉流 (StreamFFmpeg: RTMP/RTSP 推流, HTTP-FLV 拉流, 侧录) |
| `desktopcapture_dxgi/`  | Windows DXGI Desktop Duplication API (高性能, 光标采集, HDR 检测) |
| `net_qt6/`              | Qt6 网络 StreamQt6 (空桩，默认 OFF)                       |
| `rtc_webrtc/`           | WebRTC (桩代码，当前为空)                                    |

### 5.5 帧总线系统 (`ScreenFrameBus` / `CameraFrameBus`)
- **模式**: 单例发布订阅总线，用于 YUV 帧数据
- 每条总线有命名通道 (sourceId)，线程安全 (QMutex)
- 用于合成：源端发布 YUV 帧；合成器读取快照
- `ScreenFrameBus` 支持目标尺寸提示用于合成输出尺寸

### 5.6 OpenGL 渲染器 (`FGLWidget`)
- 自定义 `QOpenGLWidget`，将 YUV420P 帧渲染到屏幕
- 使用 3 个纹理 (Y, U, V 平面) 配合自定义着色器
- 通过 QMutex 实现线程安全的帧更新
- 作为所有后端的首选视频渲染器

### 5.7 AI 服务 (`AiService`)
- OpenAI 兼容的 Chat Completions API 客户端
- 支持通过 base64 编码图像进行图像分析
- SSE (Server-Sent Events) 流式实时文本响应
- 可配置端点、API key、模型和 UI 颜色

### 5.8 图库 (`ImagePoolService`)
- 使用 `QFileSystemWatcher` 的目录监听图库服务
- 支持带 LRU 缓存的缩略图生成
- 排序模式: 名称/日期/大小 升序/降序
- 与截图功能无缝集成

### 5.9 系统配置仓库 (`SystemSettingsRepository`)
- 基于 YAML 的所有用户偏好持久化
- 存储: 截图/录制目录, 推流偏好, AI 配置, 主题, 颜色, 最近输入/输出
- 线程安全的持久化

### 5.10 设计令牌 (`designtokens.h`)
- 完整的 Dark/Light 双主题设计系统
- 从令牌数据生成 QSS 样式表
- 定义颜色调色板 (primary, canvas, ink, surface tiles, hairline), 圆角, 间距, 字号
- Apple 风格美学适配暗色视频应用

---

## 6. 导航结构

无传统路由（桌面应用非 Web 应用）。导航方式：

- **菜单栏**: 4 种模式 (camera/file/screen/compose) + 推/拉流 + 查看菜单 (图库)
- **底部工具栏按钮**: 播放/暂停, 截图, 录制, 设置, 图库, 全屏
- **模态对话框**: 推流配置, 拉流配置, 系统设置, AI 聊天, 图片查看器
- **系统托盘图标**: 最小化到托盘, 从托盘恢复, 退出
- **图库侧栏**: 独立浮动窗口，缩略图网格, 全屏查看器, 右键 AI 分析

`CaptureWindow` 作为单页应用外壳，根据所选模式切换 UI 状态。

---

## 7. 状态管理

- **无全局状态管理器** — 状态分布在各处：
  - `CaptureWindow` 保存所有 UI 状态作为成员变量 (~60+ 个字段)
  - `Service` 保存后端实例和摄像头索引
  - `RunTime` 持有后端实现的 shared_ptr
  - `SystemSettings` 结构体通过 YAML 仓库加载/保存
  - `ScreenFrameBus` / `CameraFrameBus` 单例提供跨组件帧数据
  - QTimer 周期性 UI 更新 (文件进度, 调试统计, 录制时长)
  - Signal/slot 连接实现事件驱动更新

---

## 8. API/后端交互模式

```
CaptureWindow (Widget)
    |  调用 Service 方法
    v
Service (Facade)
    |  委托给 RunTime 工厂
    v
RunTime (Factory)
    |  创建并返回 shared_ptr<Interface>
    v
后端实现 (media_ffmpeg, media_qt6, stream_ffmpeg, desktopcapture_dxgi)
    |  实现 API 层接口
    v
API 接口 (ICamera, IPlayer, IScreenCapture, IStream, IFVideoView)
```

**预览绑定流程**:
1. `Widget` 调用 `Service::bindCameraPreview(IFVideoView*)`
2. `Service` 调用 `videoView->previewTarget()` 获取 `PreviewTarget` (原生 HWND + backend_hint)
3. `Service` 将 target 传给 `Runtime::bindCameraPreview(target)`
4. `Runtime` 在实际后端上调用 `camera->setPreviewTarget(target)`
5. 后端将帧渲染到原生窗口句柄或使用 backend_hint (Qt6 用 QVideoWidget, FFmpeg 用 OpenGL)

**带 service 网关的推拉流**:
- 推流: Desktop 调用 `requestServiceStreamStart()` (HTTP POST 到 `/api/v1/streams/start`), 获取 RTMP 推流地址, 然后启动 FFmpeg 推流
- 拉流: Desktop 调用 `requestServiceStreamStatus()` (HTTP GET 到 `/api/v1/streams/resolve`), 获取 HTTP-FLV/RTMP 播放地址, 然后启动 FFmpeg 拉流

---

## 9. 关键数据模型

| 模型                  | 位置                                     | 说明                                   |
| --------------------- | ---------------------------------------- | -------------------------------------- |
| `CameraDescription`   | `api/media/icamera.h`                    | 摄像头名称、ID、格式索引、格式列表     |
| `ScreenDescription`   | `api/media/iscreencapture.h`             | 屏幕名称、主屏幕标志、几何信息         |
| `PreviewTarget`       | `api/media/previewtarget.h`              | 平台原生窗口句柄 + 可选后端提示        |
| `MediaBackendType`    | `api/media/mediabackendtype.h`           | 枚举: Qt6, FFmpeg, Dxgi                |
| `ScreenFrame`         | `common/screenframebus/screenframebus.h` | YUV 平面数据, 尺寸, 步幅, 序列号       |
| `CameraFrame`         | `common/cameraframebus/cameraframebus.h` | 同 ScreenFrame (YUV 数据)              |
| `ImageMeta`           | `common/imagepool/imagemetadata.h`       | 文件路径、大小、时间戳、图像尺寸       |
| `SystemSettings`      | `service/systemsettingsrepository.h`     | 所有用户偏好 (40+ 字段)                |
| `AiConfig`            | `service/aiservice.h`                    | AI 端点、key、模型、颜色/字体设置      |
| `Service::PushOptions`| `service/service.h`                      | FPS、分辨率、码率、音频源、编码器      |
| `Service::PushScene`  | `service/service.h`                      | 枚举: Camera, File, Screen             |
| `ThemeColors`         | `common/designtokens.h`                  | 15 个 Dark/Light 双主题颜色令牌        |
| `ComposeSourceItem`   | `widget/capturewindow.h` (private)       | 源类型、service、view、window、裁剪状态 |

---

## 10. 关键架构模式

| 模式                | 位置/用途                                                       |
| ------------------- | --------------------------------------------------------------- |
| **Facade**          | `Service` 向 Widget 暴露统一接口                                 |
| **Factory**         | `RunTime` 根据 `MediaBackendType` 创建后端实例                   |
| **Singleton Bus**   | `ScreenFrameBus` / `CameraFrameBus` — YUV 帧发布订阅            |
| **PIMPL**           | `CameraFFmpeg` / `PlayerFFmpeg` 隐藏 FFmpeg 内部细节            |
| **Strategy**        | 同一接口多种后端实现，运行时可选切换                             |
| **Repository**      | `SystemSettingsRepository` 抽象 YAML 持久化操作                  |
| **Observer**        | Qt signal/slot 贯穿全项目                                       |
| **Bridge**          | `PreviewTarget` 桥接 QWidget 与原生渲染句柄                     |
| **Adapter**         | `qtloggeradapter` 桥接 Qt 日志 → Logger 库                      |
| **Template Method** | `IFVideoView` 基类 + `previewTarget()` 虚方法, `FVideoView` 实现 |

---

## 11. 核心数据流

```
摄像头/屏幕/文件  →  Backend 解码 YUV  →  FrameBus 发布
                                          ↓
                              FGLWidget (OpenGL YUV→RGB)  或  StreamFFmpeg (编码推流)
```

推拉流与 service 网关交互:

```
Desktop → POST /api/v1/streams/start  →  获取 RTMP 推流地址 → FFmpeg 推送
Desktop → GET  /api/v1/streams/resolve → 获取播放地址     → FFmpeg 拉流
```

---

## 12. 入口点与构建配置

### 入口点
- **文件**: `app/main.cpp`
- **流程**:
  1. 设置 Windows 控制台 UTF-8
  2. 安装 Qt → Logger 消息处理器
  3. 设置 OpenGL surface 格式 (双缓冲, VSync)
  4. 创建 `QApplication`
  5. 记录启动环境 (路径、库、平台)
  6. 解析 CLI: `--backend | -b` (0=FFmpeg, 1=Qt6)
  7. 在栈上创建 `CaptureWindow`
  8. 进入 Qt 事件循环

### 构建目标 (库)

| 目标                                        | 类型           |
| ------------------------------------------- | -------------- |
| `FPlayer_Common`                            | 动态库         |
| `FPlayer_Api`                               | 动态库         |
| `FPlayer_Backend_Media_Qt6`                 | 动态库         |
| `FPlayer_Backend_Media_FFmpeg`              | 动态库         |
| `FPlayer_Backend_Desktop_Capture_Dxgi`      | 动态库         |
| `FPlayer_Backend_Stream_FFmpeg`             | 动态库         |
| `FPlayer_Backend_Net_Qt6`                   | 动态库 (OFF by default) |
| `FPlayer_Runtime`                           | 动态库         |
| `FPlayer_Service`                           | 动态库         |
| `FPlayer_Widget`                            | 动态库         |
| `FPlayer_App`                               | 可执行文件     |

### CMake 关键选项

```cmake
-DCMAKE_PREFIX_PATH=D:/SoftWare/Qt/Qt6.10.2/6.10.2/msvc2022_64
-DQt6_DIR=D:/SoftWare/Qt/Qt6.10.2/6.10.2/msvc2022_64/lib/cmake/Qt6
-DFPLAYER_BUILD_MEDIA_FFMPEG:BOOL=ON    # 启用 FFmpeg 媒体后端
-DFPLAYER_BUILD_STREAM_FFMPEG:BOOL=ON   # 启用 FFmpeg 推流后端
-DFPLAYER_BUILD_MEDIA_QT6:BOOL=ON       # 启用 Qt6 媒体后端 (默认 ON)
-DFPLAYER_BUILD_MEDIA_DXGI:BOOL=ON      # 启用 DXGI 屏幕采集 (Windows)
-DFPLAYER_BUILD_NET_QT6:BOOL=OFF        # Qt6 网络后端 (默认 OFF，空桩)
-DFPLAYER_BUILD_RTC_WEBRTC:BOOL=OFF     # WebRTC (默认关闭)
```

### 打包
- Windows: CPack 生成 `.exe` (NSIS) 和 `.msi` (WiX) 安装包
- Linux: CPack 生成 `.deb` 和 `.tgz`
- 脚本: `scripts/package-windows.ps1` 一键 Windows 打包
- 输出: `disk/windows/` 目录

---

## 13. 关键文件索引

| 文件                                                   | 说明                                  |
| ------------------------------------------------------ | ------------------------------------- |
| `app/main.cpp`                                         | 程序入口，CLI 解析，启动 CaptureWindow |
| `widget/src/capturewindow.cpp`                         | **主窗口** (~5000 行)，所有 UI 逻辑中枢 |
| `widget/src/fvideoview.cpp`                            | 视频预览控件 (Qt/OpenGL 双模式)        |
| `service/src/service.cpp`                              | 业务外观，编排所有后端操作             |
| `runtime/src/runtime.cpp`                              | 工厂，switch 创建后端实例              |
| `common/src/fglwidget.cpp`                             | OpenGL YUV 渲染器                     |
| `common/src/screenframebus.cpp`                        | YUV 帧发布订阅总线                     |
| `backend/media_ffmpeg/src/playerffmpeg.cpp`            | FFmpeg 播放器核心                      |
| `backend/stream_ffmpeg/src/streamffmpeg.cpp`           | FFmpeg 推拉流核心                      |
| `common/include/fplayer/common/designtokens.h`         | 设计系统 token 定义与 QSS 生成         |
