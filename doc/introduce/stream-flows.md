# FPlayer Desktop — 推拉流流程

## 1. 概述

推拉流模块由 `StreamFFmpeg` 统一承载，作为 `IStream` 接口的唯一实现。该模块独立于 UI 线程运行于 `std::thread`，通过 `startPush()`/`startPull()` 启动，`stop()` 回收。输入源、编码参数、输出 URL 由 spec 字符串描述，经 `streamffmpeg_helpers` 解析后路由至对应处理循环。

---

## 2. 推流 (Push)

### 2.1 场景路由

`Service` 层调用 `streamStartPushByScene(PushScene, outputUrl, options)`，依据 `PushScene` 枚举构建 spec 字符串：

| PushScene | spec 前缀 | 数据来源 |
|-----------|-----------|----------|
| Camera | `__camera_preview__:video=...;size=...;fps=...` | CameraFrameBus |
| File | 文件路径 | avformat 解封装 |
| Screen | `__screen_capture__:...` | ScreenFrameBus |
| Compose | 多源合成参数 | 组合画布混合帧 |

### 2.2 编码与传输

spec 传入 `StreamFFmpeg::startPush()`，后台线程按 spec 类型分派：

- **remuxLoop**：源为文件且无需转码时，直接 `av_read_frame → av_write_frame`，仅更换封装格式
- **pushCameraLoop / pushScreenLoop**：从对应 FrameBus 取 YUV420P 帧 → `avcodec_encode` → `av_write_frame`；音频通过 `AudioPipeline` 经 WASAPI loopback 捕获，swresample 重采样后混入
- **compose 模式**：多源 YUV 帧按 Z-order 叠加 → 统一编码输出

### 2.3 输出协议

avformat 封装层支持 **RTMP / RTSP / SRT / HTTP-FLV**，输出 URL 由用户在 UI 中指定。推流过程中状态通过 Qt 信号槽回调至 `CaptureWindow` 日志面板。

---

## 3. 拉流 (Pull)

### 3.1 入口

用户输入拉流 URL → `Service::streamStartPull(inputUrl, options)` → `StreamFFmpeg::startPull()`。options 中包含 `recordPath`（录制路径，可选）和 `enablePreview`（是否启用预览）。

### 3.2 解封装与分流

后台线程调用 `avformat_open_input` 打开网络流，获取流信息后分叉两路：

1. **录制路径**：`av_read_frame → av_write_frame`，将原始编码帧直接写入本地 MP4/FLV/MKV 文件，不经解码-重编码
2. **预览路径**：`avcodec_decode` 解码为 YUV420P，写入 `ScreenFrameBus`，由 `FGLWidget` 消费渲染，同时输出音频至系统播放设备

### 3.3 日志

拉流过程中 SPS/PPS 参数集及码流统计信息通过信号槽推送 UI 展示。流结束或用户主动调用 `stop()` 后释放 avformat/avcodec 上下文。

---

## 4. 共用基础设施

推流和拉流共享以下组件：

- **CameraFrameBus / ScreenFrameBus**：线程安全 YUV 帧总线，mutex + sourceId 分区，解耦生产与消费
- **FGLWidget**：OpenGL YUV 渲染器，BT.601/BT.709 色彩矩阵
- **AudioPipeline + WASAPI**：Windows 音频 loopback 采集，用于推流混音
- **streamffmpeg_helpers**：spec 字符串解析，决定路由分支

---

## 5. 设计约束

- 生命周期：`StreamFFmpeg` 实例随 `RunTime::createStream()` 创建，随进程退出销毁；推拉流任务通过内部状态机管理，同一实例同一时刻仅允许一个活跃流
- 线程安全：编码/网络 I/O 在独立 `std::thread` 中执行，帧总线以 mutex 保证并发读写安全
- 错误处理：网络中断或编码失败通过回调通知 UI，线程安全退出，不阻塞主循环
