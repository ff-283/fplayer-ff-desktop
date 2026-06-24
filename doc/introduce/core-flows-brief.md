# FPlayer Desktop — 核心业务流程

## 1. 分层调用模型

系统采用 5 层单向调用架构：**UI (Widget) → Service → Runtime (工厂) → Backend → 数据输出**。各层间通过 API 纯虚接口通信，上层不持有下层实现引用。

## 2. 摄像头采集

调用链：`CaptureWindow` 选定设备 → `Service::initCamera()` → `RunTime::createCamera()` → `CameraFFmpeg`/`CameraQt6` 实例化。

FFmpeg 路径通过 libavdevice 调用 dshow 接口捕获原始帧，经 avcodec 解码为 YUV420P；Qt6 路径封装 QCamera。解码帧写入 `CameraFrameBus`（线程安全单例总线，以 sourceId 分区），下游消费者取帧执行 OpenGL 渲染或送入 `StreamFFmpeg` 推流。

## 3. 文件播放

调用链：`CaptureWindow` 选取媒体文件 → `Service::initPlayer()`/`openMediaFile()` → `RunTime::createPlayer()` → `PlayerFFmpeg`/`PlayerQt6`。

FFmpeg 路径调用 avformat 解封装、avcodec 软件解码输出 YUV420P，由 `FGLWidget` 承载 GLSL 着色器渲染，内嵌 BT.601/BT.709 色彩矩阵和 limited/full range 校正。Qt6 路径由 QMediaPlayer 驱动，通过 QVideoSink 帧回调接入同一帧总线。

## 4. 屏幕捕获

调用链：`CaptureWindow` 选择显示器并配置帧率 → `Service::initScreenCapture()`/`selectScreen()` → `RunTime::createScreenCapture()` → `ScreenCaptureDxgi`/`ScreenCaptureFFmpeg`/`ScreenCaptureQt6`。

DXGI 路径基于 Desktop Duplication API 实现，采用后台 QThread 驱动，捕获后经 swscale 完成 BGRA→YUV420P 转换。gdigrab 路径作为 FFmpeg 备选。帧数据写入 `ScreenFrameBus`，支持 `snapshotIfNew()` 差分读取，减少冗余拷贝。

## 5. 公共输出

三路采集共用 `StreamFFmpeg`（支持 remux/transcode/compose 三种 push route，由 spec 字符串驱动解析）实现 RTMP/RTSP/SRT 推流。组合模式下多源帧经 MDI 画布合成后统一编码输出。`DesignTokens` 控制 UI 主题，`SystemSettingsRepository` 以 YAML 持久化配置。
