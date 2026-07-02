# Service 服务层

业务集成，调用底层基础功能模块，供 UI 层直接调用。

核心设计原则：**Service 层保持后端口透明**——用统一接口接收渲染控件，分发给后端，后端按需适配。

## 主要类

| 类 | 职责 |
|---|---|
| `Service` | 统一入口，管理相机/播放器/屏幕捕获/推拉流的生命周期 |
| `ImagePoolService` | 截图池管理，支持目录监听和缩略图缓存 |
| `AiService` | AI 对话服务 |
| `SystemSettingsRepository` | 基于 YAML 的系统设置持久化 |

## 渲染架构

所有后端通过 `FGLWidget` 统一渲染：后端负责采集 + YUV 帧输出，`FGLWidget` 负责 OpenGL 渲染。`Service` 只需一个 `bindCameraPreview(QWidget*)` 方法，后端通过 Qt 信号 `yuvFrameReady` 松耦合连接。

## 构建

依赖 `FPlayer_Runtime`、`FPlayer_Api`、`FPlayer_Common`、`Logger`。
