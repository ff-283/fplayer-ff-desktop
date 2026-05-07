2026.4.11

- [ ] 播放.mov格式的视频时，音画不同步 + 画面播放不出来
- [x] 播放.mp4的时候，绘制过程中，会出现卡顿、画面撕裂（或者当画面中内容大幅度变化的时候，变化大的区域会变成马赛克）
- [x] 进度条不是很准确，特别是在进度条末尾附近进行拖拽的时候
- 目前分析，和使用软解/硬解没有特别大关系，应该与OpenGL绘制、内存开销、线程调度处理有较大关联，没有处理好他们的关系
- 我不是很懂怎么描述bug，缺少音视频、OpenGL方面相关的知识经验。可见调用ai的时候，如果不是很了解相关专业领域的只是，如同竭泽而渔：你只关心ai帮你解决了问题，但是不知道/看不懂具体的实现细节，如果ai写出了bug或者需求不对应功能，你看不懂哪儿不对，甚至连如何描述问题都不知道怎么去描述。所以驾驭工具，打铁还需自身硬。vibe coding，也不是小白就能用的。

2026.4.14

- [ ] 播放.mkv格式的时候，声音比画面更快，声音速度明显超速，画面撕裂

2026.5.6

- [x] 倍速播放（1.25x/1.5x/2.0x）音画不同步 + 声音变尖锐（花栗鼠效应）
  - 根因：旧方案 `swrOutRate = deviceRate / rate` 改变重采样输出速率 → 频域压缩 + 缓冲欠跑
  - 修复：swr 恒定输出设备采样率 + FFmpeg atempo 滤镜保调变速 + 倍速切换时重置音频时钟基准
- [x] 退出文件播放模式（切换到摄像头/屏幕/组合）概率性程序卡死
  - 根因：`playerPause()` 不释放线程和预览目标 → `setBackendType()` 销毁 FGLWidget 后 glWidget 悬空 → use-after-free
  - 修复：模式切换统一改用 `playerStop()`，完整 join 线程 + 清理预览状态
- [x] 音频时钟双重计数导致音画持续微小偏差
  - 根因：纯字节计数方案中 `basePts + bytesSinceBase * rate` 对首帧双重计数（basePts 取首帧 PTS，bytesSinceBase 又包含首帧字节）
  - 修复：回归 FFplay 方式 `playbackOriginMs + (nowProcessedUs - startProcessedUs) * rate / 1000`，去除 basePts
- [x] 新增 Qt6 多媒体文件播放后端 + 架构统一（重构）
  - 新增 `PlayerQt6`（`backend/media_qt6/`）实现 `IPlayer` 接口，基于 `QMediaPlayer` + `QAudioOutput`
  - `QVideoSink` 帧提取 → `ScreenFrameBus`：支持 YUV420P/NV12/NV21 格式
  - 文件推流架构统一：PlayerQt6 喂帧到 ScreenFrameBus → pushScreenPreviewLoop 编码（与摄像头/屏幕一致）
  - 新增 `PushInputKind::FilePreview`、`CaptureParams::sourceId` 路由
  - 系统设置增加文件播放后端选择（默认 Qt6，可选 FFmpeg），保存后热切换
  - `CaptureWindow` 构造函数分离各后端绑定，修复 `qobject_cast` 导致的启动崩溃
  - FFmpeg 全局日志重定向到 Logger，过滤 e2eSoft VCam 等第三方设备噪音
  - P2P 推流 `avio_open2` 加重试（5次×2秒），应对对端流名释放延迟
  - `#if defined(_WIN32)` 宏隔离平台代码，Linux 路径预留 TODO 占位
- [x] 拉流预览窗口改进
  - 关闭预览窗口不再停止拉流，仅隐藏窗口
  - 拉流配置窗口增加「预览窗口」按钮（拉流运行时启用）
  - 拉流预览窗口改为独立窗体，显示在任务栏
- [x] 独立窗体改造
  - 拉流配置窗口、系统设置、AI 识别窗口均改为独立顶层窗口（父窗口 → nullptr）
  - 托盘退出重写为直接停流 + `QApplication::quit()`，不再走 `closeEvent` 确认流程
