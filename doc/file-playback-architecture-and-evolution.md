# 文件播放模块开发文档（FFmpeg 后端）

## 1. 目标与范围

本模块在现有分层架构中新增“本地音视频文件播放”，并保证：

- 摄像头能力不回归；
- 文件模式可播放、暂停、seek、倍速；
- 具备可维护的分层与可观察日志；
- 优先稳定性，再追求高性能。

---

## 2. 分层设计与职责

### 2.1 API 层

`IPlayer` 提供统一能力：

- 文件：`openFile()`
- 控制：`play()/pause()/stop()`
- 进度：`durationMs()/positionMs()/seekMs()`
- 速率：`setPlaybackRate()/playbackRate()`
- 渲染：`setPreviewTarget()`

### 2.2 Runtime 层

- `createPlayer(MediaBackendType)` 创建播放器
- `bindPlayerPreview()` 绑定预览目标

### 2.3 Service 层

面向 UI 的播放器编排：

- 初始化与绑定：`initPlayer()/bindPlayerPreview()`
- 播放控制：`playerPause()/playerResume()/playerStop()`
- 进度与 seek：`playerDurationMs()/playerPositionMs()/playerSeekMs()`
- 倍速：`playerSetPlaybackRate()/playerPlaybackRate()`

### 2.4 Widget 层

`CaptureWindow` 文件模式交互：

- 菜单模式切换（摄像头/文件）
- 文件模式隐藏摄像头下拉
- 进度条 + 时间文本 + 拖拽 seek
- 倍速下拉（1/1.25/1.5/2x）
- 菜单栏中部滚动显示文件标题，点击标题可切换文件

### 2.5 Backend 层（`PlayerFFmpeg`）

- FFmpeg 读包与解码
- 视频：解码 -> 必要时 `sws_scale` -> YUV 渲染
- 音频：解码 -> `swr` 重采样 -> `QAudioSink`
- 维护状态、时钟、队列、同步与日志

---

## 3. 线程模型与队列

当前已落地三线程模型：

- `demuxThread`：`av_read_frame`，按 stream 分发
- `decodeThread`：消费视频包，解码并执行视频同步
- `audioThread`：消费音频包，解码/重采样/写入设备

队列：

- `videoPackets`：视频包队列
- `audioPackets`：音频包队列

稳定性策略：

- 队列采用有界阻塞（满则 demux 等待），避免粗暴丢旧包导致时间跳变；
- 消费后 `notify` 生产者，防止背压死锁；
- `seek/EOF/stop/cleanup` 会 flush codec + 清空队列 + 重置时钟状态。

---

## 4. 同步策略（当前稳定基线）

当前版本以稳定性为首要目标，采用：

- 视频主同步：`video PTS + monotonic clock`
- 音频线程独立输出，不反向强主导视频节奏
- 视频落后超过阈值时丢帧追赶
- 进度由视频时间推进，seek 后重建时钟基线

说明：

- 历史上尝试过音频设备主时钟（`processedUSecs`）与偏置校正；
- 在当前组合下该策略出现严重回归，因此当前回到稳定基线。

---

## 5. 关键优化与修复（已完成）

### 5.1 音频稳定性

- 输出格式使用默认音频设备 `preferredFormat`
- `swr` 首帧懒初始化 + 声道布局兜底
- 音频写入处理部分写入，减少断续
- `QAudioSink` 缓冲加大，降低调度抖动影响

### 5.2 性能优化

- demux 分发改为 `av_packet_move_ref`，减少包克隆开销
- 三线程解耦音视频，降低互相阻塞
- 渲染端高频空日志降频，减少 UI 干扰

### 5.3 稳定性修复

- 解决“速度跑飞/撕裂/时间线跳变”：
  - 移除队列满时丢旧包策略
  - 改为有界阻塞队列
  - EOF/seek 统一重置时钟与队列

---

## 6. UI 功能现状

- 模式切换：摄像头/文件
- 文件切换入口：
  - 菜单模式切到“文件播放模式”时选文件
  - 菜单栏中部滚动标题点击可再次切换文件
- 播放控制：按钮 + 空格
- 进度：显示时长、拖拽 seek
- 倍速：1x / 1.25x / 1.5x / 2x

---

## 7. 已知限制

- 三线程已落地，但队列阈值与丢帧阈值仍是固定值；
- `FGLWidget` 仍有一次 CPU 侧重打包拷贝，4K 下成本较高；
- 未引入硬解（DXVA/NVDEC 等），CPU 压力较大；
- 同步策略当前以稳定为主，未启用更激进的音频主时钟校正。

---

## 8. 后续建议

1. 队列阈值与丢帧阈值参数化（按分辨率/码率动态调整）
2. 渲染侧减少一次 memcpy（stride 直传或 PBO）
3. 引入硬解路径（Windows 优先 D3D11VA）
4. 增加播放调试面板（队列深度、同步差、丢帧率、缓冲占用）
