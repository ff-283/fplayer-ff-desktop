# FFmpeg 后端摄像头采集与 OpenGL 播放说明

本文基于当前工程实现，梳理 `FFmpeg` 后端如何获取摄像头并在 `OpenGLWidget` 上播放，重点覆盖：

- 摄像头获取
- 分辨率获取
- 开始与暂停播放
- 切换摄像头
- 切换分辨率
- 获取到的分辨率如何确保能正常播放

---

## 1. 关键模块与职责

- `CameraDescriptionFetcher`：负责枚举系统摄像头和候选格式（分辨率+帧率）。
- `CameraFFmpeg`：负责打开设备、解码、采集线程控制、将帧投递到渲染层。
- `FVideoView`：根据后端类型创建预览控件；FFmpeg 模式下创建 `FGLWidget`。
- `FGLWidget`：接收 YUV 帧并用 OpenGL 三纹理 + Shader 渲染到界面。
- `Service` / `RunTime`：UI 与底层采集渲染之间的调用桥接。

---

## 2. 摄像头获取是如何做到的

### 2.1 设备枚举（获取“可选摄像头列表”）

`CameraDescriptionFetcher::getDescriptions()` 在 Windows 下通过 `DirectShow` 枚举：

1. `CoInitializeEx` 初始化 COM。
2. `ICreateDevEnum::CreateClassEnumerator(CLSID_VideoInputDeviceCategory, ...)` 枚举视频输入设备。
3. 读取 `FriendlyName` 作为 UI 展示名（`CameraDescription.description`）。
4. 获取设备 `displayName` 作为 `id`。
5. 同时遍历 `IAMStreamConfig` 能力，收集每个设备的候选格式。

产物是 `QList<CameraDescription>`，用于填充 UI 的设备下拉框。

### 2.2 打开某个摄像头（真正开始采集链路）

`CameraFFmpeg::selectCamera(index)` 的核心步骤：

1. 校验索引并停止旧采集：`stopCapture()` + `cleanup()`。
2. 构建设备字符串：`video=<description>`。
3. 创建 `AVFormatContext`，设置 `interrupt_callback`（用于停止时中断阻塞 IO）。
4. `avformat_open_input(..., av_find_input_format("dshow"), &options)` 打开设备。
5. `avformat_find_stream_info` 获取流信息。
6. 找首个 `AVMEDIA_TYPE_VIDEO` 流并建立解码器：
  - `avcodec_find_decoder`
  - `avcodec_alloc_context3`
  - `avcodec_parameters_to_context`
  - `avcodec_open2`
7. 启动采集线程，在线程中执行 `captureLoop()`。

---

## 3. 分辨率获取是如何做到的

### 3.1 候选分辨率来自哪里

`CameraDescriptionFetcher` 通过 `IAMStreamConfig::GetStreamCaps` 读取 `VIDEOINFOHEADER`：

- 宽高：`biWidth/biHeight`（取绝对值）
- 帧率：`fps = 10000000 / AvgTimePerFrame`
- 文本化后写入 `CameraDescription.formats`，格式形如：`1920x1080 30fps`

并且做了去重（`QSet`），避免驱动重复上报同一能力。

### 3.2 当前策略

- 启动阶段：只做能力枚举，不逐个开流验证（启动更快，避免摄像头反复亮灯）。
- 真正验证：在 `selectCamera()` / `selectCameraFormat()` 时打开设备执行“实测验证”。

---

## 4. 开始播放、暂停播放是如何做到的

### 4.1 开始播放

在当前实现中，“开始播放”分两层含义：

- 设备链路启动：`selectCamera()` 成功后启动采集线程，持续读包解码。
- 画面播放开关：`resume()` 把 `m_isPlaying=true`，允许 `captureLoop()` 推帧。

`captureLoop()` 标准流程：

1. `av_read_frame` 读包。
2. `avcodec_send_packet / avcodec_receive_frame` 解码得到 `AVFrame`。
3. 若非 `YUV420P/YUVJ420P`，先 `sws_scale` 转为 `YUV420P`。
4. 深拷贝为 `QByteArray`（跨线程安全），`emit yuvFrameReady(...)`。

### 4.2 暂停播放

`pause()` 仅设置 `m_isPlaying=false`，采集线程不销毁、解码链路不重建。  
`captureLoop()` 检测到暂停后 `msleep(100)`，停止推送新帧。  
恢复时 `resume()` 将 `m_isPlaying=true`，继续从现有链路快速出帧。

---

## 5. 切换摄像头是如何做到的

入口：`Service::selectCamera(index)` -> `CameraFFmpeg::selectCamera(index)`。

核心机制是“先停旧链路，再建新链路”：

1. `isCapturing=false`，通知采集循环退出。
2. `quit + wait` 等待线程退出；必要时关闭输入打断阻塞读。
3. 释放旧 FFmpeg 资源（`formatContext/codecContext/swsContext` 等）。
4. 用新设备重新执行打开、建流、建解码器、起线程。

这种重建策略避免了旧状态污染新设备，稳定性更高。

---

## 6. 切换分辨率是如何做到的

入口：`Service::selectCameraFormat(index)` -> `CameraFFmpeg::selectCameraFormat(index)`。

实现方式：

1. 更新当前摄像头的 `formatIndex`。
2. 复用 `selectCamera(m_cameraIndex)` 全流程重开。
3. 打开前从格式文本中解析 `width/height/fps`，设置到 dshow 参数：
  - `video_size=<WxH>`
  - `framerate=<fps>`
4. 若“指定格式打开失败”，自动 fallback：不带显式分辨率再试一次（设备默认格式）。

结论：分辨率切换不是“热改参数”，而是“重建采集会话”，以成功率优先。

---

## 7. 在 OpenGLWidget 上播放是如何做到的

### 7.1 预览目标绑定

`FVideoView::previewTarget()` 在 FFmpeg 模式下创建 `FGLWidget`，并把指针放到 `PreviewTarget.backend_hint`。  
`CameraFFmpeg::setPreviewTarget()` 取出该指针并连接：

- `CameraFFmpeg::yuvFrameReady` -> `FGLWidget::updateYUVFrame`（QueuedConnection）

### 7.2 渲染流程

`FGLWidget` 做了 3 件关键事：

1. `updateYUVFrame`：按输入 stride 将 Y/U/V 逐行重打包为紧密内存。
2. `updateYUVTextures`：上传三张 `R8/GL_RED` 纹理（Y 全分辨率，U/V 半分辨率）。
3. `paintGL`：片段着色器执行 YUV->RGB 变换并绘制。

另外，`calculateVertices` 采用 contain 等比策略，避免拉伸变形。

---

## 8. “获取到的分辨率”如何确保能正常播放

当前代码通过“前置筛选 + 打开期验证 + 渲染期兼容”三层保证：

### 8.1 前置筛选（枚举层）

- 只接受 `fps` 在有效区间（1~240）的能力项。
- 通过去重减少异常重复格式干扰。

这一步保证 UI 展示的是“看起来合理”的候选集合。

### 8.2 打开期验证（采集层，最关键）

- 用户选定格式后，`selectCamera()` 以 `video_size + framerate` 实际打开设备。
- 若该组合不可用，`avformat_open_input` 会失败，随后触发 fallback 默认格式重试。
- 只有打开、建流、建解码器都成功，才会进入采集与播放。

这意味着：最终能播放的分辨率是“真实开流成功”的分辨率，而不是仅靠枚举声明。

### 8.3 渲染期兼容（OpenGL 层）

- 解码输出若不是 `YUV420P`，统一 `sws_scale` 转换到 `YUV420P`，避免 shader 不兼容。
- 使用 stride 感知 + 重打包，避免行对齐导致花屏/错位。
- 纹理按实时帧宽高重建，支持切换分辨率后的尺寸变化。
- 采用 `GL_RED/R8` 单通道纹理路径，兼容性优于旧 `GL_LUMINANCE`。

这保证了“即使设备输出格式复杂或 stride 不规则”，仍可稳定显示。

---

## 9. 边界与注意事项（当前实现）

- 枚举能力与真实可开流能力可能存在偏差（驱动常见现象），当前已通过“打开期验证 + fallback”兜底。
- fallback 成功时，实际播放分辨率可能不是用户点选值，而是设备默认值（这是可用性优先策略）。
- 当前按 `description` 组装 `video=<...>`，在个别设备/语言环境下可能需要更稳定的设备标识策略（如唯一名映射）。

---

## 10. 一句话结论

本项目的 FFmpeg 摄像头播放链路本质是：  
**DirectShow 枚举候选能力 -> FFmpeg 按选项真实开流验证 -> 解码帧统一为 YUV420P -> 跨线程安全投递到 FGLWidget -> OpenGL 三纹理着色渲染。**  
其中“分辨率可播放性”主要靠“打开期实测验证 + 失败 fallback + 渲染格式统一与 stride 处理”来保障。