# Windows 音频后端开发说明

## 背景

在推流场景中，`DXGI` 只负责视频帧来源。系统声音采集需要独立链路。  
当 FFmpeg 构建未启用 `wasapi indev`，且 `dshow` 回采设备不可用时，传统 `libavdevice` 路径会失败。

为降低对 FFmpeg 构建差异的依赖，项目新增了 Windows 系统 API 音频后端（WASAPI loopback）。

## 目录与职责

- `backend/stream_ffmpeg/src/platform/windows/audioinputprobe.h/.cpp`
  - 职责：`dshow` 设备探测与多候选打开（含常见回采设备名兜底）。
  - 对外接口：
    - `openDshowAudioInputWithFallback(...)`

- `backend/stream_ffmpeg/src/platform/windows/wasapiloopbackcapture.h/.cpp`
  - 职责：基于 Windows 原生 WASAPI loopback 采集系统播放音频。
  - 对外接口：
    - `WasapiLoopbackCapture::init(...)`
    - `WasapiLoopbackCapture::readInterleaved(...)`
    - `WasapiLoopbackCapture::close()`

- `backend/stream_ffmpeg/src/streamffmpeg.cpp`
  - 职责：推流编排与编码封装，不再承载 Windows API 实现细节。
  - 使用方式：
    1. 先调用 `openDshowAudioInputWithFallback(...)`
    2. `dshow` 失败且 `audio=system` 时，切到 `WasapiLoopbackCapture`
    3. 将采集 PCM 通过 `swr` 转换后喂给 AAC 编码器

## 当前回退策略

1. 用户未开启音频（`audio=off`）  
   -> 仅视频推流

2. 用户开启音频  
   -> 先走 `dshow` 探测/打开

3. `dshow` 打开失败且来源为 `system`  
   -> 尝试 native WASAPI loopback（`default render endpoint`）

4. 两者都失败  
   -> 记录失败明细并降级为无声推流

## 场景支持矩阵（当前实现）

- `PushScene::Screen`
  - `__screen_preview__`（DXGI 预览推流）：
    - 支持音频（dshow -> native WASAPI 回退）
  - `__screen_capture__`（gdigrab 推流）：
    - 支持音频（dshow）

- `PushScene::Camera`
  - `__camera_preview__`（当前默认）：
    - 支持音频（dshow -> native WASAPI 回退）
    - `Service` 已透传 `audio` 参数到 camera spec
  - `__camera_capture__`（备用）：
    - 支持音频（dshow -> native WASAPI 回退）

- `PushScene::File`
  - 直推 remux/copy：
    - 保留源音频
  - `__file_transcode__`：
    - 视频转码 + 音频流 copy（若源存在音频）

## 日志关键字

- `音频设备=...`
  - 表示音频链路成功启动

- `dshow音频不可用，已切换系统API回采`
  - 表示从 FFmpeg dshow 自动切换到了 native WASAPI

- `音频采集初始化失败，已自动降级为无声推流`
  - 表示最终未拿到可用音频输入

## 典型排障

### 1) `wasapi:default -> input-format-not-found`
说明 FFmpeg indev 不支持 wasapi。  
不影响 native WASAPI 后端（两者是独立实现）。

### 2) `dshow:audio=... -> I/O error`
说明系统无可用回采设备或设备被占用。  
观察是否后续出现“切换系统API回采”日志。

### 3) 推流中断 `-10054` / `-138`
通常是网络/服务端连接问题，不是音频采集本身问题。

## 后续建议

- 将 `pushScreenLoop` 的音频路径进一步抽象为统一 `audio pipeline` helper（与 `pushScreenPreviewLoop` 对齐）。
- 在 UI 增加“音频诊断”按钮，展示：
  - dshow 枚举设备
  - 当前是否启用 native WASAPI
  - 实际采样率/声道/格式
