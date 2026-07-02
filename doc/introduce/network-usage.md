# FPlayer Desktop — 网络与推拉流系统详解

## 1. 概述

FPlayer Desktop 的网络层围绕 **局域网流媒体推拉** 设计，核心场景包括：

- **推流**：将摄像头、屏幕、本地文件或组合画布编码后推送到 RTMP/RTSP/SRT 服务器
- **拉流**：从 RTMP/RTSP/HTTP-FLV/UDP 等源拉取流媒体并本地预览，可选侧录
- **P2P 直连**：两台设备直接通过 RTMP URL 推拉，无需中间服务
- **Service 网关编排**：通过 HTTP API 与外置网关（如 SRS/Monibuca）交互，获取动态推拉流地址
- **AI 聊天**：OpenAI 兼容的 SSE 流式 API 调用

所有核心流媒体网络操作基于 **FFmpeg libavformat** 实现，Qt 网络模块仅用于 HTTP API 调用和 AI 通信。

---

## 2. 架构分层

```
widget/capturewindow.cpp      UI 层 — 推流/拉流对话框, service 网关 HTTP 请求
       │
service/service.cpp           业务层 — streamStartPush/streamStartPull, PushScene 场景路由
       │
runtime/runtime.cpp           工厂层 — createStream(backend) 创建 IStream 实例
       │
api/net/istream.h             接口层 — IStream 抽象（startPush/startPull/stop/录制/预览控制）
       │
backend/stream_ffmpeg/        FFmpeg 推拉流实现（~6500 行核心代码）
  ├── streamffmpeg.cpp         推拉流主逻辑（remux/transcode/encode loops）
  ├── streamffmpeg_helpers.cpp 路由解析、编码器选择、硬件编码检测
  ├── audio_pipeline.cpp       音频 FIFO 混音与重采样管线
  └── platform/                平台音频采集（WASAPI loopback / Linux ALSA）
       │
backend/net_qt6/              Qt6 网络后端（空桩代码，默认 OFF）
       │
backend/rtc_webrtc/           WebRTC 桩（当前为空，默认不构建）
```

---

## 3. IStream 抽象接口

**文件：** [api/include/fplayer/api/net/istream.h](../api/include/fplayer/api/net/istream.h)

```cpp
class IStream {
    virtual bool startPush(const QString& inputUrl, const QString& outputUrl) = 0;
    virtual bool startPull(const QString& inputUrl, const QString& outputUrl) = 0;
    virtual void stop() = 0;
    virtual bool isRunning() const = 0;
    virtual QString lastError() const = 0;
    virtual QString recentLog() const = 0;

    // 拉流预览控制
    virtual void setPreviewPaused(bool paused);
    virtual bool previewPaused() const;
    virtual void setPreviewVolume(float volume);

    // 拉流侧录
    virtual bool startPullRecording(const QString& outputPath);
    virtual void stopPullRecording();
    virtual bool isPullRecording() const;

    // 编码器查询
    virtual QStringList availableVideoEncoders() const;
};
```

`inputUrl` 在推流场景中具有 **双重语义**：

- **普通 URL**（如 `rtmp://...`、本地文件路径）→ 按常规流媒体/文件协议处理
- **内部协议前缀**（如 `__screen_capture__:...`）→ 触发内置采集+编码管线，不走 FFmpeg 输入

---

## 4. StreamFFmpeg — 核心实现

**文件：**
- 头文件：[backend/stream_ffmpeg/include/fplayer/backend/stream_ffmpeg/streamffmpeg.h](../backend/stream_ffmpeg/include/fplayer/backend/stream_ffmpeg/streamffmpeg.h)
- 实现：[backend/stream_ffmpeg/src/streamffmpeg.cpp](../backend/stream_ffmpeg/src/streamffmpeg.cpp)（~6500 行）

### 4.1 推流路由（PushInputRoute）

推流时，StreamFFmpeg 通过解析 `inputUrl` 前缀确定 **输入来源**，路由到不同的工作循环：

| 内部协议前缀 | PushInputKind | 工作循环 | 说明 |
|---|---|---|---|
| `__compose_scene__:` | ComposeScene | `pushComposeSceneLoop` | 组合画布合成后编码推流 |
| `__screen_capture__:` | ScreenCapture | `pushScreenLoop` | FFmpeg gdigrab 独立采集屏幕并编码 |
| `__screen_preview__:` | ScreenPreview | `pushScreenPreviewLoop` | 复用 DXGI 预览帧编码推流（避免二次采集） |
| `__camera_capture__:` | CameraCapture | `pushCameraLoop` | FFmpeg 独立采集摄像头并编码 |
| `__camera_preview__:` | CameraPreview | `pushCameraPreviewLoop` | 复用摄像头预览帧编码推流（避免设备二次占用） |
| `__file_transcode__:` | FileTranscode | `transcodeFileLoop` | 文件解码 → 重编码 → 推流（可调码率/尺寸/FPS） |
| `__file_preview__:` | FilePreview | `pushScreenPreviewLoop` | 复用播放器解码帧推流 |
| 其他（普通 URL） | Remux | `remuxLoop` | libavformat 转封装（流拷贝，无重编码） |

路由解析在 `streamffmpeg_helpers.cpp` 的 `parsePushInputRoute()` 中完成。

### 4.2 推流工作线程

`startPush()` → `startPushWorkerByRoute()` 根据路由类型创建 `std::thread`，线程安全的启动/停止通过：

- `m_stopRequest` (atomic\<bool\>) — 外部设置以请求停止
- `m_running` (atomic\<bool\>) — 反映当前运行状态
- `interruptCallback` — 注入 FFmpeg 的 `interrupt_callback`，使阻塞的 IO 操作可被快速中断

### 4.3 remuxLoop — 拉流/转封装核心

推流（remux 路由）和拉流共用 `remuxLoop`，是项目中最核心的网络 IO 函数（~780 行）：

```
输入 URL (RTMP/RTSP/HTTP-FLV/UDP/本地文件)
    ↓ avformat_open_input (支持重试 + listener 模式等待)
    ↓ avformat_find_stream_info
    ↓ avformat_alloc_output_context2
    ↓ 逐流复制 codecpar
    ↓ avformat_write_header
    ↓ while (av_read_frame) {
    │     // 时间戳重新基化（避免 dts/pts 不连续）
    │     av_packet_rescale_ts(pkt, ...)
    │     av_interleaved_write_frame(ofmt, pkt)
    │     // 可选的预览解码（拉流时解码视频到 YUV420P → ScreenFrameBus）
    │     // 可选的音频预览播放（Qt QAudioSink）
    │     // 可选的拉流侧录（同步写入第二个 AVFormatContext）
    │  }
    ↓ av_write_trailer
```

**关键特性：**

- **Listener 模式**：检测到 `listen=1`、`mode=listener` 或 `udp://0.0.0.0:` 时，不立即从输入读取，而等待远程连接建立
- **拉流预览**：在 remux 同时解码视频帧到 YUV420P，发布到 `ScreenFrameBus`（通道 ID `pull_preview`），供 FGLWidget 渲染
- **音频预览**：拉流时可选将音频解码并通过 Qt `QAudioSink` 播放
- **拉流侧录**：`startPullRecording(path)` 可在拉流运行中动态开启第二个输出文件，实时录制 MP4/FLV
- **输出格式自动选择**：UDP 输出 → `mpegts`，RTMP 输出 → `flv`，文件输出 → 根据扩展名自动探测
- **UDP pkt_size 自动补齐**：UDP 输出自动加上 `pkt_size=1316` 参数（MPEG-TS 最优值）

### 4.4 编码推流工作循环

`pushScreenLoop` / `pushCameraLoop` / `pushComposeSceneLoop` 等采用统一模式：

```
1. 打开/创建视频输入源（FFmpeg device / 从 FrameBus 快照 / 合成缓冲区）
2. 打开音频输入（WASAPI loopback / 麦克风 / off）
3. 创建视频编码器（h264_nvenc → h264_amf → h264_qsv → libx264 逐级 fallback）
4. 创建音频编码器（AAC）
5. 打开输出（avformat_alloc_output_context2 → avio_open2 → avformat_write_header）
6. while (running) {
     采集/获取一帧 → sws_scale（缩放/格式转换）→ avcodec_send_frame → avcodec_receive_packet
     读取音频 → swr_convert → FIFO → avcodec_send_frame → avcodec_receive_packet
     av_interleaved_write_frame
   }
7. av_write_trailer
```

### 4.5 音频管线

**音频输入源（推流时）：**

| 值 | 含义 |
|---|---|
| `off` | 不采集音频 |
| `loopback` | WASAPI 系统声音回环捕获（Windows） |
| `microphone` / 设备名 | 麦克风/指定音频输入设备 |
| `:0` / `:1` 等后缀 | 设备索引 |

**音频处理管线：**

```
WASAPI Loopback / FFmpeg 音频设备
    ↓ readInterleaved / av_read_frame
    ↓ swr_convert（重采样为编码器要求的格式）
    ↓ AVAudioFifo（缓冲队列）
    ↓ 可选第二路混音（麦克风 + 系统声音混合）
    ↓ fillAudioEncFrameFromFifos（从 FIFO 填充编码帧）
    ↓ avcodec_send_frame → avcodec_receive_packet
    ↓ av_interleaved_write_frame
```

**相关文件：**
- [backend/stream_ffmpeg/src/audio_pipeline.h](../backend/stream_ffmpeg/src/audio_pipeline.h) / `.cpp` — 音频 FIFO、平面混音
- [backend/stream_ffmpeg/src/platform/windows/wasapiloopbackcapture.h](../backend/stream_ffmpeg/src/platform/windows/wasapiloopbackcapture.h) / `.cpp` — WASAPI 回环捕获封装
- [backend/stream_ffmpeg/src/platform/audioinputprobe.h](../backend/stream_ffmpeg/src/platform/audioinputprobe.h) / `.cpp` — FFmpeg 音频设备探测与打开（含 Linux 实现）

---

## 5. 视频编码器选择

`pickVideoEncoderCandidates()` 按优先级返回可用编码器：

| 优先级 | 编码器 | 类型 | 平台 |
|---|---|---|---|
| 1 | `h264_nvenc` | NVIDIA GPU 硬编 | Windows/Linux |
| 2 | `h264_amf` | AMD GPU 硬编 | Windows |
| 3 | `h264_qsv` | Intel QuickSync 硬编 | Windows/Linux |
| 4 | `h264_vaapi` | VAAPI 硬编 | Linux |
| 5 | `h264_videotoolbox` | VideoToolbox 硬编 | macOS |
| 6 | `libx264` | CPU 软编 | 全平台 |

用户可通过 `PushOptions::videoEncoder` 指定偏好（`auto` = 自动选择最优先可用硬件编码器，`cpu` = 强制 CPU 软编）。

硬件编码器可用性在启动时通过诊断日志输出（检测 `avcodec_find_encoder_by_name` 返回值 + D3D11VA 设备可用性）。

**像素格式适配：** `pickEncoderPixelFormat()` 根据编码器支持的格式选择 YUV420P 或 NV12，硬编优先使用 NV12。

---

## 6. Service 层 — 推流场景路由

**文件：** [service/src/service.cpp](../service/src/service.cpp) `streamStartPushByScene()`

`Service` 提供三个推流场景的封装，将 UI 的 `PushOptions` 翻译为内部协议前缀：

### 6.1 PushScene::Screen

```
DXGI 后端:  __screen_preview__:fps=N;x=X;y=Y;size=WxH;outsize=OWxOH;bitrate=B;encoder=E;audio_in=...
FFmpeg 后端: __screen_capture__:fps=N;x=X;y=Y;size=WxH;outsize=OWxOH;bitrate=B;encoder=E;audio_in=...
```

- DXGI 路径复用现有预览帧（不二次采集）
- FFmpeg 路径独立 gdigrab 采集

### 6.2 PushScene::File

```
FFmpeg 后端 + 无需转码:  直接传递文件路径 → remuxLoop（流拷贝）
FFmpeg 后端 + 需要转码:   __file_transcode__:src64=BASE64;fps=N;size=WxH;bitrate=B
Qt6 后端:               __file_preview__:sourceid=BUS_ID;fps=N;...
```

- 文件转码时可通过 `keepAspectRatio` 保持宽高比

### 6.3 PushScene::Camera

```
有摄像头设备:  __camera_preview__:video=DEVICE;fps=N;size=WxH;bitrate=B;encoder=E;audio_in=...;audio_out=...
无摄像头设备:  __camera_capture__:audio_in=...;audio_out=...
```

- 复用摄像头预览帧，避免设备独占冲突
- 输出尺寸自动对齐偶数（`& ~1`）

### 6.4 PullScene（拉流）

```cpp
bool streamStartPull(const QString& inputUrl, const QString& outputUrl);
```

- `inputUrl`：拉流源地址（RTMP/RTSP/HTTP-FLV/UDP）
- `outputUrl`：可选本地保存路径（空字符串表示仅预览不保存）

---

## 7. Service 网关编排

**文件：** [widget/src/capturewindow.cpp](../widget/src/capturewindow.cpp)（静态函数）

### 7.1 两种连接模式

| 模式 | 说明 | 使用场景 |
|---|---|---|
| **P2P 直连** | 直接输入 RTMP/RTSP URL，不经过服务网关 | 两台设备在同一局域网 |
| **Service 编排** | 通过 HTTP API 获取动态推拉流地址 | 多设备跨网段、需要流管理 |

### 7.2 推流编排流程

```
Desktop → POST /api/v1/streams/start
          Body: { app, stream, serviceMode, publisherMeta, sourceMeta }
          ← Response: { publishRtmp, playHttpFlv, id }
          → 使用 publishRtmp 作为推流输出地址
```

`requestServiceStreamStart()` 函数（capturewindow.cpp:306）使用 `QNetworkAccessManager::post` 同步 HTTP 请求（阻塞事件循环，5s 超时）。

### 7.3 拉流编排流程

```
Desktop → GET /api/v1/streams/resolve?app=xxx&stream=xxx
          ← Response: { publishRtmp, playHttpFlv, playUrls: { httpFlv, rtmp } }
          → 使用获取到的 URL 作为拉流输入地址
```

`requestServiceStreamStatus()` 函数（capturewindow.cpp:426）使用 `QNetworkAccessManager::get`。

### 7.4 URL 路径构建

网关 API 路径支持灵活的 base URL 配置：

- 若 base URL path 为空或 `/` → 添加 `/api/v1/streams/start`
- 若 base URL path 已包含前缀 → 拼接 `/streams/start`
- 否则 → 拼接 basePath + `/api/v1/streams/start`

---

## 8. 支持的流媒体协议

| 协议 | 用途 | 实现方式 |
|---|---|---|
| **RTMP** | 推流输出（主要）、拉流输入 | FFmpeg libavformat（flv muxer/demuxer） |
| **RTSP** | 拉流输入 | FFmpeg libavformat（rtsp demuxer） |
| **HTTP-FLV** | 拉流输入（低延迟） | FFmpeg libavformat（flv over HTTP） |
| **SRT** | 推流输出（可靠 UDP 传输） | FFmpeg libavformat（mpegts muxer over SRT） |
| **UDP** | 推流输出（局域网组播） | FFmpeg libavformat（mpegts muxer over UDP） |

### 8.1 输出格式自动匹配

- RTMP URL → `flv` 封装
- UDP URL → `mpegts` 封装 + 自动 `pkt_size=1316`
- 本地文件 → 根据扩展名自动探测封装格式
- 预览模式（无输出）→ 输出到 `NUL` (Win) 或 `/dev/null` (Unix)

---

## 9. AI 聊天 — SSE 流式通信

**文件：**
- [service/include/fplayer/service/aiservice.h](../service/include/fplayer/service/aiservice.h)
- [service/src/aiservice.cpp](../service/src/aiservice.cpp)

### 9.1 通信模式

`AiService` 使用 `QNetworkAccessManager` 向 OpenAI 兼容 API 端点发送 HTTP POST 请求，通过 **SSE (Server-Sent Events)** 流式接收回复：

```
POST {endpoint}
Header: Authorization: Bearer {apiKey}
Header: Content-Type: application/json
Body: {
    "model": "...",
    "messages": [{ "role": "user", "content": [{ "type": "text", "text": "..." },
                                                { "type": "image_url", "image_url": { "url": "data:image/jpeg;base64,..." }}] }],
    "stream": true
}
    ↓
Server → SSE stream: data: {"choices":[{"delta":{"content":"文本片段"}}]}
    ↓
AiService → signal readyRead → 逐块解析 → emit textDelta → UI 更新
```

### 9.2 关键设计

- **流式解析**：增量解析 `data:` 行，每次触发 `textDelta` 信号更新聊天 UI
- **图像分析**：支持将图库中的图片编码为 base64 内联到请求中
- **可配置**：端点 URL、API key、模型名称均可通过 UI 设置
- **QNetworkAccessManager 复用**：成员变量 `m_net`，请求完成后通过 `sender()` 判断来源

---

## 10. 线程模型

```
┌─ GUI 主线程 ──────────────────────────────────────────────┐
│  CaptureWindow / Service / FGLWidget                       │
│  ├── 用户触发 startPush / startPull                        │
│  ├── 创建 worker std::thread                               │
│  └── 定时器轮询日志/状态                                    │
└────────────────────────────────────────────────────────────┘
         │ m_stopRequest = true
         ▼
┌─ Worker 线程 (std::thread) ────────────────────────────────┐
│  remuxLoop / pushScreenLoop / transcodeFileLoop ...         │
│  ├── av_read_frame (可被 interruptCallback 打断)            │
│  ├── avcodec_send_frame / avcodec_receive_packet           │
│  ├── av_interleaved_write_frame                             │
│  ├── 拉流预览帧 → ScreenFrameBus::publish（线程安全）        │
│  └── 日志 → appendLogLine（QMutex 保护）                    │
└────────────────────────────────────────────────────────────┘
```

- **停止流程**：`stop()` 设置 `m_stopRequest=true` → `interruptCallback` 中断阻塞 IO → `m_worker->join()` 等待线程退出
- **帧发布**：`ScreenFrameBus::publish()` 内部使用 `QMutex`，可在 worker 线程安全发布

---

## 11. 构建配置

### 11.1 stream_ffmpeg 模块

[backend/stream_ffmpeg/CMakeLists.txt](../backend/stream_ffmpeg/CMakeLists.txt)：

```cmake
find_package(Qt6 COMPONENTS Core Multimedia REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE Qt6::Core Qt6::Multimedia)
target_link_libraries(${PROJECT_NAME} PUBLIC FPlayer_Api FPlayer_Common)
target_link_libraries(${PROJECT_NAME} PRIVATE
    avdevice avfilter avformat avcodec swscale swresample avutil)
if (WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE ksuser uuid)
endif()
```

- `Qt6::Multimedia` 仅用于拉流预览音频播放（`QAudioSink`），由 `FPLAYER_PREVIEW_AUDIO_WITH_QT` 宏控制
- FFmpeg 库来自 `3rd/ffmpeg_v8.1/` 捆绑的预编译包

### 11.2 CMake 开关

```cmake
-DFPLAYER_BUILD_STREAM_FFMPEG=ON   # FFmpeg 推拉流（主）
-DFPLAYER_BUILD_NET_QT6=ON         # Qt6 网络后端（桩）
-DFPLAYER_BUILD_RTC_WEBRTC=OFF     # WebRTC（未实现）
```

---

## 12. 推拉流日志与诊断

StreamFFmpeg 提供了丰富的诊断信息：

- **启动诊断**：输出 FFmpeg 构建配置、编码器可见性（NVENC/AMF/QSV）、D3D11VA 设备状态
- **运行日志**：保存最近 16KB 日志（`m_recentLog` / `m_recentPushLog` / `m_recentPullLog`）
- **错误输出**：`m_lastError` 保存最近错误消息
- **退出码**：`m_lastExitCode` 保存 FFmpeg 函数的 av error code
- **FFmpeg 全局日志接管**：`av_log_set_callback` 将所有 FFmpeg 日志重定向到项目 Logger

---

## 13. 关键文件索引

| 文件 | 说明 |
|---|---|
| [api/include/fplayer/api/net/istream.h](../api/include/fplayer/api/net/istream.h) | IStream 抽象接口 |
| [backend/stream_ffmpeg/include/.../streamffmpeg.h](../backend/stream_ffmpeg/include/fplayer/backend/stream_ffmpeg/streamffmpeg.h) | StreamFFmpeg 类定义（8 个推流循环 + remuxLoop） |
| [backend/stream_ffmpeg/src/streamffmpeg.cpp](../backend/stream_ffmpeg/src/streamffmpeg.cpp) | 核心实现（~6500 行） |
| [backend/stream_ffmpeg/src/streamffmpeg_helpers.cpp](../backend/stream_ffmpeg/src/streamffmpeg_helpers.cpp) | 路由解析、编码器选择、参数解析 |
| [backend/stream_ffmpeg/src/audio_pipeline.cpp](../backend/stream_ffmpeg/src/audio_pipeline.cpp) | 音频混音与 FIFO 管线 |
| [backend/stream_ffmpeg/src/platform/windows/wasapiloopbackcapture.cpp](../backend/stream_ffmpeg/src/platform/windows/wasapiloopbackcapture.cpp) | WASAPI 系统声音回环捕获 |
| [backend/stream_ffmpeg/src/platform/audioinputprobe.cpp](../backend/stream_ffmpeg/src/platform/audioinputprobe.cpp) | FFmpeg 音频设备探测 |
| [backend/net_qt6/src/streamqt6.cpp](../backend/net_qt6/src/streamqt6.cpp) | Qt6 网络后端（空桩，默认不构建） |
| [service/src/service.cpp](../service/src/service.cpp) | Service 层推拉流编排（streamStartPushByScene 等） |
| [widget/src/capturewindow.cpp](../widget/src/capturewindow.cpp) | 推拉流 UI + service 网关 HTTP 请求（requestServiceStreamStart / requestServiceStreamStatus） |
| [service/src/aiservice.cpp](../service/src/aiservice.cpp) | AI 聊天 SSE 流式通信 |
