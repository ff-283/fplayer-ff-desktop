# 后端-媒体文件播放开发详解（基于当前项目实现）

本文档面向项目开发者，完整说明 `fplayer-ff-desktop` 中“后端-媒体文件播放”模块的设计思路、调用链路、线程模型、音视频同步、渲染路径、状态管理、性能与稳定性策略、已知问题与扩展建议。

---

## 1. 文档目标与边界

### 1.1 目标

- 统一团队对“文件播放”模块的实现认知。
- 明确各层职责与跨层边界，降低后续维护成本。
- 提供可执行的排障与优化路径，支持稳定迭代。

### 1.2 边界

- 本文聚焦本地媒体文件播放（`IPlayer` / `PlayerFFmpeg`）。
- 涉及 UI 交互（进度条、倍速、模式切换）与 OpenGL 显示链路。
- 不展开摄像头采集实现细节（摄像头链路已有独立文档）。
- 本文同时覆盖 FFmpeg 和 Qt6 两种文件播放后端，缺省为 Qt6。

---

## 2. 架构总览（分层与职责）

当前项目采用 API -> Runtime -> Service -> Widget -> Backend 分层。

### 2.1 API 层（抽象能力定义）

核心接口：`api/include/fplayer/api/media/iplayer.h`

`IPlayer` 提供统一播放器能力：

- 文件控制：`openFile()`
- 播放控制：`play() / pause() / stop()`
- 时间控制：`durationMs() / positionMs() / seekMs()`
- 倍速控制：`setPlaybackRate() / playbackRate()`
- 渲染绑定：`setPreviewTarget()`
- 调试输出：`debugStats()`

设计意义：

- 上层不依赖具体后端（Qt6/FFmpeg）。
- Runtime 可按 `MediaBackendType` 动态装配实现（`Qt6` → `PlayerQt6`，`FFmpeg` → `PlayerFFmpeg`）。

### 2.2 Runtime 层（实例装配）

核心文件：`runtime/src/runtime.cpp`、`runtime/include/fplayer/runtime/runtime.h`

- `createPlayer(MediaBackendType::Qt6)` -> `PlayerQt6`
- `createPlayer(MediaBackendType::FFmpeg)` -> `PlayerFFmpeg`
- `bindPlayerPreview()` 将 `PreviewTarget` 下发到播放器后端

设计意义：

- 统一后端创建点，避免 UI 直接 new 具体后端类。
- 支持运行时热切换后端（在系统设置中更改后即时生效，无需重启）。

### 2.3 Service 层（业务编排）

核心文件：`service/src/service.cpp`

对 UI 暴露稳定 API：

- 初始化与绑定：`initPlayer()`、`bindPlayerPreview()`
- 播放控制：`playerPause()`、`playerResume()`、`playerStop()`
- 时间控制：`playerDurationMs()`、`playerPositionMs()`、`playerSeekMs()`
- 倍速与观测：`playerSetPlaybackRate()`、`playerDebugStats()`

设计意义：

- Widget 不直接接触后端实现细节。
- 后端切换（Qt6 ↔ FFmpeg）对 UI 透明，Service 负责停止旧播放器、重新创建并恢复状态。

### 2.4 Widget 层（交互与展示）

核心文件：`widget/src/capturewindow.cpp`、`widget/src/fvideoview.cpp`

关键职责：

- 模式切换（摄像头模式 / 文件模式）
- 文件选择、播放暂停、进度拖拽、倍速选择
- 周期刷新进度与调试统计
- 预览窗口创建并绑定后端渲染目标

设计意义：

- UI 只表达交互，不承载解码/同步算法。

### 2.5 Backend 层（双后端实现）

#### Qt6 后端（默认）

核心文件：`backend/media_qt6/src/playerqt6.cpp`

实现方式：封装 Qt6 原生 `QMediaPlayer` + `QAudioOutput`。

关键职责：

- 文件打开：`QMediaPlayer::setSource(QUrl::fromLocalFile(path))`
- 播放控制：`play() / pause() / stop()` 委托给 `QMediaPlayer`
- 倍速控制：`QMediaPlayer::setPlaybackRate(rate)`
- 音量控制：`QAudioOutput::setVolume(volume)`
- 视频渲染：通过 `QMediaPlayer::setVideoOutput(QVideoWidget*)` 绑定到预览窗口
- 自动循环：`setLoops(QMediaPlayer::Infinite)`，与 FFmpeg 后端行为一致

平台适配：
- Windows：底层使用 Windows Media Foundation，支持硬件加速解码
- Linux：预留 GStreamer 管线配置扩展点（当前为 TODO 占位）

帧输出（推流用）：
- `setComposeStreamBusId()` 在 `m_videoSink` 上连接 `QVideoSink::videoFrameChanged` 信号
- `onVideoFrameChanged` 槽函数接收 `QVideoFrame`，按像素格式分派：
  - `Format_YUV420P`：三平面直接拷贝
  - `Format_NV12` / `Format_NV21`：拆分交错 UV/VU 平面为独立 U/V 平面
  - 其他格式：`QVideoFrame::toImage()` → `QImage::Format_ARGB32` → `rgbToYuv420p()` 软件转换
- 发布到 `ScreenFrameBus::publish(y, u, v, ...)`，以 `sourceId` 为通道 Key
- `m_videoSink` 来源：`QMediaPlayer::setVideoOutput(QVideoWidget*)` 后调用 `QVideoWidget::videoSink()` 获取
- 注意：必须用 `qobject_cast<QVideoWidget*>` 校验 `backend_hint`，避免 FFmpeg 视图传入的 `FGLWidget*` 导致崩溃

#### FFmpeg 后端（可选）

核心文件：`backend/media_ffmpeg/src/playerffmpeg.cpp`

关键职责：

- 解封装（demux）
- 视频解码与格式转换（`sws_scale`）
- 音频解码与重采样（`swr` + `QAudioSink`）
- 音视频同步、队列背压、seek 与 EOF 行为
- 向 OpenGL 组件投递 YUV 帧
- 向 ScreenFrameBus 发布 YUV 帧（组合推流）

---

## 3. 端到端调用链路

以“用户切换到文件模式并播放文件”为例：

**Qt6 后端路径：**
1. UI：`CaptureWindow::chooseAndPlayFile()`
2. Service：`Service::openMediaFile(path)`
3. Player：`PlayerQt6::openFile(path)`
4. 后端打开流程：
   - `QMediaPlayer::setSource(QUrl::fromLocalFile(path))`
   - 自动探测媒体格式并初始化解码管线（由系统框架完成）
   - 调用 `play()` 启动播放
5. 视频渲染：
   - `QMediaPlayer` 通过 `setVideoOutput()` 直接输出到 `QVideoWidget`
   - 无需中间 YUV 拷贝步骤

**FFmpeg 后端路径：**
1. UI：`CaptureWindow::chooseAndPlayFile()`
2. Service：`Service::openMediaFile(path)`
3. Player：`PlayerFFmpeg::openFile(path)`
4. 后端打开流程：
   - `avformat_open_input`
   - `avformat_find_stream_info`
   - 打开视频解码器（必需）
   - 打开音频解码器（可选）
   - 启动 demux/decode/audio 三线程
5. 视频帧投递：
   - decode 线程得到 YUV 数据
   - 通过 `queuePreviewYuv()` 合并投递
   - Qt 主线程 `deliverPreviewFrame()`
   - `FGLWidget::updateYUVFrame()` -> `paintGL()`

---

## 4. 线程模型与并发设计

### 4.1 三线程职责

- `demuxThread`
  - 持续 `av_read_frame`
  - 将包分发到 `videoPackets` / `audioPackets`
  - 响应 seek 请求（flush + 清队列 + 状态重置）

- `decodeThread`（视频）
  - 消费视频包并解码成 `AVFrame`
  - 非 YUV420P 时做 `sws_scale` 转换
  - 执行视频端同步与必要丢帧
  - 将最终帧投递到预览组件

- `audioThread`
  - 消费音频包并解码
  - `swr` 重采样到输出设备格式
  - 写入 `QAudioSink` 设备缓冲
  - 维护音频时钟（用于 AV 同步）

### 4.2 并发原语

- `std::mutex + std::condition_variable`
- `std::atomic` 状态位（paused/running/stopRequested/seekRequestMs 等）
- 编解码器对象使用单独互斥锁保护（`videoCodecMutex` / `audioCodecMutex`）

### 4.3 队列策略（稳定性优先）

当前策略要点：

- 音视频队列均为有界队列，满时背压等待。
- 普通播放阶段不再丢视频压缩包（避免参考帧链断裂导致宏块失真）。
- seek 期间允许“丢旧包保新包”，用于快速靠近目标时间点。

---

## 5. 时间轴与音视频同步设计

### 5.1 PTS 换算

- 使用 `framePtsMs(frame, stream)` 将帧时间戳换算为毫秒。
- 统一以 `best_effort_timestamp` 优先，缺失时回退 `pts`。

### 5.2 同步核心思想

- 有音频时：视频对齐音频时钟。
- 无音频时：视频按单调时钟推进（`av_gettime_relative`）。

### 5.3 MOV 兼容关键点（已纳入）

问题背景：部分 `.mov` 存在音轨/视频轨起点偏移，直接比较原始 PTS 会导致视频被连续判慢并丢帧，表现为“有声音、无画面”。

当前修复策略：

- 新增同步基线：
  - `avSyncVideoBaseMs`
  - `avSyncAudioBaseMs`
- 同步比较使用归一化时间：
  - `normVideo = videoPts - videoBase`
  - `normAudio = audioClock - audioBase`
  - `drift = normVideo - normAudio`
- 在 seek/EOF/cleanup 时重置基线，防止跨段污染。

### 5.4 seek 与时间收敛

- `seekMs()` 只下发请求，不阻塞 UI。
- demux 线程处理 seek 后：
  - `avcodec_flush_buffers`
  - 清空音视频包队列
  - 重置时钟与同步版本
- 视频线程在 `seekTargetMs` 存在时先丢旧帧直到接近目标，再恢复常规播放。

---

## 6. 视频渲染链路（OpenGL）

### 6.1 组件关系

- `FVideoView` 根据后端类型创建 `FGLWidget`
- `PlayerFFmpeg::setPreviewTarget()` 保存 `FGLWidget*`
- 视频帧通过 Qt `QueuedConnection` 投递到 UI 线程

### 6.2 数据路径

1. 解码线程拿到 `AVFrame`
2. 转成 YUV420P（必要时）
3. 三平面 `QByteArray`（Y/U/V）投递
4. `FGLWidget` 缓存最新帧并触发 `update()`
5. `paintGL()` 中上传纹理并绘制

### 6.3 Shader 与纹理模型

- 三纹理：`texY` / `texU` / `texV`
- 单通道纹理（`R8` + `GL_RED`）
- 片段着色器做 YUV->RGB 变换
- 顶点计算采用 contain 等比显示，避免拉伸

### 6.4 stride 与宏块失真修复（已纳入）

历史问题：部分平台/驱动在 `GL_UNPACK_ROW_LENGTH` 路径上出现错行、花屏、宏块。

当前策略：

- 优先使用 `UNPACK_ROW_LENGTH`（性能优先）。
- 若环境不可靠（尤其部分 GLES/ANGLE 路径）：
  - 逐行重打包为紧密缓冲区再上传（稳定优先）。
- UV 宽高按向上取整计算：`(w+1)/2`、`(h+1)/2`，兼容奇数分辨率。

---

## 7. 音频输出链路

### 7.1 输出设备与格式

- 使用默认音频设备 `preferredFormat`
- `QAudioSink` 作为设备输出端
- 启动时设置较大缓冲（降低调度抖动影响）

  ### 7.2 重采样与倍速处理

- `swr` 在首帧懒初始化，使用真实帧参数构建输入侧配置
- 输出侧**始终输出到设备原生采样率**（如 48000Hz），不做速率相关的采样率变更
- **倍速通过 FFmpeg `atempo` 音频滤镜实现**（WSOLA 算法）：
  - 构建 `abuffer → atempo → abuffersink` 滤镜图管线
  - `tempo=1.0` 时跳过滤镜（直通路径，零开销）
  - `tempo>1.0` 时 swr 输出经 atempo 在时域压缩，保持原始音调不变
- 倍速切换时：先 flush atempo 残留数据，再销毁旧滤镜图，新帧触发重建（携带新 tempo 参数）

### 7.3 时钟维护（FFplay 方式）

音频时钟公式：
```
audioClockMs = playbackOriginMs + (nowProcessedUs - startProcessedUs) * rate / 1000
```

- `playbackOriginMs`：当前播放段的媒体时间原点（开始=0，seek=目标ms，倍速切换=当前时钟快照）
- `startProcessedUs`：原点对应的 `QAudioSink::processedUSecs()` 值（-1 表示惰性初始化）
- `nowProcessedUs`：设备此刻已处理的微秒数
- `rate`：当前播放倍速（1.0~2.0）

关键设计点：
- 时钟基于设备实际播放位置（`processedUSecs`），天然包含 atempo 缓冲延迟、设备驱动延迟等所有环节，写入但未播放的数据不会预支时钟
- 时钟随设备播放平滑单调递增，不会在音频帧之间跳跃
- 倍速切换时**快照当前时钟值到 `playbackOriginMs` + 重置 `startProcessedUs`**，确保旧速率下累积时间不被新速率乘算（消除跨速率串扰）
- seek 后由 demux 线程预置 `playbackOriginMs=seekMs`，音频线程在 reset sink 后填入实际 `startProcessedUs`
- 采用 FFplay / mpv / VLC 等主流播放器的同一方案

---

## 8. UI 交互与状态管理

### 8.1 文件模式 UI

- 显示进度条、时长文本、倍速下拉、调试统计
- 隐藏摄像头设备控件
- 后端类型影响 `FVideoView` 内部创建的渲染组件：
  - Qt6 后端 → `QVideoWidget`（系统框架渲染）
  - FFmpeg 后端 → `FGLWidget`（OpenGL YUV 渲染）

### 8.2 进度条

- 定时器周期刷新当前位置/总时长
- 拖拽释放后调用 `playerSeekMs(value)`
- 拖拽中暂停外部覆盖，避免手感冲突

### 8.3 后端热切换

用户可在「系统设置」中切换文件播放后端（Qt6 / FFmpeg），保存后即时生效：

1. 停止当前播放器并释放资源
2. 按新后端类型重新 `initPlayer()`
3. 更新 `FVideoView` 后端类型（触发内部组件重建）
4. 若当前正在播放：保存 `positionMs` → 重新 `openFile()` → `seekMs()` 恢复到原进度 → 恢复播放/暂停状态

### 8.4 非全屏窗口横向扩展问题（已处理）

根因：底部调试文本宽度随内容增长，可能抬高布局最小宽度，触发窗口被动扩宽。

处理：

- 对进度文本与调试文本设置固定宽度，稳定布局边界。

---

## 9. 关键状态变量说明（PlayerFFmpeg）

建议重点关注以下状态：

- 生命周期：`stopRequested`、`paused`、`running`
- 时间轴：`durationMs`、`currentPosMs`、`seekRequestMs`、`seekTargetMs`
- 音频时钟：`audioClockMs`、`audioPlaybackOriginMs`、`audioPlaybackStartProcessedUs`
- 倍速滤镜：`audioFilterGraph`、`audioBufSrcCtx`、`audioBufSinkCtx`（atempo 管线）
- 同步版本：`syncVersion`（seek/EOF 后跨线程重置标记）
- AV 基线：`avSyncVideoBaseMs`、`avSyncAudioBaseMs`
- 渲染投递：`previewHasPending`、`previewDeliverQueued`

---

## 10. 日志与可观测性

当前已有基础观测输出：

- `droppedVideo/2s`
- `audioFrames/2s`
- `audioBytes/2s`
- `QAudioSink` 状态日志

建议团队形成统一排障步骤：

1. 先看是否大量 `droppedVideo`
2. 再看音频输出是否稳定（frames/bytes 是否持续）
3. 观察 seek 前后时钟是否重置
4. 对问题样本区分：容器时间轴问题 vs 渲染上传问题 vs 码流损坏

---

## 11. 常见故障与定位指南

### 11.1 有声音无画面（常见于部分 MOV）

优先检查：

- AV 基线是否建立（video/audio base 是否有效）
- 视频是否被持续判慢而丢帧
- seek/EOF 后是否正确重置基线

### 11.2 宏块失真/马赛克

优先检查：

- 是否发生压缩包丢弃（普通播放阶段必须禁止）
- OpenGL 上传路径是否退化到兼容重打包
- UV 尺寸/stride 计算是否匹配

### 11.3 进度条末尾不准

与编码结构相关因素：

- 关键帧间隔（GOP）决定可精确跳转粒度
- backward seek 回退行为在末尾更明显
- EOF 自动回环策略可能影响末尾体验

---

## 12. 本轮问题复盘与正确修复方法（重要）

本节记录本项目这轮真实遇到的问题、错误修复方式（踩坑）与最终正确方案，后续开发请优先参考。

### 12.1 问题：MP4 播放出现宏块失真/马赛克

- **现象**
  - 高动态场景出现局部块化、画面破损，常持续到下一个关键帧。
- **根因**
  - 普通播放阶段视频队列满时丢弃了压缩包（`AVPacket`），破坏了 H.264/H.265 参考帧链。
  - 部分平台下 `GL_UNPACK_ROW_LENGTH` 直传 stride 兼容性不稳定。
- **正确修复**
  - 普通播放阶段禁止丢视频压缩包，改为背压等待；仅 seek 期间允许丢旧包保新包。
  - OpenGL 上传增加兼容兜底：不可靠环境退化为逐行重打包后上传。
  - UV 平面尺寸统一使用向上取整：`(w+1)/2`、`(h+1)/2`。

### 12.2 问题：部分 MOV 有声音无画面

- **现象**
  - 进度推进正常，有声音，但视频不出图。
- **根因**
  - 音视频轨首 PTS 基线不一致，直接比较原始 PTS 导致视频线程长期判定“落后过多”并持续丢帧。
- **正确修复**
  - 增加 AV 同步基线（视频基线 + 音频基线）。
  - 用归一化时间计算漂移：`(videoPts-videoBase) - (audioClock-audioBase)`。
  - 在 seek/EOF/cleanup 时重置基线，避免跨段污染。

### 12.3 问题：非全屏播放几秒后窗口横向自动变宽

- **现象**
  - 不全屏播放时，窗口会在播放中自行横向扩展。
- **根因**
  - 底部动态文本（调试统计/时间文本）长度增长，抬高布局最小宽度，导致父窗口被动扩宽。
- **正确修复**
  - 为动态文本控件设置固定宽度，控制布局边界。
  - 动态统计文本使用普通文本格式，避免富文本测量差异。

### 12.4 问题：视频在末尾前提前跳回开头

- **现象**
  - 例如 46s 视频在约 40s 左右直接跳到开头，不论是否拖动进度条。
- **根因**
  - demux 线程一旦读到 EOF 立即 seek 到 0，未等待已在队列中的音视频数据消费完。
- **正确修复**
  - EOF 到达先只打标记，不立刻回环。
  - 等音视频队列都消费完成后再执行循环 seek。
  - 回环触发前将 `currentPosMs` 推到 `durationMs`，确保用户看到完整结尾。

### 12.5 问题：修复末尾回环后，进度条失效（回归）

- **现象**
  - 进度条出现异常回零/不连续，拖拽后表现为“失效”。
- **错误修复方式（禁止）**
  - 在每次 seek 时重置全局时间轴原点（`timelineOriginMs`）。
- **根因**
  - 时间轴原点被频繁重置，导致 `positionMs` 从新的局部起点重新计算，不再代表全片时间。
- **正确修复**
  - `timelineOriginMs` 仅在打开新文件或全量 cleanup 时初始化/重置。
  - seek 过程中不重置全局时间轴原点，保持进度条以全片时间轴推进。

### 12.6 问题：倍速播放音画不同步 & 声音变尖锐

- **现象**
  - 切换到 1.25x / 1.5x / 2.0x 后声音变尖锐（花栗鼠效应），且画面与声音逐渐错位。
  - 切换回 1.0x 后不同步现象持续。
- **根因**
  - 旧方案通过 `swrOutRate = deviceRate / rate` 改变重采样输出速率。这导致两个问题：
    1. 频率域压缩 → 所有频率按 `rate` 倍上移 → 花栗鼠效应；
    2. 每音频帧产出的 PCM 字节数降为 `1/rate`，设备以恒定速率消耗 → 缓冲欠跑 → 断续/错位。
  - 倍速切换时 `audioClockBasePtsMs` 未重置，旧速率下累积的 `playedUs` 被新速率乘算 → 音频时钟跳变 → 视频错位。
  - 恢复 1.0x 后时钟基线已被污染，不同步不可逆。
- **正确修复**
  - swr **恒定输出设备采样率**（`swrOutRate = outSampleRate`），不再除以 rate。
  - 倍速改用 FFmpeg `atempo` 音频滤镜（WSOLA 算法）在时域压缩，保持音调不变。
  - 音频时钟采用 FFplay 方式：`playbackOriginMs + (nowProcessedUs - startProcessedUs) * rate / 1000`。
  - 倍速切换时快照当前时钟到 `playbackOriginMs`、重置 `startProcessedUs`，消除跨速率串扰。

### 12.7 问题：退出文件播放模式导致程序未响应

- **现象**
  - 从文件播放模式切换到摄像头/屏幕/组合模式时，程序概率性卡死。
- **根因**
  - 模式切换代码仅调用 `playerPause()`（暂停线程但不 join），随后 `setBackendType()` 销毁了 `FGLWidget`。
  - `PlayerFFmpeg::Impl::glWidget` 仍指向已释放的内存，后续 `deliverPreviewFrame` 或线程竞争触发 use-after-free。
  - `audioSink->suspend()` 与音频线程 write 操作存在竞态。
- **正确修复**
  - `switchToCameraMode` / `switchToScreenMode` / `setComposeMode(true)` 中全部将 `playerPause()` 替换为 `playerStop()`。
  - `playerStop()` 完整执行：通知线程停止 → join 等待退出 → 清理预览状态 → 重置音频设备。
  - `deliverPreviewFrame` 在 `previewHasPending=false` 时跳过 glWidget 访问，与 `stopThread()` 的清理构成安全序列。

### 12.8 问题：音频时钟双重计数导致持续偏差（回归修复）

- **现象**
  - 修复倍速不同步后，1.0x 正常播放仍存在微小音画偏差（视频略快于音频）。
- **根因**
  - 初次修复时使用纯字节计数时钟 `basePts + bytesSinceBase * rate`：
    - `basePts` 取首帧 PTS（如 23ms）
    - `bytesSinceBase` 包含首帧自身写入的字节（对应 23ms 音频）
    - 首帧写入后时钟跳到 46ms，**首帧被计数两次**
  - 每次倍速切换后偏差累积恶化。
- **正确修复**
  - 放弃纯字节计数方案，回归 `processedUSecs` 路线。
  - 去除 `basePts`（首帧 PTS），改用 `playbackOriginMs`：
    - 播放开始 = 0
    - seek = 目标位置
    - 倍速切换 = 当前 `audioClockMs` 快照
  - `startProcessedUs` 使用 -1 惰性初始化，首次时钟更新时填入设备当前值。
  - 最终公式：`playbackOriginMs + (nowProcessedUs - startProcessedUs) * rate / 1000`。

### 12.9 回归防护清单（提交前必查）

- 涉及“丢弃策略”时，确认是否误丢 `AVPacket`（普通播放阶段必须否）。
- 涉及时间轴改动时，检查 open/seek/EOF/cleanup 四个状态迁移点是否一致。
- 涉及 EOF 循环时，确认“先播完再回环”且进度能到 `durationMs`。
- 涉及 UI 动态文本时，确认不会抬高窗口最小宽度。

---

## 13. 文件推流架构统一（Qt6 后端）

### 13.1 问题背景

原有文件推流（`PushScene::File`）通过 `StreamFFmpeg` 直接 `avformat_open_input()` 打开文件读取数据，与播放器形成两个独立的文件读取链路。当 Qt6 后端使用 FFmpeg 多媒体后端时，两个不同 FFmpeg 实例竞争同一文件句柄，Windows 下触发共享冲突（退出码 -1313558）。

### 13.2 统一方案

将文件推流改为与摄像头/屏幕相同的架构：**播放器解码 → ScreenFrameBus → 推流编码**。

```
PlayerQt6::openFile() → QMediaPlayer::setSource()
    ↓
QVideoSink::videoFrameChanged 信号
    ↓
onVideoFrameChanged → publishYuvFromQVideoFrame()
    ↓
ScreenFrameBus::publish(y, u, v, sourceId)
    ↓
pushScreenPreviewLoop → snapshot(sourceId) → 编码 → RTMP
```

### 13.3 关键实现

- **PlayerQt6 帧提取**：通过 `QVideoWidget::videoSink()` 获取 `QVideoSink*`，连接 `videoFrameChanged` 信号
- **像素格式转换**：Qt 解码输出 NV21（格式码 18），转为 YUV420P 后再发布。NV12/NV21 通过解交错 UV/VU 平面实现，其他格式通过 QImage 回退
- **推流路由**：新增 `PushInputKind::FilePreview`（kind=6），前缀 `__file_preview__:`，携带 `sourceid` 参数
- **Service 路由**：`PushScene::File` + Qt6 后端 → 自动切换为 `__file_preview__:sourceid=...` 路径
- **FFmpeg 后端**：保持原有 `remuxLoop` / `transcodeFileLoop` 直读文件路径，互不影响

### 13.4 音频处理

文件推流时的音频通过 `pushScreenPreviewLoop` 的音频采集链路获取，来源由 `audio_in` 参数指定：麦克风（默认）或 WASAPI loopback（系统音频回采）。

---

## 14. FFmpeg 日志管理

### 14.1 问题

FFmpeg 默认将 `AV_LOG_INFO` 及以上级别日志输出到 stderr。Qt6 多媒体模块若使用 FFmpeg 后端，会在 `QMediaPlayer::setSource()` 时打印 `Input #0...` 格式探测信息到控制台，绕过项目 Logger。

### 14.2 方案

在 `StreamFFmpeg` 构造函数中（始终被创建），通过 `std::call_once` 设置全局日志回调：

- `av_log_set_callback`：接管所有 FFmpeg 日志，按级别路由到 Logger（`LOG_ERROR`/`LOG_WARN`/`LOG_INFO`）
- `av_log_set_level(AV_LOG_ERROR)`：仅输出 ERROR 及以上级别，过滤 `Input #0` 等 INFO 噪音
- 回调内过滤 `”real-time buffer” + “too full”` 组合：排除 e2eSoft VCam 等虚拟设备枚举时的缓冲区告警

---

## 15. P2P 推流重试

### 15.1 问题

P2P 模式下，上一轮推流 `avio_closep` 后对端 RTMP 服务需要几秒释放流名。立即重新推流到同一流名时 `avio_open2` 返回 -138（`AVERROR_EXIT`），导致「打开推流输出地址失败」。

### 15.2 方案

在 `pushScreenPreviewLoop` 的 `avio_open2` 调用处增加重试循环：失败后 `QThread::sleep(2)` 等待对端释放，最多重试 5 次。每次重试前检查 `m_stopRequest` 确保用户可随时中断。

---

## 16. 窗口管理

- **拉流配置窗口**：独立顶层窗口（`parent=nullptr`），任务栏可见
- **拉流预览窗口**：独立顶层窗口，关闭时 `hide()` 不销毁，可通过配置窗口「预览窗口」按钮重新显示
- **系统设置 / AI 识别**：独立顶层窗口
- **托盘退出**：直接停流 + `QApplication::quit()`，不走 `closeEvent` 确认弹窗流程

---

## 17. 设计取舍说明

### 17.1 为什么保留三线程而不是单线程

- 降低音视频相互阻塞
- 提高在高码率场景下的吞吐与稳定性
- 更易实现 seek/暂停的局部控制

### 17.2 为什么普通播放阶段禁止丢视频包

- H.264/H.265 帧间预测依赖参考链
- 丢包会导致长时间宏块直到下一个关键帧
- 相比”追实时”，桌面播放器更需要”画面可用性”

### 17.3 为什么要有上传兼容兜底

- 不同平台/驱动对像素存储参数支持存在差异
- 兼容路径略慢，但可显著提高稳定性

### 17.4 为什么 Qt6 后端选择 QVideoSink 提取帧而非开第二个文件读取

- 架构统一：与摄像头/屏幕推流模式一致，单一数据源 → 多消费者
- 避免文件共享冲突：Windows 下不同 FFmpeg 实例同时打开同一文件会触发共享违例
- 零额外 I/O：解码数据已在内存中，直接转发到 ScreenFrameBus

---

## 18. 后续优化路线图

### 18.1 短期（稳定性）

- 增加“首帧保护窗口”：避免启动早期过度丢帧导致黑屏
- seek 末尾策略优化（末尾阈值与回环策略解耦）
- 细化错误码日志（解码错误、GL 错误）

### 18.2 中期（性能）

- 降低每帧 `QByteArray` 拷贝成本（复用缓冲池）
- 研究 PBO/异步上传，减少 UI 线程阻塞
- 动态队列阈值（按分辨率/fps/码率自适应）

### 18.3 长期（能力扩展）

- 引入硬解路径（如 D3D11VA）并与当前渲染链协同
- 增加 A/V 统计面板（更可观测）
- Qt6 后端增加 Linux GStreamer 管线配置

---

## 19. 开发规范建议（针对该模块）

- 修改同步逻辑必须同步更新 seek/EOF/reset 三处重置点。
- 任何“丢弃策略”必须区分“压缩包”与“解码后帧”。
- UI 文本动态变化控件必须控制宽度边界，避免布局反向影响窗口尺寸。
- 新增优化先以“稳定性回归测试”作为门禁，再看性能收益。

---

## 20. 参考文件清单

- `api/include/fplayer/api/media/iplayer.h`
- `runtime/include/fplayer/runtime/runtime.h`
- `runtime/src/runtime.cpp`
- `service/src/service.cpp`
- `widget/src/capturewindow.cpp`
- `widget/src/fvideoview.cpp`
- `common/src/fglwidget.cpp`
- `backend/media_ffmpeg/include/fplayer/backend/media_ffmpeg/playerffmpeg.h`
- `backend/media_ffmpeg/src/playerffmpeg.cpp`
- `backend/media_qt6/include/fplayer/backend/media_qt6/playerqt6.h`
- `backend/media_qt6/src/playerqt6.cpp`
- `service/src/systemsettingsrepository.cpp`（`file_playback_backend` 配置项）

---

## 21. 一句话总结

当前项目的文件播放后端设计核心是：  
**以 `IPlayer` 统一能力接口，默认使用 `PlayerQt6`（基于 QMediaPlayer + 系统媒体框架，硬件加速解码），可选 `PlayerFFmpeg`（三线程软解，OpenGL YUV 渲染）。双后端共享同一套 UI、Service、Runtime 调度层，支持在系统设置中热切换，无需重启应用。**

