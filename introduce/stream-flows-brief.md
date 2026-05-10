# FPlayer Desktop — 推拉流流程

## 1. 推流 (Push)

推流入口为 `Service::streamStartPushByScene()`，依据 `PushScene` 枚举（Camera / File / Screen / Compose）构建 spec 字符串，交由 `streamffmpeg_helpers` 解析。`StreamFFmpeg::startPush()` 在独立 `std::thread` 中按 spec 路由至对应处理循环：文件源且无需转码时走 `remuxLoop`，直接 `av_read_frame → av_write_frame`；摄像头/屏幕/组合源走 `pushCameraLoop`/`pushScreenLoop`，从 FrameBus 获取 YUV420P 帧，经 `avcodec_encode` 后写入。音频由 WASAPI loopback 采集，swresample 重采样混入。输出经 avformat 封装为 RTMP/RTSP/SRT/HTTP-FLV 流。

## 2. 拉流 (Pull)

拉流入口为 `Service::streamStartPull()`。`StreamFFmpeg::startPull()` 在后台线程调用 `avformat_open_input` 解封装网络流，随后分叉：录制路径以 `av_read_frame → av_write_frame` 直写本地 MP4/FLV/MKV，不经重编码；预览路径经 `avcodec_decode` 输出 YUV420P 至 FrameBus，由 `FGLWidget` 渲染。SPS/PPS 与码流统计通过信号槽推送 UI。

## 3. 共用组件

推拉流共用 `CameraFrameBus`/`ScreenFrameBus`（mutex 保护、sourceId 分区）、`FGLWidget`（OpenGL YUV 渲染，BT.601/BT.709）、`AudioPipeline`（WASAPI）及 `streamffmpeg_helpers`（spec 解析路由）。`StreamFFmpeg` 实例通过内部状态机管理并发，单实例同时仅持有一个活跃流。
