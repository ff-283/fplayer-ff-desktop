# FPlayer Desktop — 核心业务流程

## 1. 概述

FPlayer Desktop 的核心业务围绕三种采集模式展开：**摄像头采集**、**文件播放**、**屏幕捕获**。三种模式共享统一的分层调用架构，自 UI 层逐层向下，经 Service、Runtime、Backend，最终通过帧总线（FrameBus）输出至渲染或推流模块。

![核心业务流程泳道图](视频采集泳道图.drawio)

---

## 2. 分层调用链

系统采用 5 层泳道结构组织调用流，各层职责如下：

| 泳道 | 所属层 | 职责 |
|------|--------|------|
| UI 层 | Widget | 用户交互入口，接收操作指令，设置参数 |
| Service | Service | 业务编排门面，参数校验，状态管理 |
| Runtime | Runtime | 抽象工厂，根据 `MediaBackendType` 创建对应的 Backend 实例 |
| Backend | Backend | 具体实现层，调用底层 SDK 执行采集/解码 |
| 数据/输出 | Common + Backend | 帧总线分发，OpenGL 渲染，或 StreamFFmpeg 推流 |

调用方向严格单向：**UI → Service → Runtime → Backend → 数据/输出**，上层仅依赖 API 接口，不感知具体实现。

---

## 3. 流程一：摄像头采集

### 3.1 调用路径

```
CaptureWindow (选择设备、设置预览目标)
  → Service::initCamera() / selectCamera() / cameraResume()
    → RunTime::createCamera(backend)
      → CameraFFmpeg (dshow) 或 CameraQt6 (QCamera)
        → CameraFrameBus → FGLWidget (OpenGL 渲染)
                        → StreamFFmpeg (RTMP/SRT 推流)
```

### 3.2 详细步骤

1. **UI 层**：用户在 `CaptureWindow` 中选择摄像头设备并配置预览参数（分辨率、帧率）
2. **Service 层**：`Service` 调用 `initCamera(backend)` 初始化，`selectCamera(index)` 切换设备，`cameraResume()` 启动采集
3. **Runtime 层**：工厂方法 `createCamera(MediaBackendType)` 根据启动参数创建 `CameraFFmpeg` 或 `CameraQt6`
4. **Backend 层**：
   - `CameraFFmpeg`：通过 FFmpeg libavdevice (dshow) 捕获，软件解码为 YUV420P，发布至 `CameraFrameBus`
   - `CameraQt6`：封装 QCamera + QMediaCaptureSession，利用 Qt6 原生 API
5. **数据/输出**：`CameraFrameBus`（线程安全单例）按 sourceId 分发 YUV 帧，下游消费者包括 `FGLWidget`（预览渲染）和 `StreamFFmpeg`（推流编码）

### 3.3 关键类

| 类 | 文件 | 说明 |
|----|------|------|
| `CameraFFmpeg` | `backend/media_ffmpeg/cameraffmpeg.h` | dshow 采集 + 软件解码 |
| `CameraQt6` | `backend/media_qt6/cameraqt6.h` | QCamera 封装 |
| `CameraFrameBus` | `common/cameraframebus/cameraframebus.h` | YUV 帧总线（单例） |

---

## 4. 流程二：文件播放

### 4.1 调用路径

```
CaptureWindow (选择文件、播放/暂停/跳转)
  → Service::initPlayer() / openMediaFile() / playerResume()
    → RunTime::createPlayer(backend)
      → PlayerFFmpeg (avformat/avcodec) 或 PlayerQt6 (QMediaPlayer)
        → FGLWidget (OpenGL 渲染)
        → StreamFFmpeg (组合模式推流)
```

### 4.2 详细步骤

1. **UI 层**：用户在 `CaptureWindow` 中通过文件对话框选择媒体文件，操作播放、暂停、停止、进度跳转
2. **Service 层**：`Service` 调用 `initPlayer(backend)`，然后 `openMediaFile(path)` 打开文件，`playerResume()` 开始播放。支持 `playerSeekMs()` 精确跳转
3. **Runtime 层**：`createPlayer(MediaBackendType)` 根据配置返回 `PlayerFFmpeg` 或 `PlayerQt6`
4. **Backend 层**：
   - `PlayerFFmpeg`：使用 FFmpeg avformat 解封装 + avcodec 软件解码，输出 YUV420P，通过 `FGLWidget` 以 OpenGL 渲染（BT.601/BT.709 色彩矩阵）
   - `PlayerQt6`：使用 QMediaPlayer + QAudioOutput + QVideoSink，通过 `QVideoSink::videoFrameChanged` 信号获取帧数据发布至帧总线
5. **数据/输出**：播放帧直接渲染到 `FGLWidget` 供用户观看；在组合模式下，通过 `ScreenFrameBus` 发布帧供 `StreamFFmpeg` 混合编码推流

### 4.3 关键类

| 类 | 文件 | 说明 |
|----|------|------|
| `PlayerFFmpeg` | `backend/media_ffmpeg/playerffmpeg.h` | FFmpeg 软件解码播放器 |
| `PlayerQt6` | `backend/media_qt6/playerqt6.h` | QMediaPlayer 封装 |
| `FGLWidget` | `common/fglwidget/fglwidget.h` | OpenGL YUV 渲染控件 |

---

## 5. 流程三：屏幕捕获

### 5.1 调用路径

```
CaptureWindow (选择屏幕/显示器、设置帧率)
  → Service::initScreenCapture() / selectScreen() / screenSetActive()
    → RunTime::createScreenCapture(backend)
      → ScreenCaptureDxgi (DXGI) / ScreenCaptureQt6 / ScreenCaptureFFmpeg
        → ScreenFrameBus → FGLWidget (OpenGL 渲染)
                        → StreamFFmpeg (RTMP/SRT 推流)
```

### 5.2 详细步骤

1. **UI 层**：用户在 `CaptureWindow` 中选择目标屏幕/显示器，配置捕获帧率和光标捕获开关
2. **Service 层**：`Service` 调用 `initScreenCapture(backend)`→`selectScreen(index)`→`screenSetActive(true)` 启动捕获
3. **Runtime 层**：`createScreenCapture(MediaBackendType)` 优先创建 `ScreenCaptureDxgi`（DXGI），也可回退至 `ScreenCaptureQt6` 或 `ScreenCaptureFFmpeg`
4. **Backend 层**（三种实现）：
   - `ScreenCaptureDxgi`（首选）：基于 DirectX 11 Desktop Duplication API，支持光标叠加，BGRA → YUV420P 通过 swscale 转换，运行于后台 QThread
   - `ScreenCaptureQt6`：封装 QScreenCapture + QMediaCaptureSession
   - `ScreenCaptureFFmpeg`：基于 FFmpeg gdigrab，运行于后台 QThread
5. **数据/输出**：帧数据发布至 `ScreenFrameBus`（支持 `snapshotIfNew()` 差分消费），下游消费于 `FGLWidget` 预览和 `StreamFFmpeg` 推流

### 5.3 关键类

| 类 | 文件 | 说明 |
|----|------|------|
| `ScreenCaptureDxgi` | `backend/desktopcapture_dxgi/screencapturedxgi.h` | DXGI 高性能屏幕捕获 |
| `ScreenCaptureFFmpeg` | `backend/media_ffmpeg/screencaptureffmpeg.h` | gdigrab 屏幕捕获 |
| `ScreenCaptureQt6` | `backend/media_qt6/screencaptureqt6.h` | QScreenCapture 封装 |
| `ScreenFrameBus` | `common/screenframebus/screenframebus.h` | 屏幕帧总线（单例） |

---

## 6. 公共输出通道

三种采集模式的输出汇聚于以下公共通道：

| 通道 | 说明 |
|------|------|
| **FGLWidget** | OpenGL YUV 渲染器，使用 GLSL 着色器做 BT.601/BT.709 色彩空间转换，支持 limited/full range，三种模式共用 |
| **CameraFrameBus / ScreenFrameBus** | 线程安全帧总线，mutex 保护，按 sourceId 分区发布/订阅，解耦生产者与消费者 |
| **StreamFFmpeg** | 统一推流后端。支持 remux（仅封装格式转换）、transcode（重新编码）、compose（多源场景合成推流）三种模式。Push route 由 spec 字符串驱动解析（`streamffmpeg_helpers`），Pull 支持录制 |
| **组合模式** | CaptureWindow 的 compose canvas 支持多源（摄像头+文件+屏幕）MDI 布局，Z-order 叠加，然后通过 StreamFFmpeg 统一编码推流 |

---

## 7. 设计要点

- **可插拔后端**：通过 CLI `--backend` 参数在 Qt6 / FFmpeg / DXGI 间切换，架构不绑定具体实现
- **帧总线解耦**：生产者（Backend）与消费者（渲染、推流、截图）通过帧总线异步通信，互不阻塞
- **依赖反转**：UI/Service 层仅依赖 API 纯虚接口（`ICamera` / `IPlayer` / `IScreenCapture`），Runtime 层作为唯一组装点
- **线程模型**：FFmpeg/DXGI 后端在后台线程中运行，帧总线的 mutex 保证线程安全
- **音频采集**：WASAPI loopback 用于系统音频采集，配合推流场景使用
