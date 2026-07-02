# FPlayer-Desktop 音视频学习教程

> 面向「想通过真实项目从零学音视频」的工程师。本教程**所有代码引用都指向本仓库的真实文件与行号**,可点击跳转。
>
> 配套阅读:`doc/教程/FPlayer-Desktop-技术全景教程.html`(概念图解)、`doc/introduce/`(系统设计)、`doc/backend-media-file-playback-开发详解.md`(播放链路详解)。

---

## 目录

- [前言:这份教程怎么用](#前言这份教程怎么用)
- [第 0 章 前置知识:音视频核心概念](#第-0-章-前置知识音视频核心概念)
- [第 1 章 把项目跑起来](#第-1-章-把项目跑起来)
- [第 2 章 看懂分层架构](#第-2-章-看懂分层架构)
- [第 3 章 链路一:本地文件播放(解码链路)](#第-3-章-链路一本地文件播放解码链路)
- [第 4 章 链路二:摄像头与屏幕采集(采集链路)](#第-4-章-链路二摄像头与屏幕采集采集链路)
- [第 5 章 链路三:推流与拉流(网络链路)](#第-5-章-链路三推流与拉流网络链路)
- [第 6 章 链路四:OpenGL 渲染 YUV](#第-6-章-链路四opengl-渲染-yuv)
- [第 7 章 进阶:组合模式与音视频解耦](#第-7-章-进阶组合模式与音视频解耦)
- [第 8 章 性能优化专题](#第-8-章-性能优化专题)
- [第 9 章 学习路线与进阶方向](#第-9-章-学习路线与进阶方向)
- [附录:关键文件速查表](#附录关键文件速查表)

---

## 前言:这份教程怎么用

### 这份教程能教你什么

本仓库是一个**跨平台局域网流媒体播放系统**,完整覆盖了音视频应用的四大核心链路:

```
采集(Capture) → 编解码(Codec) → 网络(Network) → 渲染(Render)
```

跟着本教程走完,你会理解:

1. 一个视频文件是怎么变成屏幕上的画面的(解码链路);
2. 摄像头/屏幕的画面是怎么变成一帧帧数据的(采集链路);
3. 这些画面怎么在局域网里推出去、拉回来(网络链路);
4. YUV 数据怎么高效画到屏幕上(渲染链路);
5. 一个工业级 C++ 项目是怎么分层、怎么用接口隔离后端的(架构)。

### 怎么读

- **不要从第一行代码读到最后一行**。那是低效的。按「链路」切片学,每条链路都是「输入 → 处理 → 输出」的完整故事。
- **每章先看概念,再跟代码**。代码引用都带行号,建议在 IDE 里点开跟着走一遍。
- **每章末尾有动手实验**,做完再进下一章。
- **重要概念会反复出现**(如 YUV、PTS、时间基),第一次看不懂没关系,后面代码里再见时会加深理解。

### 前置技能

- C++ 基本语法(类、虚函数、智能指针、`std::thread`、lambda);
- 会用 Qt 的基本信号槽(不熟也行,教程里会点);
- 命令行能跑 CMake。
- **不需要**预先懂 FFmpeg / OpenGL / 音视频理论——那是本教程要教你的。

---

## 第 0 章 前置知识:音视频核心概念

> 这一章只讲概念,不碰代码。但每个概念后面都会标注「在本项目哪一章用到」,让你带着目标学。

### 0.1 像素、分辨率、帧率

- **像素(Pixel)**:图像最小单位。一个彩色像素通常用 RGB 三个分量表示。
- **分辨率**:宽×高,如 1920×1080。
- **帧率(FPS)**:每秒显示多少帧。电影 24fps,直播常 30/60fps。

> 📍 本项目第 4、5 章会设置 `framerate`,第 6 章按窗口宽高比缩放画面。

### 0.2 YUV 与色度子采样(★重点)

这是音视频第一个拦路虎。**视频在存储和传输时,几乎不用 RGB,而用 YUV**。

- **Y(Luma)**:亮度,即灰度图。人眼对亮度敏感。
- **U/V(Chroma)**:色度,即颜色信息。人眼对色度不敏感,所以可以**降采样**省空间。
- **YUV420P**:每 2×2 个像素共享一组 U、V(色度分辨率是亮度的 1/4)。4K RGB 是 24MB/帧,YUV420P 约 12MB/帧。这是本项目全程使用的格式。

```
YUV420P 平面存储(Planar):先存所有 Y,再存所有 U,再存所有 V
  Y 平面:width × height 字节
  U 平板:(width/2) × (height/2) 字节
  V 平板:(width/2) × (height/2) 字节
```

> 📍 本项目所有 backend 产出 YUV420P 三平面(Y/U/V 三个 `QByteArray`),第 6 章用三张纹理渲染。**记住「三平面」这个词,代码里到处都是。**

### 0.3 容器、编码、码率

容易混淆的三层,务必分清:

| 概念                        | 作用                                  | 例子                         |
| --------------------------- | ------------------------------------- | ---------------------------- |
| **容器(Container/Format)**  | 把音视频流装在一起的壳,管「怎么打包」 | mp4, mkv, flv                |
| **编码(Codec)**             | 把原始帧压缩/解压缩,管「怎么压缩」    | H.264, H.265, AAC            |
| **封装(Mux)/解封装(Demux)** | 写入/读取容器                         | `av_interleaved_write_frame` |

- 一个 `.mp4` 文件 = mp4 容器 + H.264 视频流 + AAC 音频流。
- **码率(Bitrate)**:每秒多少比特,决定画质与带宽。本项目第 5 章会设 `bit_rate`、`rc_max`。
- **GOP(Group of Pictures)**:一组帧,含 1 个 I 帧 + 若干 P/B 帧。低延迟场景 GOP 要短(甚至只有 I 帧,`max_b_frames=0`)。

### 0.4 时间戳:PTS / DTS / 时间基(★重点)

音视频同步的灵魂:

- **PTS(Presentation Timestamp)**:这一帧**该在什么时刻显示**。
- **DTS(Decoding Timestamp)**:这一帧**该在什么时刻解码**(因为 B 帧乱序,DTS≠PTS)。
- **时间基(Timebase)**:时间戳的单位。比如 `1/1000` 表示单位是毫秒。

> 📍 本项目第 3 章用 `av_rescale_q` 在不同时间基间换算 PTS,做音视频同步;第 5 章推流时设 `time_base` 保证拉流端按正确节奏播放。

### 0.5 FFmpeg 是什么

FFmpeg 是一套 C 语言音视频处理库,本项目**直接链接它的动态库**,不依赖 `ffmpeg.exe`。核心模块:

| 库              | 作用                  | 本项目用到                 |
| --------------- | --------------------- | -------------------------- |
| `libavformat`   | 容器读写(解封装/封装) | 打开文件、推拉流           |
| `libavcodec`    | 编解码                | 解码 H.264、编码 H.264/AAC |
| `libswscale`    | 图像缩放/色彩转换     | BGRA→YUV420P、YUV→YUV420P  |
| `libswresample` | 音频重采样            | 音频格式转换               |
| `libavutil`     | 工具                  | 时间换算、内存             |

> 📍 第 3 章会看到 `avformat_open_input`、`avcodec_send_packet`、`avcodec_receive_frame` 这组黄金调用序列。

### 0.6 推流 / 拉流 / 协议

- **推流(Push)**:把音视频流发送到服务器/对端。
- **拉流(Pull)**:从服务器/对端接收音视频流。
- **协议**:本项目局域网用 **RTMP**(推/拉,FLV 封装)、**RTSP**、**SRT**。

> 📍 第 5 章详述。

### 概念检查

能用自己的话回答以下问题再进入下一章:

1. YUV420P 为什么比 RGB 省?U/V 平面尺寸为什么是 Y 的 1/4?
2. 容器和编码是什么关系?一个 mkv 文件里能有 H.264 视频流吗?
3. PTS 和 DTS 为什么不相等?
4. FFmpeg 的 `libavformat` 和 `libavcodec` 分别管什么?

---

## 第 1 章 把项目跑起来

> 目标:编译运行,本地播放一个视频。先有体感,再读代码。

### 1.1 环境准备

见 [README.md](../../README.md)「编译环境要求」章节。需要:

- Visual Studio 2022(MSVC v143)
- CMake 3.24+
- Qt 6.10.2
- FFmpeg(项目自带或 `3rd/` 目录)

### 1.2 CMake 配置与构建

关键开关(见 [README.md:138-151](../../README.md#L138)):

```cmake
-DCMAKE_PREFIX_PATH=D:/SoftWare/Qt/Qt6.10.2/6.10.2/msvc2022_64
-DQt6_DIR=D:/SoftWare/Qt/Qt6.10.2/6.10.2/msvc2022_64/lib/cmake/Qt6
-DFPLAYER_BUILD_MEDIA_FFMPEG:BOOL=ON   # 启用 FFmpeg 后端(必开,否则没法学编解码)
-DFPLAYER_BUILD_STREAM_FFMPEG:BOOL=ON  # 启用推拉流后端
```

> ⚠️ 学习音视频**必须开 `FPLAYER_BUILD_MEDIA_FFMPEG`**,因为 Qt6 后端只是封装了 `QMediaPlayer`,看不到 FFmpeg 细节。

### 1.3 运行并播放本地文件

1. 启动后进入主窗口;
2. 切到「文件」模式,选择一个本地视频;
3. 确认设置里「文件播放后端」是 **FFmpeg**(不是 Qt6);
4. 播放,看到画面、听到声音。

✅ **实验 1.1**:用 Qt6 后端和 FFmpeg 后端各播放一次,观察画质/性能差异。差异来自哪?带着这个问题进第 3 章。

---

## 第 2 章 看懂分层架构

> 目标:理解项目的「七层架构」和「接口+工厂」设计模式,建立全局地图。后面每条链路都会在这张地图上走。

### 2.1 七层架构

项目用 CMake 组织成多个模块,依赖关系如下(自上而下):

```
app        程序入口,解析命令行参数
 ↓
widget     UI 层,Qt 界面与交互
 ↓
service    服务层,业务编排(场景路由)
 ↓
runtime    运行时层,工厂:按类型创建 backend
 ↓
backend    后端层,真正的音视频实现(qt6 / ffmpeg / dxgi)
 ↓
api        抽象层,纯虚接口 + 枚举(被所有层依赖)
 ↓
common     公共层,共享组件(FGLWidget、帧总线)
```

> 注意:虽然画成线性,但 `api` 是**接口定义层**,被 widget/service/runtime/backend 共同依赖;`common` 是跨层共享。真正的依赖规则见 [README.md:209-219](../../README.md#L209)。

### 2.2 接口层:抽象是关键

`api` 层只定义「能做什么」,不说「怎么做」。看几个核心接口:

- [IPlayer](../../api/include/fplayer/api/media/iplayer.h) — 媒体播放(`openFile/play/pause/stop/seekMs/...`),[iplayer.h:22](../../api/include/fplayer/api/media/iplayer.h#L22)
- [ICamera](../../api/include/fplayer/api/media/icamera.h) — 摄像头采集,[icamera.h:28](../../api/include/fplayer/api/media/icamera.h#L28)
- [IScreenCapture](../../api/include/fplayer/api/media/iscreencapture.h) — 屏幕采集,[iscreencapture.h:27](../../api/include/fplayer/api/media/iscreencapture.h#L27)
- [IStream](../../api/include/fplayer/api/net/istream.h) — 推拉流,[istream.h:15](../../api/include/fplayer/api/net/istream.h#L15)
- [IFVideoView](../../api/include/fplayer/api/media/ifvideoview.h) — 视频视图,[ifvideoview.h:16](../../api/include/fplayer/api/media/ifvideoview.h#L16)

**思考**:为什么要把接口单独放一层,而不放在 backend 里?
**答**:解耦。widget/service 只依赖 `IPlayer`,不认识 `PlayerFFmpeg`。这样后端可以从 Qt6 换成 FFmpeg 而上层一行代码不改。

### 2.3 后端类型枚举

[mediabackendtype.h:13](../../api/include/fplayer/api/media/mediabackendtype.h#L13):

```cpp
enum class MediaBackendType { Qt6, FFmpeg, Dxgi };
```

> ⚠️ 准确性提示:README 提到 WebRTC,但**WebRTC 后端尚未实现**(`FPLAYER_BUILD_RTC_WEBRTC` 默认 OFF,无源码)。当前只有 Qt6 / FFmpeg / DXGI 三套后端,其中 DXGI 仅用于 Windows 屏幕采集。

### 2.4 工厂模式:runtime 层

`runtime` 是唯一「认识所有 backend」的层。看工厂如何按类型创建:

[runtime.cpp:36-53](../../runtime/src/runtime.cpp#L36) `createPlayer`:

```cpp
std::shared_ptr<IPlayer> RunTime::createPlayer(MediaBackendType type) {
    switch (type) {
        case MediaBackendType::Qt6:    m_player = std::make_shared<PlayerQt6>();    break;
        case MediaBackendType::FFmpeg: m_player = std::make_shared<PlayerFFmpeg>(); break;
        ...
    }
    return m_player;
}
```

类似的还有 `createCamera`、`createScreenCapture`、`createStream`,见 [runtime.cpp:17-99](../../runtime/src/runtime.cpp#L17)。DXGI 屏幕后端不可用时还会**回退到 FFmpeg**,见 [runtime.cpp:73-74](../../runtime/src/runtime.cpp#L73)。

### 2.5 业务编排:service 层

`service` 是 UI 与后端之间的门面,负责场景路由。比如推流时根据场景拼参数:

[service.cpp:389](../../service/src/service.cpp#L389) `streamStartPushByScene` —— `PushScene::Screen` 时拼 `__screen_preview__:...` URL(见 [service.cpp:419-457](../../service/src/service.cpp#L419))。

### 2.6 从入口到后端的完整路径

以「启动时初始化播放器」为例,串起所有层:

| 步  | 层      | 文件:行                                                            | 调用                                     |
| --- | ------- | ------------------------------------------------------------------ | ---------------------------------------- |
| 1   | app     | [main.cpp:84](../../app/main.cpp#L84)                              | `main()` 解析 `--backend` 参数           |
| 2   | app     | [main.cpp:124](../../app/main.cpp#L124)                            | `new CaptureWindow(backendType)`         |
| 3   | widget  | [capturewindow.cpp:1517](../../widget/src/capturewindow.cpp#L1517) | `m_service->initPlayer(backend)`         |
| 4   | service | [service.cpp:46](../../service/src/service.cpp#L46)                | `Service::initPlayer()`                  |
| 5   | service | [service.cpp:119](../../service/src/service.cpp#L119)              | `m_runtime->createPlayer(backend)`       |
| 6   | runtime | [runtime.cpp:36](../../runtime/src/runtime.cpp#L36)                | `createPlayer()` switch → `PlayerFFmpeg` |

✅ **实验 2.1**:在 [main.cpp:84](../../app/main.cpp#L84) 打断点,启动调试,单步跟到 `PlayerFFmpeg` 构造。这条路径走通了,你就理解了整个分层。

---

## 第 3 章 链路一:本地文件播放(解码链路)

> 目标:搞懂「一个 mp4 文件怎么变成屏幕上的画面」。这是音视频入门最经典的链路,也是本教程的核心。

### 3.1 全景图

```
mp4 文件
  │  avformat_open_input       (打开文件,解封装容器)
  │  avformat_find_stream_info  (探测流信息)
  ▼
AVFormatContext (含若干流:视频流、音频流)
  │  avcodec_open2              (打开解码器)
  ▼
解复用线程: av_read_frame         (从容器读出一个 packet)
  │  按 stream_index 分发
  ▼
videoPackets 队列  ◄──背压(上限120)──►  audioPackets 队列
  │                                        │
视频解码线程                               音频解码线程
avcodec_send_packet                        avcodec_send_packet
avcodec_receive_frame                      avcodec_receive_frame
  │ sws_scale (→YUV420P)                   │ swr (重采样)
  ▼                                        ▼
queuePreviewYuv() ───跨线程──► deliverPreviewFrame() (主线程)
                                  │
                                  ▼
                          FGLWidget::updateYUVFrame()  (第6章)
```

核心文件:[playerffmpeg.cpp](../../backend/media_ffmpeg/src/playerffmpeg.cpp)(约 1264 行)。

### 3.2 打开文件与解封装

[playerffmpeg.cpp:229](../../backend/media_ffmpeg/src/playerffmpeg.cpp#L229) `openFile()`:

```cpp
avformat_open_input(&formatContext, path, nullptr, nullptr);   // :238 打开文件
avformat_find_stream_info(formatContext, nullptr);             // :246 探测流信息
// :265 打开视频解码器(必需)
openCodecContext(..., AVMEDIA_TYPE_VIDEO, ...);
// :271 打开音频解码器(可选)
openCodecContext(..., AVMEDIA_TYPE_AUDIO, ...);
```

辅助函数 [openCodecContext()](../../backend/media_ffmpeg/src/playerffmpeg.cpp#L1221) 做了三件事:

1. `av_find_best_stream` —— 在多个流里选出视频流;
2. `avcodec_find_decoder` —— 按 `codec_id` 找解码器(如 H.264);
3. `avcodec_alloc_context3` / `avcodec_parameters_to_context` / `avcodec_open2` —— 打开解码器。

> 🧠 **理解点**:`avformat_*` 管「容器」,`avcodec_*` 管「编码」。打开一个文件 = 打开容器 + 为每条流打开解码器。

### 3.3 三线程模型(★重点)

`play()` ([playerffmpeg.cpp:289](../../backend/media_ffmpeg/src/playerffmpeg.cpp#L289)) 启动三个 `std::thread`:

| 线程             | 职责                       | 关键 API                                                                                                             |
| ---------------- | -------------------------- | -------------------------------------------------------------------------------------------------------------------- |
| **demuxThread**  | 从容器读 packet,分发到队列 | `av_read_frame` ([playerffmpeg.cpp:349](../../backend/media_ffmpeg/src/playerffmpeg.cpp#L349))                       |
| **decodeThread** | 视频解码 + 同步 + 投递预览 | `avcodec_send_packet`/`receive_frame` ([playerffmpeg.cpp:519](../../backend/media_ffmpeg/src/playerffmpeg.cpp#L519)) |
| **audioThread**  | 音频解码 + 重采样 + 输出   | `swr` + `QAudioSink`                                                                                                 |

**为什么三线程?** 因为解复用、视频解码、音频解码速度不同,用**有界队列**解耦:

- `videoPackets` 上限 120,`audioPackets` 上限 240;
- 队列满时用 `condition_variable` 阻塞生产者(**背压**),避免内存爆掉;
- 正常不丢视频包(防止参考帧断裂),seek 期间才丢旧包。

> 🧠 **参考帧断裂**:H.264 的 P/B 帧依赖前面的 I/P 帧,丢了中间包会导致花屏。所以视频包队列宁肯阻塞也不丢。

### 3.4 解码黄金序列

视频解码线程核心 ([playerffmpeg.cpp:519](../../backend/media_ffmpeg/src/playerffmpeg.cpp#L519)):

```cpp
avcodec_send_packet(videoCodecContext, packet);   // 送入一个压缩包
avcodec_receive_frame(videoCodecContext, frame);  // 取出一个解码帧(可能需要送多次才出一帧)
```

这是 FFmpeg 新版 API 的「发送-接收」模型。注意:

- `send_packet` 和 `receive_frame` **不一定一一对应**:送一个包可能不出帧(B 帧重排序),也可能送包后能出多帧;
- 解码结束时要 send null packet flush 出缓冲帧。

### 3.5 色彩转换:sws_scale

解码出来的帧未必是 YUV420P,需要统一:

```cpp
// playerffmpeg.cpp:543-560
sws_getContext(..., AV_PIX_FMT_YUV420P, SWS_FAST_BILINEAR, ...);
sws_scale(swsCtx, src, srcStride, 0, height, dst, dstStride);
```

`sws_scale` 同时做两件事:**色彩空间转换**(任意格式→YUV420P)+ **缩放**。本项目所有渲染和推流都假定 YUV420P。

### 3.6 音视频同步(★难点)

解码出一帧后,什么时候送去显示?这就是**音视频同步**。本项目策略 ([playerffmpeg.cpp:585](../../backend/media_ffmpeg/src/playerffmpeg.cpp#L585)):

- **有音频**:对齐音频时钟 `audioClockMs`(音频是主时钟,因为人耳对卡顿更敏感);
- **无音频**:对齐 `av_gettime_relative()` 单调系统时钟;
- **drift 过大**:睡眠等下一帧,或丢帧追上。

时间戳换算用 [framePtsMs()](../../backend/media_ffmpeg/src/playerffmpeg.cpp#L1251):

```cpp
// best_effort_timestamp 是 FFmpeg 推荐的显示时间戳
// av_rescale_q 在不同时间基之间换算
pts_ms = av_rescale_q(frame->best_effort_timestamp - base, stream->time_base, {1, 1000});
```

> 🧠 **为什么用音频做主时钟?** 音频一旦卡顿,人会立刻察觉;视频略有抖动反而不易察觉。所以让视频追音频。

### 3.7 跨线程投递预览帧

解码线程不能直接调 UI(Qt 禁止非主线程操作 UI),所以用 `QueuedConnection` 跨线程投递:

[playerffmpeg.cpp:1026](../../backend/media_ffmpeg/src/playerffmpeg.cpp#L1026) `queuePreviewYuv()`:

```cpp
// 锁存到 previewPendingY/U/V
// QMetaObject::invokeMethod(this, "deliverPreviewFrame", Qt::QueuedConnection)
```

[playerffmpeg.cpp:1055](../../backend/media_ffmpeg/src/playerffmpeg.cpp#L1055) `deliverPreviewFrame()` 在**主线程**执行,调用 `m_impl->glWidget->updateYUVFrame(...)`。

> 🧠 **QueuedConnection**:把调用「打包」放到目标线程(主线程)的事件队列里,等主线程空闲时执行。这是 Qt 跨线程通信的标准手段。

### 3.8 完整调用链速查

```
CaptureWindow::chooseAndPlayFile()     capturewindow.cpp:4732
  → Service::openMediaFile()           service.cpp:119
    → IPlayer::openFile()              playerffmpeg.cpp:229
      → play()                          playerffmpeg.cpp:289
        → demuxThread: av_read_frame    playerffmpeg.cpp:349
        → decodeThread: send/receive    playerffmpeg.cpp:519
          → sws_scale → YUV420P         playerffmpeg.cpp:543
          → 音视频同步                   playerffmpeg.cpp:585
          → queuePreviewYuv()           playerffmpeg.cpp:1026
            → deliverPreviewFrame()     playerffmpeg.cpp:1055  (主线程)
              → FGLWidget::updateYUVFrame()  fglwidget.cpp:349  (第6章)
```

> 📖 深入阅读:[doc/backend-media-file-playback-开发详解.md](../backend-media-file-playback-开发详解.md)

✅ **实验 3.1**:在 [playerffmpeg.cpp:532](../../backend/media_ffmpeg/src/playerffmpeg.cpp#L532) `avcodec_receive_frame` 处加日志,打印每帧的 `pts_ms` 和格式。播放一个视频,观察 PTS 是否单调递增。

✅ **实验 3.2**:故意把 `videoPackets` 上限改成 1,观察会发生什么(提示:背压)。

---

## 第 4 章 链路二:摄像头与屏幕采集(采集链路)

> 目标:搞懂「画面从哪来」——摄像头和屏幕怎么变成 YUV 帧。

### 4.1 摄像头采集:CameraFFmpeg

核心文件:[cameraffmpeg.cpp](../../backend/media_ffmpeg/src/cameraffmpeg.cpp)。

**设备枚举**(不走 FFmpeg,走平台 DirectShow):

[camerainfofetcher.cpp:99](../../backend/media_ffmpeg/src/camerainfofetcher.cpp#L99) `getDescriptions()` 带 30 秒缓存(枚举 dshow 设备耗时 1-5 秒)。Windows 用 COM API:

```cpp
CoCreateInstance(CLSID_SystemDeviceEnum, ...);
CreateClassEnumerator(CLSID_VideoInputDeviceCategory, ...);  // camerainfofetcher.cpp:137
// 遍历 IEnumMoniker,读 FriendlyName、DisplayName
```

> 🧠 **为什么不用 FFmpeg 枚举?** 注释说:FFmpeg 擅长打开解码,不擅长完整能力枚举(分辨率/帧率组合)。所以枚举走平台 API,打开走 FFmpeg。

**打开设备并采集**:

[cameraffmpeg.cpp:232](../../backend/media_ffmpeg/src/cameraffmpeg.cpp#L232) `selectCamera()`:

```cpp
// 构造 dshow 输入路径 "video=<name>"
avformat_open_input(...);            // cameraffmpeg.cpp:311
avformat_find_stream_info(...);      // :343
avcodec_open2(...);                  // :373-404
QThread::create(captureLoop);        // :409 启动采集线程
```

[cameraffmpeg.cpp:490](../../backend/media_ffmpeg/src/cameraffmpeg.cpp#L490) `captureLoop()`:

```cpp
av_read_frame(...);                  // :506  从摄像头读包
avcodec_send_packet(...);            // :523
avcodec_receive_frame(...);          // :532
// 非YUV420P 则 sws_scale 转换       :564-600
// 深拷贝 Y/U/V 三平面到 QByteArray    :621-623
emit yuvFrameReady(...);             // :633 给 FGLWidget 预览
CameraFrameBus::publish(...);        // :643 供推流消费
```

注意这里和第 3 章解码链路的相似性:`av_read_frame → send_packet → receive_frame`。**采集和文件播放共用同一套 FFmpeg 解码序列**,区别只是输入源是摄像头而非文件。

### 4.2 屏幕采集:DXGI Desktop Duplication

核心文件:[screencapturedxgi.cpp](../../backend/desktopcapture_dxgi/src/screencapturedxgi.cpp)(Windows 专用,812 行)。

DXGI Desktop Duplication 是 Windows 上高效的屏幕采集方式,直接从 GPU 复制桌面纹理,比老的 gdigrab 快得多。

**枚举屏幕**(用 DXGI 原生坐标,避免 Qt+DPR 的 1px 偏差):

[screencapturedxgi.cpp:95](../../backend/desktopcapture_dxgi/src/screencapturedxgi.cpp#L95) `refreshScreens()`:

```cpp
CreateDXGIFactory1(...);
// 遍历 IDXGIAdapter1 / IDXGIOutput,用 GetDesc 的 DesktopCoordinates
```

**打开 Duplication**:

[screencapturedxgi.cpp:345](../../backend/desktopcapture_dxgi/src/screencapturedxgi.cpp#L345) `openDuplicationForScreenIndex()`:

```cpp
D3D11CreateDevice(..., D3D11_CREATE_DEVICE_BGRA_SUPPORT, ...);  // :413
IDXGIOutput1::DuplicateOutput(...);   // :421 得到 IDXGIOutputDuplication
```

**采集一帧**:

[screencapturedxgi.cpp:547](../../backend/desktopcapture_dxgi/src/screencapturedxgi.cpp#L547) `captureOneFrame()`:

```cpp
AcquireNextFrame(100, ...);           // :558  从 GPU 取帧
// DXGI_ERROR_WAIT_TIMEOUT 视为画面静止(非故障)
CopyResource(...);                    // 复制到 staging texture
Map(...);                             // :647 读到 m_bgraFrame (BGRA)
drawCursorOnBgra(...);                // :489 叠鼠标(GetCursorInfo + DrawIconEx)
sws_scale(BGRA → YUV420P);            // :755 关键转换
dispatchFrameToView(...);             // :758 投递预览
ScreenFrameBus::publish(...);         // :759 供推流
```

> 🧠 **BGRA vs YUV**:屏幕采集拿到的是 **BGRA**(GPU 友好),但渲染和推流要 **YUV420P**。`sws_scale` 负责转换。这是采集链路里最贵的一步,第 8 章会优化它。

> 🧠 **HDR 处理**:`R16G16B16A16_FLOAT`(HDR 浮点帧)走「跳帧」分支([screencapturedxgi.cpp:585](../../backend/desktopcapture_dxgi/src/screencapturedxgi.cpp#L585)),不重建设备,日志限频。

### 4.3 帧总线(FrameBus):解耦采集与推流

这是本项目的精髓设计。采集端把帧 `publish` 到总线,推流端从总线 `snapshot`,两者**互不认识**:

- [ScreenFrameBus](../../common/include/fplayer/common/screenframebus/screenframebus.h) —— 屏幕帧总线
- [CameraFrameBus](../../common/src/cameraframebus.cpp) —— 摄像头帧总线

总线按 `sourceId` 分通道(`QHash<QString, Channel>`,见 [screenframebus.h:52](../../common/include/fplayer/common/screenframebus/screenframebus.h#L52)),组合模式每路摄像头/屏分配独立 sourceId,避免抢帧闪烁。

> 🧠 **为什么要总线?** 因为同一帧要给「预览」和「推流」两个消费者用。如果直接调用,采集端要认识所有消费者,耦合。总线是「发布-订阅」模式,采集端只管发布,谁要谁订阅。

### 4.4 调用链速查

```
摄像头: CameraFFmpeg::selectCamera()  cameraffmpeg.cpp:232
  → captureLoop()                       cameraffmpeg.cpp:490
    → av_read_frame/send/receive
    → sws_scale → YUV420P
    → emit yuvFrameReady (预览)
    → CameraFrameBus::publish (推流)

屏幕:   ScreenCaptureDxgi::setActive() screencapturedxgi.cpp:215
  → captureLoop()                       screencapturedxgi.cpp:764
    → captureOneFrame()                 screencapturedxgi.cpp:547
      → AcquireNextFrame → BGRA → sws_scale → YUV420P
      → dispatchFrameToView (预览)
      → ScreenFrameBus::publish (推流)
```

✅ **实验 4.1**:在 [screencapturedxgi.cpp:755](../../backend/desktopcapture_dxgi/src/screencapturedxgi.cpp#L755) `sws_scale` 前后各打印一次时间戳,测一次 BGRA→YUV 转换耗时。这就是第 8 章 P1 优化要消灭的开销。

✅ **实验 4.2**:切换设置里的屏幕后端为 FFmpeg(gdigrab 回退),对比 DXGI 的 CPU 占用。

---

## 第 5 章 链路三:推流与拉流(网络链路)

> 目标:搞懂画面怎么在局域网里流动。这是流媒体的核心。

核心文件:[streamffmpeg.cpp](../../backend/stream_ffmpeg/src/streamffmpeg.cpp)(7658 行,项目最大文件)。

### 5.1 推流全景

```
采集端 (Camera/Screen/File)
  │ publish YUV 帧
  ▼
FrameBus (按 sourceId 分通道)
  │ snapshotIfNew (只取新帧)
  ▼
推流线程 pushXxxLoop()
  │ sws_scale (调整到编码尺寸)
  │ avcodec_send_frame / receive_packet  (编码 H.264)
  │ av_interleaved_write_frame           (封装 FLV,写网络)
  ▼
RTMP/RTSP 服务器 或 对端
```

### 5.2 场景路由:URL 即指令

本项目用一个巧妙设计:**用特殊 URL 前缀表示推流场景**。Service 层根据场景拼 URL,StreamFFmpeg 解析 URL 决定走哪个推流循环。

[streamffmpeg.cpp:130](../../backend/stream_ffmpeg/src/streamffmpeg.cpp#L130) `startPush()` → `parsePushInputRoute()` 识别 `PushInputKind`:

| URL 前缀             | 含义                  | 推流循环                                                                                                  |
| -------------------- | --------------------- | --------------------------------------------------------------------------------------------------------- |
| `__compose_scene__`  | 组合模式              | `pushComposeSceneLoop` ([streamffmpeg.cpp:1310](../../backend/stream_ffmpeg/src/streamffmpeg.cpp#L1310))  |
| `__screen_preview__` | 屏幕预览帧推流(DXGI)  | `pushScreenPreviewLoop` ([streamffmpeg.cpp:4143](../../backend/stream_ffmpeg/src/streamffmpeg.cpp#L4143)) |
| `__screen_capture__` | 屏幕 gdigrab 采集推流 | `pushScreenLoop` ([streamffmpeg.cpp:3000](../../backend/stream_ffmpeg/src/streamffmpeg.cpp#L3000))        |
| `__camera_capture__` | 摄像头采集推流        | `pushCameraLoop` ([streamffmpeg.cpp:6656](../../backend/stream_ffmpeg/src/streamffmpeg.cpp#L6656))        |
| `__file_transcode__` | 文件转码推流          | `transcodeFileLoop` ([streamffmpeg.cpp:2632](../../backend/stream_ffmpeg/src/streamffmpeg.cpp#L2632))     |

> 🧠 **为什么用预览帧推流?** DXGI 采集时,预览和推流可以用同一帧(都从 `ScreenFrameBus` 取),避免采集两次。所以屏幕推流走 `__screen_preview__` 复用预览帧。而 gdigrab 后端没有预览帧总线,只能走 `__screen_capture__` 重新采集。

### 5.3 编码器选择与低延迟调优

[streamffmpeg_helpers.h](../../backend/stream_ffmpeg/src/streamffmpeg_helpers.h) `pickVideoEncoderCandidates()` 自动选择顺序:

```
h264_nvenc (NVIDIA 硬编) → h264_amf (AMD 硬编) → libx264 (软编)
```

不同编码器设不同低延迟参数([streamffmpeg.cpp:4474](../../backend/stream_ffmpeg/src/streamffmpeg.cpp#L4474) 起):

| 编码器             | 低延迟参数                                |
| ------------------ | ----------------------------------------- |
| h264_nvenc         | `preset=p1, tune=ll, rc=cbr, zerolatency` |
| h264_amf / h264_mf | `usage=ultralowlatency`                   |
| libx264            | `superfast, zerolatency`                  |

通用低延迟设置:

```cpp
encCtx->gop_size = ...;
encCtx->max_b_frames = 0;     // 不要 B 帧,降延迟
encCtx->bit_rate = ...;
// ofmt->flags |= AVFMT_FLAG_FLUSH_PACKETS;     // 立即发包
// max_interleave_delta = 0;                      // 不交织缓冲
```

> 🧠 **为什么 `max_b_frames=0`?** B 帧需要「未来帧」才能编码,引入延迟。低延迟场景宁可码率高一点也不用 B 帧。

### 5.4 编码黄金序列

推流核心编码循环([streamffmpeg.cpp:2442](../../backend/stream_ffmpeg/src/streamffmpeg.cpp#L2442)):

```cpp
av_frame_get_buffer(encFrame, 0);              // :4805 分配帧缓冲
sws_scale(...);                                // :5417 YUV 调整到编码尺寸
avcodec_send_frame(encCtx, encFrame);          // :2442 送入原始帧
avcodec_receive_packet(encCtx, outPkt);        // :2454 取出压缩包
av_interleaved_write_frame(ofmt, outPkt);      // :2461 封装并写网络
```

对比第 3 章解码序列(`send_packet/receive_frame`),编码是反过来的:`send_frame/receive_packet`。**编码和解码是镜像的**。

### 5.5 拉流:解码 + 总线预览

拉流复用 `remuxLoop`([streamffmpeg.cpp:448](../../backend/stream_ffmpeg/src/streamffmpeg.cpp#L448)):

```
对端
  │ av_read_frame (从网络读 packet)
  ▼
视频解码预览: avcodec_send_packet/receive_frame  streamffmpeg.cpp:956/958
  │ sws_scale → YUV420P
  ▼
ScreenFrameBus::publish("pull_preview")          streamffmpeg.cpp:1004
  │
  ▼
预览窗口 snapshotIfNew("pull_preview")            capturewindow.cpp:3177
  │
  ▼
FGLWidget 显示
```

> 🧠 **为什么拉流要解码?** 不能直接显示,因为网络传的是压缩的 H.264,必须解码成 YUV 才能渲染。同时项目把解码后的帧发布到 `pull_preview` 总线,预览窗口订阅显示。

**音频预览**:解码后 `swresample` 重采样,`QAudioSink` 输出。

**录制**:拉流过程中动态开 matroska 输出(`openPullRecordOutput`,[streamffmpeg.cpp:579](../../backend/stream_ffmpeg/src/streamffmpeg.cpp#L579)),同线程内 `av_interleaved_write_frame` 写录制文件。

### 5.6 监视窗口 ↔ 预览窗口联动

[widget/src/capturewindow.cpp](../../widget/src/capturewindow.cpp):

- 拉流监视窗口([capturewindow.cpp:2744](../../widget/src/capturewindow.cpp#L2744)),`streamStartPull(candidate, QString())`([capturewindow.cpp:2969](../../widget/src/capturewindow.cpp#L2969)) 默认输出 `null`(纯预览模式);
- 预览窗口绑定 `pull_preview` 总线([capturewindow.cpp:3177](../../widget/src/capturewindow.cpp#L3177));
- 录制在预览窗口触发 `streamStartPullRecording`([capturewindow.cpp:3305](../../widget/src/capturewindow.cpp#L3305))。

> 📖 深入阅读:[doc/推拉流-开发文档.md](../推拉流-开发文档.md)、[doc/推拉流-使用说明.md](../推拉流-使用说明.md)

✅ **实验 5.1**:在本机起一个 RTMP 服务器(如用 nginx-rtmp 或本项目配套的 `fplayer-ff-service`),用 desktop 推流,用另一个 desktop 拉流,测量端到端延迟。

✅ **实验 5.2**:对比 `h264_nvenc` 和 `libx264` 的 CPU 占用和延迟差异。

---

## 第 6 章 链路四:OpenGL 渲染 YUV

> 目标:搞懂 YUV 三平面怎么高效画到屏幕上。这是性能工程的核心。

核心文件:[fglwidget.cpp](../../common/src/fglwidget.cpp) + [fglwidget.h](../../common/include/fplayer/common/fglwidget/fglwidget.h)。类 `FGLWidget`,继承 `QOpenGLWidget`。

### 6.1 渲染全景

```
updateYUVFrame(y,u,v,w,h,stride)    fglwidget.cpp:349  (主线程被调用)
  │ 锁存到 m_yuvData + update()
  ▼
paintGL()                            fglwidget.cpp:167  (GPU 渲染)
  │ 快照 m_yuvData
  │ updateYUVTextures()              fglwidget.cpp:257
  │   uploadViaPBO() × 3             fglwidget.cpp:224  (三平面上传纹理)
  ▼
绑定 Y/U/V 三张纹理 + shader
  ▼
glDrawArrays(GL_TRIANGLE_FAN, 0, 4)  画一个四边形
  ▼
片段着色器: YUV → RGB (BT.601/BT.709)
```

### 6.2 三纹理 + 着色器

YUV420P 的三个平面分别上传成三张**单通道纹理**(`GL_RED`):

```cpp
// fglwidget.cpp:257 updateYUVTextures
// texY: width × height,单通道
// texU: (width/2) × (height/2),单通道
// texV: (width/2) × (height/2),单通道
```

片段着色器([fglwidget.cpp:62-122](../../common/src/fglwidget.cpp#L62))做 YUV→RGB 转换:

```glsl
// 采样 Y/U/V
float y = texture(texY, vTexCoord).r;
float u = texture(texU, vTexCoord).r - 0.5;  // 中心化
float v = texture(texV, vTexCoord).r - 0.5;
// limited range → full range
y = (y - 16.0/255.0) * (255.0/224.0);
// colorMatrix 选 BT.601 或 BT.709
vec3 rgb = uColorMatrix * vec3(y, u, v);
gl_FragColor = vec4(rgb, 1.0);
```

> 🧠 **为什么用着色器转 YUV→RGB?** 因为 GPU 并行计算,每个像素独立转换,比 CPU `sws_scale` 快几个数量级。

> 🧠 **BT.601 vs BT.709**:不同的 YUV→RGB 转换矩阵,SD 用 BT.601,HD 用 BT.709。`uColorMatrix` uniform 切换。

### 6.3 PBO 双缓冲:异步纹理上传(★优化)

`glTexSubImage2D` 是**同步阻塞**的:CPU 把数据拷到 GPU 时, CPU 要等。PBO(Pixel Buffer Object)把这一步异步化。

[fglwidget.cpp:224](../../common/src/fglwidget.cpp#L224) `uploadViaPBO()`:

```cpp
glGenBuffers(2, pbo);                    // 生成两个 PBO
glBufferData(GL_PIXEL_UNPACK_BUFFER, bytes, nullptr, GL_STREAM_DRAW);
pboIdx = (pboIdx + 1) & 1;               // 轮换到另一个 PBO
glMapBufferRange(..., GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
memcpy(mapped, data, bytes);             // 写入 PBO(CPU 端)
glUnmapBuffer(...);
glTexSubImage2D(..., nullptr);           // 从 PBO 上传纹理(GPU 端,异步)
```

**双缓冲**的意思:用 PBO A 上传第 N 帧时,CPU 同时往 PBO B 写第 N+1 帧。两块交替使用,GPU 上传和 CPU 写入并行。

声明见 [fglwidget.h:62-68](../../common/include/fplayer/common/fglwidget/fglwidget.h#L62):`m_pboY[2]/m_pboU[2]/m_pboV[2]`。

### 6.4 Shader 位置缓存

`glGetUniformLocation` 每帧调用有驱动开销。本项目在 shader link 后**一次性查询并缓存**:

[fglwidget.cpp:418](../../common/src/fglwidget.cpp#L418) `setupShaders()`:

```cpp
m_posLocation = attributeLocation("position");
m_texYLocation = uniformLocation("texY");
m_colorMatrixLocation = uniformLocation("uColorMatrix");
// ... 共 7 个 location 缓存到成员变量
```

`paintGL` 直接用缓存值,见 [fglwidget.h:71-77](../../common/include/fplayer/common/fglwidget/fglwidget.h#L71)。

### 6.5 隐式共享(COW):减少帧拷贝

帧数据在采集线程→UI 线程之间传递,如果深拷贝每帧 36MB(YUV420P 1080p),开销巨大。本项目利用 **QByteArray 隐式共享(Copy-On-Write)**:

[screencapturedxgi.cpp:277](../../backend/desktopcapture_dxgi/src/screencapturedxgi.cpp#L277):

```cpp
// COW 隐式共享:浅拷贝仅增加引用计数;
// 下一帧 resize() 时 Qt 自动 detach,旧缓冲区保持有效。
const QByteArray yCopy = yData;   // 浅拷贝,只增加引用计数
const QByteArray uCopy = uData;
const QByteArray vCopy = vData;
// lambda 捕获后 QueuedConnection 跨线程投递
```

> ⚠️ **准确性提示**:这其实是「隐式共享」而非真零拷贝。性能文档 P2 指出:跨线程场景下 `resize` 会触发 detach(深拷贝),一帧仍被复制 2-3 次。**真正的零拷贝(ring buffer + `shared_ptr<uint8_t[]>`)是计划中的优化**,见第 8 章。

### 6.6 stride 兼容(Windows 稳定性坑)

图像每行字节数(stride)可能大于 `width`(对齐)。`GL_UNPACK_ROW_LENGTH` 本可告诉 GPU 跳过填充,但**Windows 部分驱动实现不稳定**,会导致行错位/扭曲。

[fglwidget.cpp:35](../../common/src/fglwidget.cpp#L35) `canUseUnpackRowLength()` 在 Windows 上**硬编码返回 false**,统一走逐行 `memcpy` 重打包(`repackPlaneTightBuf`)。非 Windows 按扩展判断。

> 🧠 **教训**:GPU API 在不同平台/驱动上行为不一致,生产代码要有兜底路径。这是初学者光看教程学不到的工程经验。

### 6.7 contain 等比缩放

[fglwidget.cpp:449](../../common/src/fglwidget.cpp#L449) `calculateVertices()`:比较窗口与图像宽高比,取较小缩放系数,留黑边(不拉伸),生成 4 个顶点用 `GL_TRIANGLE_FAN` 绘制。

> 📖 深入阅读:[doc/introduce/opengl-usage.md](../introduce/opengl-usage.md)、[doc/introduce/图像渲染与实时合成技术.md](../introduce/图像渲染与实时合成技术.md)

✅ **实验 6.1**:在 [fglwidget.cpp:167](../../common/src/fglwidget.cpp#L167) `paintGL` 里统计每秒帧率,对比开关 PBO(`uploadViaPBO` vs 直接 `glTexSubImage2D`)的差异。

---

## 第 7 章 进阶:组合模式与音视频解耦

> 目标:理解多源合成与音视频并行,这是进阶内容。

### 7.1 组合模式:多源画布合成

组合模式把摄像头/屏幕/文件自由布局到一个画布上,编码成一路流推出去。

[streamffmpeg.cpp:1310](../../backend/stream_ffmpeg/src/streamffmpeg.cpp#L1310) `pushComposeSceneLoop()`:

```
画布 (canvas_w × canvas_h, YUV420P)
  │ memset Y=16/UV=128 黑底          streamffmpeg.cpp:2291
  ▼
遍历各素材(按层级):
  Camera → CameraFrameBus::snapshot()  streamffmpeg.cpp:2322
  Screen → ScreenFrameBus::snapshot()  streamffmpeg.cpp:2380
  File   → ScreenFrameBus::snapshot()  (PlayerFFmpeg 发布到同一总线)
  │ drawYuv420pContainInSlot()         streamffmpeg.cpp:2373  等比贴入槽位
  ▼
avcodec_send_frame / receive_packet    streamffmpeg.cpp:2442/2454
av_interleaved_write_frame             streamffmpeg.cpp:2461
```

> 🧠 **为什么组合模式只用 YUV420P 软件缓冲?** 注释说:硬编 NV12 与三平面 CPU 写入不兼容,曾导致崩溃。所以组合模式固定 YUV420P 三平面 CPU 合成,只选声明支持 yuv420p 的编码器。

文件素材如何进入总线?`Service::setPlayerComposeStreamBusId` 让 `PlayerFFmpeg::queuePreviewYuv` 同步发布到同一总线(第 3 章的 `queuePreviewYuv` 里有这个分支,见 [playerffmpeg.cpp:1026](../../backend/media_ffmpeg/src/playerffmpeg.cpp#L1026))。

### 7.2 音视频解耦

组合链路音视频解耦:

- **视频线程 + 音频线程并行**采集/编码,复用统一时间基写入复用器;
- **双设备混音**(`audio_pipeline`):双 FIFO 对齐后混单路 AAC。次路不足不阻塞主路,主路可补静音;
- 文件推流不进设备采集混音链路,沿用源内建音视频。

音频采集(Windows):WASAPI 环回采集,见 [backend/stream_ffmpeg/src/platform/windows/wasapiloopbackcapture.cpp](../../backend/stream_ffmpeg/src/platform/windows/wasapiloopbackcapture.cpp)。

✅ **实验 7.1**:开启组合模式,把摄像头画面叠在屏幕画面角落,推流并拉流观看。在 [streamffmpeg.cpp:2373](../../backend/stream_ffmpeg/src/streamffmpeg.cpp#L2373) `drawYuv420pContainInSlot` 处理解槽位布局逻辑。

---

## 第 8 章 性能优化专题

> 目标:从性能视角重新审视前面的链路,学工程级优化思路。

阅读 [doc/性能优化清单.md](../性能优化清单.md)(12 项 P1-P12)。挑几个跟前面链路直接相关的讲:

### P1:DXGI 双路 sws_scale 合并

**问题**:同一帧 BGRA 被两次 `sws_scale`(预览 + 推流),4K 每次 2-8ms。

**位置**:[screencapturedxgi.cpp](../../backend/desktopcapture_dxgi/src/screencapturedxgi.cpp) `captureOneFrame()`。

**优化**:先查 `ScreenFrameBus::publishTargetSize` 决定输出尺寸,单次 `sws_scale` 同时产出预览与推流帧。预估 CPU 降 30-40%。

### P2:帧总线零拷贝改造

**问题**:QByteArray 跨线程 `resize` 触发 detach,一帧复制 2-3 次(第 6.5 节提到的「伪零拷贝」)。

**位置**:[screenframebus.cpp](../../common/src/screenframebus.cpp)、[cameraframebus.cpp](../../common/src/cameraframebus.cpp)。

**优化**:预分配 ring buffer + `std::shared_ptr<uint8_t[]>` + 原子序列号。预估延迟降 20-30%。

### P3:PBO 异步纹理上传

**位置**:[fglwidget.cpp](../../common/src/fglwidget.cpp) `paintGL/updateYUVTextures`。

**现状**:**PBO 双缓冲已实现**(第 6.3 节),文档描述的是进一步流水线化(加 fence sync、缩小锁粒度)。预估帧率提升 15-25%。

### P4:推流编码管线并行化

**问题**:采集/编码/封装单线程串行。

**位置**:[streamffmpeg.cpp](../../backend/stream_ffmpeg/src/streamffmpeg.cpp) `pushScreenLoop/pushCameraLoop`。

**优化**:三阶段流水线 + condition_variable 背压。预估吞吐提升 20-35%。

### P12:编码器预设调优

**位置**:[streamffmpeg.cpp](../../backend/stream_ffmpeg/src/streamffmpeg.cpp) 编码器配置。

**优化**:NVENC 设 `p1 + ll`、AMF 设 `ultralowlatency`、x264 设 `ultrafast + zerolatency`。预估编码延迟降 30-50%。(部分已在第 5.3 节实现)

> 🧠 **性能优化的通用方法论**:① 测量定位瓶颈(不要猜);② 区分 CPU 瓶颈 vs GPU 瓶颈 vs IO 瓶颈;③ 优化热点,避免过早优化;④ 每次优化后重新测量验证。

✅ **实验 8.1**:用性能分析器(VTune/Very Sleepy)跑一遍推流,找出实际热点,对照性能清单看是否吻合。

---

## 第 9 章 学习路线与进阶方向

### 9.1 推荐学习顺序(本教程章节顺序)

```
第0章 理论 → 第1章 跑起来 → 第2章 架构
   ↓
第3章 解码链路(核心,务必吃透)
   ↓
第4章 采集链路 → 第5章 网络链路
   ↓
第6章 渲染链路 → 第7章 组合模式
   ↓
第8章 性能优化
```

每章的「实验」一定要做,光读不练学不会音视频。

### 9.2 配套阅读地图

| 主题          | 文档                                                                                                    |
| ------------- | ------------------------------------------------------------------------------------------------------- |
| 系统总览      | [doc/introduce/1.整体架构设计.md](../introduce/1.整体架构设计.md)                                       |
| 概念图解      | [doc/教程/FPlayer-Desktop-技术全景教程.html](FPlayer-Desktop-技术全景教程.html)                         |
| 播放链路详解  | [doc/backend-media-file-playback-开发详解.md](../backend-media-file-playback-开发详解.md)               |
| 摄像头+OpenGL | [doc/ffmpeg-camera-backend-opengl-playback-guide.md](../ffmpeg-camera-backend-opengl-playback-guide.md) |
| 推拉流        | [doc/推拉流-开发文档.md](../推拉流-开发文档.md)                                                         |
| 屏幕采集      | [doc/屏幕捕获模式-设计与拆分说明.md](../屏幕捕获模式-设计与拆分说明.md)                                 |
| OpenGL        | [doc/introduce/opengl-usage.md](../introduce/opengl-usage.md)                                           |
| 性能          | [doc/性能优化清单.md](../性能优化清单.md)                                                               |
| 音频后端      | [doc/windows-audio-backend-开发说明.md](../windows-audio-backend-开发说明.md)                           |
| 协议          | [doc/introduce/2.流媒体传输协议设计.md](../introduce/2.流媒体传输协议设计.md)                           |
| 服务端联动    | [doc/连接模式与服务分发说明.md](../连接模式与服务分发说明.md)                                           |
| 技术综述      | [doc/论文-项目技术重点与难点综述.md](../论文-项目技术重点与难点综述.md)                                 |

### 9.3 进阶方向

学完本教程后,可以往这些方向拓展:

1. **WebRTC 实时通信**:本项目预留了 WebRTC 后端(`FPLAYER_BUILD_RTC_WEBRTC` OFF),可作为实践项目实现它。WebRTC 涉及 ICE/STUN/TURN、SRTP、拥塞控制,是音视频的高阶方向。
2. **GStreamer 流水线框架**:README 提到「方向1」,适合更复杂场景(NAT 穿透、超低延迟互动)。
3. **服务端流媒体**:配套项目 `fplayer-ff-service`(Go + ZLMediaKit),学控制面/数据面分离、协议转换(RTMP→HTTP-FLV/HLS)。
4. **硬件加速进阶**:CUDA/D3D11VA/VAAPI 硬件编解码,避免 CPU↔GPU 数据往返。
5. **音视频算法**:去噪、超分、美颜等滤镜(`libavfilter`)。

### 9.4 外部补充资源

本项目代码已是最好的教材,但概念层面可补充:

- FFmpeg 官方文档(尤其是 `doc/examples/` 目录的示例代码);
- 雷霄骀(Leixiaohua)的 FFmpeg 教程(中文社区经典入门);
- 《Video Demystified》——视频技术百科全书;
- OpenGL 规范与 `learnopengl.com`。

---

## 附录:关键文件速查表

### 入口与架构

| 角色       | 路径                                                                                                       |
| ---------- | ---------------------------------------------------------------------------------------------------------- |
| 程序入口   | [app/main.cpp](../../app/main.cpp)                                                                         |
| 后端枚举   | [api/include/fplayer/api/media/mediabackendtype.h](../../api/include/fplayer/api/media/mediabackendtype.h) |
| 播放器接口 | [api/include/fplayer/api/media/iplayer.h](../../api/include/fplayer/api/media/iplayer.h)                   |
| 摄像头接口 | [api/include/fplayer/api/media/icamera.h](../../api/include/fplayer/api/media/icamera.h)                   |
| 屏幕接口   | [api/include/fplayer/api/media/iscreencapture.h](../../api/include/fplayer/api/media/iscreencapture.h)     |
| 推拉流接口 | [api/include/fplayer/api/net/istream.h](../../api/include/fplayer/api/net/istream.h)                       |
| 工厂实现   | [runtime/src/runtime.cpp](../../runtime/src/runtime.cpp)                                                   |
| 业务编排   | [service/src/service.cpp](../../service/src/service.cpp)                                                   |
| 主窗口     | [widget/src/capturewindow.cpp](../../widget/src/capturewindow.cpp)                                         |
| 预览视图   | [widget/src/fvideoview.cpp](../../widget/src/fvideoview.cpp)                                               |

### 后端实现

| 角色            | 路径                                                                                                                                               |
| --------------- | -------------------------------------------------------------------------------------------------------------------------------------------------- |
| FFmpeg 播放器   | [backend/media_ffmpeg/src/playerffmpeg.cpp](../../backend/media_ffmpeg/src/playerffmpeg.cpp)                                                       |
| FFmpeg 摄像头   | [backend/media_ffmpeg/src/cameraffmpeg.cpp](../../backend/media_ffmpeg/src/cameraffmpeg.cpp)                                                       |
| 摄像头枚举      | [backend/media_ffmpeg/src/camerainfofetcher.cpp](../../backend/media_ffmpeg/src/camerainfofetcher.cpp)                                             |
| DXGI 屏幕采集   | [backend/desktopcapture_dxgi/src/screencapturedxgi.cpp](../../backend/desktopcapture_dxgi/src/screencapturedxgi.cpp)                               |
| 推拉流(7658 行) | [backend/stream_ffmpeg/src/streamffmpeg.cpp](../../backend/stream_ffmpeg/src/streamffmpeg.cpp)                                                     |
| 推流辅助        | [backend/stream_ffmpeg/src/streamffmpeg_helpers.h](../../backend/stream_ffmpeg/src/streamffmpeg_helpers.h)                                         |
| 音频管线        | [backend/stream_ffmpeg/src/audio_pipeline.h](../../backend/stream_ffmpeg/src/audio_pipeline.h)                                                     |
| WASAPI 回采     | [backend/stream_ffmpeg/src/platform/windows/wasapiloopbackcapture.cpp](../../backend/stream_ffmpeg/src/platform/windows/wasapiloopbackcapture.cpp) |

### 公共组件

| 角色         | 路径                                                                 |
| ------------ | -------------------------------------------------------------------- |
| OpenGL 渲染  | [common/src/fglwidget.cpp](../../common/src/fglwidget.cpp)           |
| 屏幕帧总线   | [common/src/screenframebus.cpp](../../common/src/screenframebus.cpp) |
| 摄像头帧总线 | [common/src/cameraframebus.cpp](../../common/src/cameraframebus.cpp) |

### FFmpeg 关键 API 速查

| 操作       | API                                             | 本项目位置                                                                      |
| ---------- | ----------------------------------------------- | ------------------------------------------------------------------------------- |
| 打开容器   | `avformat_open_input`                           | [playerffmpeg.cpp:238](../../backend/media_ffmpeg/src/playerffmpeg.cpp#L238)    |
| 探测流信息 | `avformat_find_stream_info`                     | [playerffmpeg.cpp:246](../../backend/media_ffmpeg/src/playerffmpeg.cpp#L246)    |
| 选流       | `av_find_best_stream`                           | [playerffmpeg.cpp:1221](../../backend/media_ffmpeg/src/playerffmpeg.cpp#L1221)  |
| 打开解码器 | `avcodec_open2`                                 | [playerffmpeg.cpp:1221](../../backend/media_ffmpeg/src/playerffmpeg.cpp#L1221)  |
| 读包       | `av_read_frame`                                 | [playerffmpeg.cpp:349](../../backend/media_ffmpeg/src/playerffmpeg.cpp#L349)    |
| 解码       | `avcodec_send_packet` / `avcodec_receive_frame` | [playerffmpeg.cpp:519](../../backend/media_ffmpeg/src/playerffmpeg.cpp#L519)    |
| 编码       | `avcodec_send_frame` / `avcodec_receive_packet` | [streamffmpeg.cpp:2442](../../backend/stream_ffmpeg/src/streamffmpeg.cpp#L2442) |
| 色彩转换   | `sws_getContext` / `sws_scale`                  | [playerffmpeg.cpp:543](../../backend/media_ffmpeg/src/playerffmpeg.cpp#L543)    |
| 时间换算   | `av_rescale_q`                                  | [playerffmpeg.cpp:1251](../../backend/media_ffmpeg/src/playerffmpeg.cpp#L1251)  |
| 写网络     | `av_interleaved_write_frame`                    | [streamffmpeg.cpp:2461](../../backend/stream_ffmpeg/src/streamffmpeg.cpp#L2461) |

---

> **结语**:音视频的学习曲线确实陡,但本项目把每条链路都实现得清晰可读。带着本教程,一条链路一条链路地啃,每啃通一条,你对音视频的理解就会上一个台阶。遇到不懂的概念,回到第 0 章;遇到不懂的代码,跟着调用链走。坚持下来,你会拥有真正扎实的音视频工程能力。
>
> 祝学习顺利。
