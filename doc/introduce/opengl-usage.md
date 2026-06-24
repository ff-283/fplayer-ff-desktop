# FPlayer Desktop — OpenGL 使用详解

## 1. 概述

OpenGL 在本项目中扮演**视频帧渲染核心**的角色。所有非 Qt6 后端的视频预览（包括 FFmpeg 媒体、DXGI 屏幕采集、文件播放、摄像头采集、组合模式、拉流预览）都通过 OpenGL 将 YUV420P 帧数据渲染到屏幕上。

核心渲染组件是 `FGLWidget`（位于 `common/` 模块），它被设计为一个独立的、可复用的 OpenGL 渲染控件，向上对 widget 层暴露简单的 `updateYUVFrame` 接口，向下封装了完整的 OpenGL 纹理和着色器管线。

---

## 2. 核心组件：FGLWidget

**文件位置：**
- 头文件：[common/include/fplayer/common/fglwidget/fglwidget.h](../common/include/fplayer/common/fglwidget/fglwidget.h)
- 实现：[common/src/fglwidget.cpp](../common/src/fglwidget.cpp)

### 2.1 类继承关系

```
QOpenGLWidget
  └── QOpenGLFunctions (protected 继承)
        └── FGLWidget
```

`FGLWidget` 继承自 `QOpenGLWidget`（Qt 的 OpenGL 渲染容器）并同时混入 `QOpenGLFunctions`，后者提供了跨平台的 OpenGL 函数指针，屏蔽了桌面 OpenGL 与 OpenGL ES 的差异。

### 2.2 核心接口

| 方法 | 说明 |
|------|------|
| `updateYUVFrame(y, u, v, w, h, yStride, uStride, vStride)` | 传入 YUV420P 三个平面数据，触发异步重绘 |
| `setColorParams(isBT709, isFullRange)` | 设置色彩矩阵（BT.601/BT.709）和量化范围（limited/full） |

`updateYUVFrame` 是线程安全的：通过 `QMutex` 保护内部帧缓冲，并使用 Qt 隐式共享（COW）的 `QByteArray` 减少拷贝开销。

### 2.3 OpenGL 资源管理

FGLWidget 管理三类 OpenGL 资源：

| 资源 | 类型 | 用途 |
|------|------|------|
| `m_program` | `QOpenGLShaderProgram*` | YUV→RGB 着色器程序 |
| `m_texY` | `QOpenGLTexture*` | Y（亮度）平面纹理，R8 单通道 |
| `m_texU` | `QOpenGLTexture*` | U（蓝色色度）平面纹理，R8 单通道 |
| `m_texV` | `QOpenGLTexture*` | V（红色色度）平面纹理，R8 单通道 |

纹理在 `updateYUVTextures()` 中按需创建和重建（当分辨率变化时），使用 `QOpenGLTexture::R8_UNorm` 格式，过滤模式为 `Linear`，包裹模式为 `ClampToEdge`。

---

## 3. OpenGL 着色器管线

### 3.1 顶点着色器

```glsl
attribute vec2 position;
attribute vec2 texCoord;
varying vec2 vTexCoord;

void main() {
    gl_Position = vec4(position, 0.0, 1.0);
    vTexCoord = texCoord;
}
```

最简单的 pass-through：将 2D 顶点坐标和纹理坐标传递到片段着色器。

### 3.2 片段着色器 — YUV→RGB 转换

核心逻辑分三步：

**第一步 — 采样三个纹理（每个存储一个 YUV 平面）：**

```glsl
yuv.x = texture2D(texY, vTexCoord).r;  // Y 分量
yuv.y = texture2D(texU, vTexCoord).r;  // U 分量
yuv.z = texture2D(texV, vTexCoord).r;  // V 分量
```

**第二步 — Limited range → Full range 扩展（ITU-R 标准公式）：**

```glsl
if (!uFullRange) {
    yuv.x = (yuv.x - 16.0/255.0) * (255.0/219.0);
    yuv.y = (yuv.y - 16.0/255.0) * (255.0/224.0);
    yuv.z = (yuv.z - 16.0/255.0) * (255.0/224.0);
}
```

**第三步 — 色彩矩阵转换（BT.601 或 BT.709）：**

- **BT.709**（HD/4K：720p+）：chroma 系数 `(0, -0.21482, 2.12798)` / `(1.28033, -0.38059, 0)`
- **BT.601**（SD：≤576p）：chroma 系数 `(0, -0.39465, 2.03211)` / `(1.13983, -0.58060, 0)`

### 3.3 顶点计算 — Contain 等比显示

`calculateVertices()` 在每帧根据窗口尺寸和视频尺寸动态计算顶点坐标，实现 **contain** 模式（保持宽高比，不足部分留黑边）：

- 窗口更宽 → 高度贴满，左右留黑边
- 窗口更高 → 宽度贴满，上下留黑边

使用 `GL_TRIANGLE_FAN` 绘制四边形（4 个顶点：左下→右下→右上→左上），每个顶点包含 `(x, y, u, v)` 4 个 float。

---

## 4. OpenGL 帧上传策略

### 4.1 单色通道纹理

YUV420P 的每个平面被分别上传到独立的 GL 纹理，格式为 `GL_RED`，类型为 `GL_UNSIGNED_BYTE`，对齐设为 1 字节（`GL_UNPACK_ALIGNMENT = 1`）。

### 4.2 Stride 处理

对于 stride ≠ width 的情况（如 DXGI 采集带有对齐填充），有两种策略：

- **Windows**：统一走 CPU 重打包路径（`repackPlaneTight`），逐行 `memcpy` 去掉填充，因为 Windows 部分驱动对 `GL_UNPACK_ROW_LENGTH` 支持不稳定
- **非 Windows / OpenGL ES 3.0+**：使用 `GL_UNPACK_ROW_LENGTH` 直接按源 stride 上传，避免额外的 CPU 拷贝

### 4.3 纹理绑定流程（paintGL）

```
glClear(GL_COLOR_BUFFER_BIT)
  → m_program->bind()
    → updateYUVTextures()  // 按需创建/重建纹理，上传 Y/U/V 数据
      → glActiveTexture(GL_TEXTURE0) → m_texY->bind()
      → glActiveTexture(GL_TEXTURE1) → m_texU->bind()
      → glActiveTexture(GL_TEXTURE2) → m_texV->bind()
        → 设置 vertex attrib（position + texCoord，交错存储）
        → 设置 uniform（sampler 索引 + 色彩参数）
          → glDrawArrays(GL_TRIANGLE_FAN, 0, 4)
```

---

## 5. 初始化与全局配置

### 5.1 应用级 OpenGL 配置

在 [app/main.cpp](../app/main.cpp) 中，**创建 QApplication 之前**设置全局默认 Surface Format：

```cpp
QSurfaceFormat fmt;
fmt.setSwapBehavior(QSurfaceFormat::DoubleBuffer);  // 双缓冲
fmt.setSwapInterval(1);                              // VSync
QSurfaceFormat::setDefaultFormat(fmt);
```

这确保所有 OpenGL 窗口默认支持双缓冲 + 垂直同步，防止画面撕裂。

### 5.2 Widget 级配置

FGLWidget 构造函数中进一步设置：

```cpp
setUpdateBehavior(QOpenGLWidget::NoPartialUpdate);  // 禁用部分更新，避免 Linux/X11 闪烁
setAttribute(Qt::WA_OpaquePaintEvent, true);        // 不透明绘制
setAttribute(Qt::WA_NoSystemBackground, true);       // 无系统背景
setAutoFillBackground(false);
```

### 5.3 FGLWidget 析构

析构时调用 `makeCurrent()` 确保 OpenGL 上下文有效，然后安全删除所有 GL 资源（纹理、着色器程序），最后调用 `doneCurrent()`。

---

## 6. 与各后端的集成方式

### 6.1 预览目标桥接（PreviewTarget）

`FVideoView` 根据后端类型决定创建何种渲染控件：

```cpp
if (backend == Qt6)
    → 创建 QVideoWidget（Qt 原生多媒体渲染）
if (backend == FFmpeg || backend == Dxgi)
    → 创建 FGLWidget（OpenGL YUV 渲染）
```

`FVideoView::previewTarget()` 将 `FGLWidget` 的指针放入 `PreviewTarget::backend_hint`，后端通过 `static_cast<FGLWidget*>(target.backend_hint)` 获取并直接调用 `updateYUVFrame`。

### 6.2 各后端的帧投递方式

| 后端 | 投递方式 | 线程模型 |
|------|----------|----------|
| **CameraFFmpeg** | Qt signal/slot 直接连接 → `updateYUVFrame` (`QueuedConnection`) | 采集线程发射信号，主线程槽执行 |
| **PlayerFFmpeg** | 解码线程将帧存入中间缓冲区（加锁），通过 `QMetaObject::invokeMethod` 派发 `deliverPreviewFrame` 槽函数 | 解码线程 → 中间缓冲 → 主线程槽 |
| **ScreenCaptureFFmpeg** | lambda + `QMetaObject::invokeMethod`（`QueuedConnection`），帧数据深拷贝后投递 | 采集线程深拷贝 → 主线程 |
| **ScreenCaptureDxgi** | 同上，DXGI 采集线程深拷贝 YUV 后通过 lambda 投递 | 采集线程深拷贝 → 主线程 |
| **组合模式（拉流预览）** | 定时器 + `ScreenFrameBus::snapshotIfNew()` 获取帧 → `QMetaObject::invokeMethod` | 定时器驱动（主线程） |

所有后端都通过 `QMetaObject::invokeMethod` 或 signal/slot 将帧数据线程安全地投递到 `FGLWidget` 所在的 GUI 线程。

### 6.3 摄像机后端的额外处理

CameraFFmpeg 在采集循环中统一将非 YUV420P 格式的帧通过 `libswscale` 转换为 YUV420P：

```cpp
// 将输入像素格式统一转换为 YUV420P，简化 OpenGL 渲染端处理。
if (frame->format != AV_PIX_FMT_YUV420P && frame->format != AV_PIX_FMT_YUVJ420P) {
    sws_scale(...);  // 转换为 YUV420P
}
```

这可以保证 OpenGL 渲染端始终只接收 YUV420P 格式，着色器只需处理这一种格式。

---

## 7. 构建配置

### 7.1 common 模块

[common/CMakeLists.txt](../common/CMakeLists.txt) 中声明了 OpenGL 依赖：

```cmake
find_package(Qt6 COMPONENTS Core OpenGL OpenGLWidgets REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE Qt6::Core Qt6::OpenGL Qt6::OpenGLWidgets)
target_link_libraries(${PROJECT_NAME} PUBLIC Qt6::OpenGL Qt6::OpenGLWidgets)
```

OpenGL 和 OpenGLWidgets 使用 **PUBLIC** 链接，意味着任何依赖 `FPlayer_Common` 的模块都会自动获得 OpenGL 的 include 路径和库链接。

### 7.2 依赖传播

由于 `common` 模块 PUBLIC 链接了 `Qt6::OpenGL` 和 `Qt6::OpenGLWidgets`，所有后端模块（`media_ffmpeg`、`stream_ffmpeg`、`desktopcapture_dxgi`）以及 `widget` 模块都无需单独声明 OpenGL 依赖 —— 只需链接 `FPlayer_Common` 即可。

---

## 8. 平台兼容性处理

### 8.1 Windows

- `GL_UNPACK_ROW_LENGTH` 被禁用（经验上部分驱动实现不稳定），统一走 CPU 重打包路径
- DXGI 屏幕采集后端使用相同的 FGLWidget 渲染管线

### 8.2 Linux (X11)

- `NoPartialUpdate` 行为设置用于消除持续闪烁/抖动问题
- 非 OpenGL ES 环境默认启用 `GL_UNPACK_ROW_LENGTH`

### 8.3 OpenGL ES

- 版本 < 3.0 时检测 `GL_EXT_unpack_subimage` 扩展决定是否使用 `GL_UNPACK_ROW_LENGTH`

---

## 9. 架构位置总结

```
app/main.cpp                        设置 QSurfaceFormat（全局 OpenGL 配置）
       │
widget/src/fvideoview.cpp           按后端类型创建 FGLWidget，封装为 PreviewTarget
       │
widget/src/capturewindow.cpp        组合模式中通过 ScreenFrameBus + FGLWidget 渲染拉流预览
       │
common/src/fglwidget.cpp            OpenGL YUV→RGB 渲染核心
       │
backend/media_ffmpeg/src/           CameraFFmpeg / PlayerFFmpeg / ScreenCaptureFFmpeg
  ├── cameraffmpeg.cpp               采集 YUV → signal → FGLWidget::updateYUVFrame
  ├── playerffmpeg.cpp               解码 YUV → invokeMethod → FGLWidget::updateYUVFrame
  └── screencaptureffmpeg.cpp        采集 YUV → invokeMethod → FGLWidget::updateYUVFrame
       │
backend/desktopcapture_dxgi/src/
  └── screencapturedxgi.cpp          DXGI 采集 YUV → invokeMethod → FGLWidget::updateYUVFrame
```
