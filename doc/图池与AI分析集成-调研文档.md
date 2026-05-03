# 图池（Image Pool）与 AI 分析集成 — 实现文档

> 版本：v1.2 | 日期：2026-05-02 | 状态：已实现

---

## 一、功能概述

在现有截图功能基础上，新增一个**图池（Image Pool）独立窗口**，以相册方式浏览截图目录下的所有图片，支持右键菜单文件操作，并支持调用云端 AI 大模型对指定图片进行交互式对话分析。

---

## 二、实现功能清单

| 编号 | 功能 | 状态 |
|------|------|------|
| R1 | 图池独立窗体（`Qt::Window`），任务栏可见，菜单栏"视图→图池"可唤出 | 已实现 |
| R2 | 缩略图网格展示，等比缩放保持原图比例 | 已实现 |
| R3 | 截图后自动刷新（事件驱动 + QFileSystemWatcher + 手动刷新） | 已实现 |
| R4 | 右键菜单：打开 / 打开所在文件夹 / 复制图片 / 复制路径 / 重命名 / 删除 / AI 识别 | 已实现 |
| R5 | 排序：按日期、名称、大小，正序/倒序 | 已实现 |
| R6 | 双击打开大图预览，支持键盘导航、缩放 | 已实现 |
| R7 | AI 对话：右键"AI 识别"→ 对话框，左侧图片预览 + 右侧对话气泡 | 已实现 |
| R8 | AI 配置：API Endpoint、API Key、Model 可配置，持久化到 YAML | 已实现 |
| R9 | 主窗口和拉流预览截图均触发图池刷新 | 已实现 |
| R10 | 图池按钮：已打开时置顶聚焦，最小化时还原 | 已实现 |
| R11 | AI 流式响应：SSE 逐字输出 + 闪烁光标 + 输入中指示 | 已实现 |
| R12 | 自动发送：打开 AI 对话窗自动发送分析请求 | 已实现 |
| R13 | AI 配色自定义：气泡、背景、工具栏颜色（色块预览+取色器） | 已实现 |
| R14 | 拉流预览窗口共享图池按钮 | 已实现 |

---

## 二点五、AI 对话配色自定义

在系统设置的「AI 识别配置」区域新增 4 个颜色配置项，每项旁有 32×22 色块实时预览，点击弹出系统取色器（QColorDialog）。

| 配置项 | YAML Key | 默认值 | 说明 |
|--------|---------|--------|------|
| 用户气泡颜色 | `ai_user_bubble_color` | `#0078d4` | 右侧蓝色气泡 |
| AI 气泡颜色 | `ai_ai_bubble_color` | `#ffffff` | 左侧白色气泡 |
| 聊天背景颜色 | `ai_chat_bg_color` | `#000000` | 对话区底色 |
| 图池工具栏颜色 | `image_pool_toolbar_color` | `#000000` | 图池窗口工具栏背景 |

颜色通过 `AiConfig` 传递至 `AiChatDialog`，`bubbleHtml()` 动态渲染。工具栏颜色通过 `ImagePoolSidebar::setToolbarColor()` 注入。

---

## 三、最终架构（与初始设计的主要差异）

### 3.1 UI 模式：独立窗体（非嵌入侧边栏）

初始设计为 `QSplitter` 嵌入主窗口右侧。最终采用**独立 `Qt::Window` 窗体**，原因：
- 无需修改主窗口布局
- 可在任务栏中独立显示，不会"丢失"
- 菜单栏"视图 → 图池"可随时唤出
- 关闭按钮（X）隐藏窗体而非销毁，再次点击按钮/菜单即可恢复

### 3.2 总架构

```
CaptureWindow (主窗口)
  ├── wgtView (视频预览区)
  ├── wgtDown (底部栏)
  │   └── btnImagePool → toggle ImagePoolSidebar
  ├── 拉流预览窗口 → btnImagePool（共享同一实例）
  ├── 菜单栏 → "视图 → 图池" → toggle
  │
  └── ImagePoolSidebar (独立 Qt::Window, 无父子吸附)
        ├── 工具栏 [刷新] [排序▼] [共 N 张]
        ├── 缩略图网格 (QListWidget IconMode, 等比缩略图)
        └── 右键菜单 → AI 识别 → AiChatDialog

service 层:
  ┌──────────────────────┐  ┌──────────────────────┐
  │ ImagePoolService     │  │ AiService            │
  │ - scanDir()          │  │ - sendMessage()      │
  │ - QFileSystemWatcher │  │ - encodeImageBase64()│
  │ - thumbnail()        │  │ - QNetworkAccessMgr  │
  └──────────────────────┘  └──────────────────────┘
           │                         │
           ▼                         ▼
    SystemSettingsRepository    OpenAI 兼容 API
    (screenshotDir, aiConfig)   (HTTPS POST)
```

### 3.3 模块拆分

| 层 | 文件 | 职责 |
|----|------|------|
| **common** | `imagepool/imagemetadata.h` | 图片元数据结构体 |
| **service** | `imagepoolservice.h/cpp` | 目录扫描、QFileSystemWatcher、缩略图缓存 |
| **service** | `aiservice.h/cpp` | QNetworkAccessManager 封装，图片 base64 编码，API 请求/响应 |
| **widget** | `imagepoolsidebar.h/cpp` | 图池独立窗体：网格、工具栏、右键菜单、排序 |
| **widget** | `imageviewerdialog.h/cpp` | 大图预览（导航、缩放、键盘操作） |
| **widget** | `aichatdialog.h/cpp` | AI 对话窗：图片预览 + 聊天气泡 + 消息收发 |

---

## 四、同步与刷新机制

```
截图事件源:
  ├── handleMainCaptureScreenshot()  ← 主窗口截图
  └── pull preview screenshot        ← 拉流预览截图

三层保障:
  ┌─────────────────────────────────────────────┐
  │ 第一层：事件驱动（主动通知，~0ms）            │
  │ 截图保存成功 → emit screenshotSaved(path)    │
  │ → ImagePoolSidebar::onScreenshotSaved()     │
  │ → 即时追加缩略图到网格                       │
  ├─────────────────────────────────────────────┤
  │ 第二层：文件系统监听（被动检测，~100-500ms）  │
  │ QFileSystemWatcher 监听 screenshotDir        │
  │ → directoryChanged → 增量 diff → 同步        │
  ├─────────────────────────────────────────────┤
  │ 第三层：手动刷新（兜底）                      │
  │ 工具栏 [刷新] 按钮 → scanDirectory() 全量重扫 │
  └─────────────────────────────────────────────┘
```

---

## 五、AI 集成方案

### 5.1 API 协议

采用 **OpenAI Chat Completions 兼容接口**，启用 `"stream": true` SSE 流式输出。

```
POST {endpoint}
Authorization: Bearer {apiKey}
Content-Type: application/json

{
  "model": "{model}",
  "messages": [
    {
      "role": "system",
      "content": "你是一个有帮助的AI助手。请用中文回答用户的问题。"
    },
    {
      "role": "user",
      "content": [
        { "type": "image_url", "image_url": { "url": "data:image/png;base64,..." } },
        { "type": "text", "text": "用户的问题" }
      ]
    }
  ],
  "max_tokens": 1000,
  "stream": true
}
```

`AiService::onReadyRead()` 逐行解析 SSE `data:` 行，提取 `delta.content`，emit `responseChunk(chunk)`；流结束 emit `responseFinished()`。

### 5.2 错误处理

| HTTP 状态 | 提示信息 |
|-----------|---------|
| 401 | "API Key 无效 (401)，请检查设置。" |
| 429 | "请求过于频繁 (429)，请稍后重试。" |
| 网络不通 | "请求失败：{errorString}" |
| 未配置 Key | "未配置 API Key，请在系统设置中填写。" |

### 5.3 图片预处理

- 读取时自动旋转（`QImageReader::setAutoTransform(true)`）
- 超过 2048px 自动压缩到 2048px
- PNG 格式编码为 base64

### 5.4 对话界面

聊天区使用三种视觉气泡区分身份：

| 角色 | 样式 | 对齐 |
|------|------|------|
| **我**（用户） | 蓝底白字 `#0078d4`，圆角气泡 | 右对齐 |
| **AI 助手** | 白底深灰字，灰色边框气泡 | 左对齐 |
| **系统**（错误） | 黄底 `#fff3cd` 橙色边框 | 左对齐 |

---

## 六、主流模型配置用例

所有配置在「系统设置 → AI 识别配置」中填写。

![image-20260503224439510](https://map--depot.oss-cn-hangzhou.aliyuncs.com/image/image-20260503224439510.png)

### 6.1 模型能力速查

| 服务商 | 模型 | 图片识别 | 国内直连 | 付费 | 推荐场景 |
|--------|------|:---:|:---:|:---:|------|
| 智谱 | `glm-4v-flash` | ✅ | ✅ | 免费额度 | **首选测试** |
| 智谱 | `glm-4v-plus` | ✅ | ✅ | 按量 | 高精度需求 |
| 阿里通义千问 | `qwen-vl-plus` | ✅ | ✅ | 按量 | 中文场景最佳 |
| 阿里通义千问 | `qwen-vl-max` | ✅ | ✅ | 按量 | 最强中文视觉 |
| Kimi | `moonshot-v1-8k-vision-preview` | ✅ | ✅ | 按量 | 国产替代 |
| Ollama 本地 | `llava:13b` / `llama3.2-vision` | ✅ | ✅ | 免费 | 离线/隐私 |
| OpenAI | `gpt-4o` | ✅ | ❌ 需代理 | 预付费 | 综合最强 |
| DeepSeek | `deepseek-chat` / `deepseek-reasoner` | ❌ | ✅ | 按量 | — |

> **核心要求**：本功能发送多模态请求（图片 base64 + 文本），模型必须支持 `image_url` 类型的 content。**纯文本模型会返回 HTTP 400 错误。**

### 6.2 智谱 GLM（推荐首选，免费额度）

开通：[open.bigmodel.cn](https://open.bigmodel.cn) → 注册 → API 密钥管理

| 字段 | 值 |
|------|-----|
| API 地址 | `https://open.bigmodel.cn/api/paas/v4/chat/completions` |
| API Key | 从智谱控制台获取（格式：`xxx.yyy`） |
| 模型 | `glm-4v-flash`（免费）/ `glm-4v`（标准）/ `glm-4v-plus`（最强） |

### 6.3 阿里云通义千问（中文场景最佳）

开通：[dashscope.console.aliyun.com](https://dashscope.console.aliyun.com) → 开通 DashScope → API Key 管理

| 字段 | 值 |
|------|-----|
| API 地址 | `https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions` |
| API Key | DashScope 控制台获取（`sk-...`） |
| 模型 | `qwen-vl-plus`（均衡）/ `qwen-vl-max`（最强） |

### 6.4 Kimi（Moonshot）

开通：[platform.moonshot.cn](https://platform.moonshot.cn)

| 字段 | 值 |
|------|-----|
| API 地址 | `https://api.moonshot.cn/v1/chat/completions` |
| API Key | 平台控制台获取（`sk-...`） |
| 模型 | `moonshot-v1-8k-vision-preview` |

### 6.5 OpenAI 官方

| 字段 | 值 |
|------|-----|
| API 地址 | `https://api.openai.com/v1/chat/completions` |
| API Key | [platform.openai.com/api-keys](https://platform.openai.com/api-keys) 获取（`sk-proj-...`） |
| 模型 | `gpt-4o` / `gpt-4o-mini`（更快更便宜） |

说明：需预付费，国内用户需自行解决网络访问。

### 6.6 Anthropic Claude（via 兼容代理）

Anthropic 原生 API 格式与 OpenAI 不兼容。需使用代理网关（如 [one-api](https://github.com/songquanpeng/one-api)）转换。

| 字段 | 值 |
|------|-----|
| API 地址 | `https://your-proxy.com/v1/chat/completions` |
| API Key | 代理网关颁发的 Key |
| 模型 | `claude-sonnet-4-20250514` / `claude-opus-4-20250514` |

### 6.7 本地 Ollama（免费，离线）

| 字段 | 值 |
|------|-----|
| API 地址 | `http://localhost:11434/v1/chat/completions` |
| API Key | 留空（本地无需认证） |
| 模型 | `llava:13b` / `llama3.2-vision:11b` / `minicpm-v:latest` |

安装：`ollama pull llava:13b`

### 6.8 DeepSeek（❌ 不支持图片）

> DeepSeek V3 (`deepseek-chat`) 和 R1 (`deepseek-reasoner`) 是纯文本模型，发送图片会返回 **HTTP 400** 错误。本功能无法使用 DeepSeek，请选择上方支持图片的模型。

### 6.9 任意 OpenAI 兼容网关

只要提供 `/v1/chat/completions` 端点且支持 `image_url` content 类型的服务均可使用。

---



---

## 七、实现细节

### 7.1 ImageMetadata（common 层）

纯数据结构体，无 DLL 导出宏（避免 MSVC 隐式构造/析构导入问题）。成员：`filePath`、`fileName`、`fileSize`、`birthTime`、`lastModified`、`imageSize`、`isLoaded`。

### 7.2 ImagePoolService（service 层）

```
QObject 子类，负责目录监控与缩略图管理。

核心方法：
  setWatchDir(dir)     → 切换监听目录，触发全量扫描
  imagePaths()         → 返回当前内存中已排序的路径列表
  imageMeta(path)      → 返回单张图片元数据（缓存）
  thumbnail(path,size) → LRU 缩略图缓存（最大 200 张）
  refresh()            → 强制全量扫描

信号：
  directoryChanged(dir) → watcher 检测到目录变更
  imageAdded(path)      → 增量新增
  imageRemoved(path)    → 增量删除
  scanFinished()        → 扫描完成

内部机制：
  - QFileSystemWatcher 监听目录
  - onDirectoryChanged() 增量 diff 旧/新路径列表
  - 缩略图 LRU：QStringList m_lruList + QHash<key, QPixmap>
  - 元数据缓存：QHash<path, ImageMeta>，lastModified 变化时刷新
```

### 7.3 ImagePoolSidebar（widget 层）

```
独立 Qt::Window（非嵌入主窗口），构造时传入 Qt::Window flag。

UI 结构：
  QVBoxLayout
    ├── 工具栏 (QWidget)
    │   ├── [刷新] QPushButton
    │   ├── [排序] QComboBox
    │   └── [共 N 张] QLabel
    └── QListWidget (IconMode)
          └── 缩略图网格

缩略图等比显示：
  makeThumbnail(path):
    1. QImageReader 读原图尺寸
    2. QSize::scaled(200, 150, KeepAspectRatio) 计算等比缩放尺寸
    3. 绘制到透明 QPixmap 画布 (208×158) 居中
    4. 横向/竖向图片各得正确比例

排序：
  QComboBox → ImagePoolService::SortMode enum
  → std::stable_sort 按 QFileInfo 字段排序
  → 重新填充 QListWidget

窗口生命周期：
  - closeEvent() → hide() + event->ignore()（X 按钮隐藏而非销毁）
  - 菜单栏"视图→图池"勾选同步 visibilityChanged 信号
  - btnImagePool：可见时 raise()+activateWindow() 或 showNormal()（最小化还原）
```

### 7.4 ImageViewerDialog（widget 层）

```
QDialog，支持导航、缩放、键盘操作。

关键设计：
  - updateImage() 只加载原图 QPixmap，不做缩放
  - fitToWindow() 按 viewport 实际尺寸等比缩放
  - showEvent() 首次显示时调用 fitToWindow()（此时布局已完成）
  - viewport < 100px 时兜底 800×600
  - 键盘：← → 导航、+/- 缩放、0 还原、Esc 关闭
```

### 7.5 AiService（service 层）

```
QObject + QNetworkAccessManager，发送 OpenAI 兼容多模态请求。

sendMessage(imagePath, userMessage, systemPrompt):
  1. 检查 apiKey 是否为空
  2. encodeImageBase64(): QImageReader → 超 2048px 压缩 → PNG → Base64
  3. 构造 JSON body:
     {
       model, messages: [
         { role: "system", content: "..." },
         { role: "user", content: [
           { type: "image_url", image_url: { url: "data:image/png;base64,..." } },
           { type: "text", text: "用户问题" }
         ]}
       ], max_tokens: 1000
     }
  4. QNetworkRequest POST，Authorization: Bearer {key}

错误处理：
  - 401 → "API Key 无效"
  - 429 → "请求过于频繁"
  - 网络错误 → errorString()
  - JSON 解析失败 → "解析响应失败"

信号：
  responseReceived(text) → AiChatDialog 追加 AI 气泡
  requestFailed(error)   → AiChatDialog 追加错误气泡
```

### 7.6 AiChatDialog（widget 层）

```
QDialog，左右分栏（QSplitter）。

左侧：QLabel 显示图片缩略图（260px 宽，等高比缩放）
右侧：
  ├── QTextEdit（只读，聊天历史）
  │   └── appendMessage() 渲染 HTML 表格气泡
  └── QLineEdit + [发送] 按钮

气泡样式（appendMessage 按 sender 角色选择）：
  - "我"   → 蓝色气泡 #0078d4 白字，右对齐，圆角 12px
  - "AI 助手" → 白色气泡 #fff 黑字灰边框，左对齐
  - "系统"   → 黄色气泡 #fff3cd 橙边框，左对齐

HTML 结构：
  <table width="100%">
    <tr><td align="left|right">
      <span>发送者名称</span>
    </td></tr>
    <tr><td align="left|right">
      <div style="inline-block; max-width:78%; border-radius; padding">
        消息文本
      </div>
    </td></tr>
  </table>
```

### 7.7 配置持久化

```
SystemSettings:
  aiEndpoint = "https://api.openai.com/v1/chat/completions"  // 默认
  aiApiKey   = ""                                             // 空
  aiModel    = "gpt-4o"                                       // 默认

YAML 键名：ai_endpoint / ai_api_key / ai_model

CaptureWindow 成员：m_aiEndpoint / m_aiApiKey / m_aiModel
  - loadCapturePreferences() 从 SystemSettings 读取
  - saveCapturePreferences() 写回 SystemSettings
  - openCaptureSettingsDialog() 中 QLineEdit（API Key 为 Password 模式）

API Key 以明文存储于 YAML，建议用户妥善保管配置文件。
```

### 7.8 线程模型

```
全部操作在主线程（Qt 事件循环）：
  - 缩略图生成：QTimer 分批加载（每批 20 张，processEvents 释放 UI）
  - AI 网络请求：QNetworkAccessManager 异步（非阻塞）
  - 文件监听：QFileSystemWatcher 信号驱动（Qt 事件循环内）
```

---

## 八、用户手册

### 8.1 打开图池

- **方式一**：点击主窗口底部栏「图池」按钮
- **方式二**：菜单栏「视图 → 图池」

图池窗口可独立拖动、最小化、调整大小。关闭按钮（X）仅隐藏窗口，不会丢失浏览状态。

### 8.2 浏览与管理截图

| 操作 | 方式 |
|------|------|
| 查看截图 | 启动后自动加载截图目录下所有 PNG/JPG/BMP/GIF/WEBP |
| 排序 | 工具栏下拉框：日期/名称/大小，升序/降序 |
| 刷新 | 点击工具栏 [刷新] 按钮 |
| 查看大图 | 双击缩略图，弹出预览窗口 |
| 上一张/下一张 | 预览窗口中 ← → 方向键 |
| 缩放 | + 放大 / - 缩小 / 0 还原 |
| 关闭预览 | Esc 或关闭按钮 |

### 8.3 右键菜单

| 菜单项 | 功能 |
|--------|------|
| 打开 | 调用系统默认图片查看器打开原图 |
| 打开所在文件夹 | 在资源管理器中定位该文件 |
| 复制图片 | 将原图复制到剪贴板 |
| 复制图片路径 | 将文件完整路径复制到剪贴板 |
| 重命名 | 弹出对话框输入新名称 |
| 删除 | 确认后删除文件（不可恢复） |
| AI 识别 | 打开 AI 对话窗口分析该图片 |

### 8.4 使用 AI 识别

**第一步：配置**

1. 点击主窗口底部栏「设置」按钮
2. 滚动到「AI 识别配置」区域
3. 填写 API 地址、API Key、模型名称
4. 点击 OK 保存

配置示例（详见第六章，务必选择支持图片的模型）：

| 服务商 | API 地址 | 模型 | 图片 |
|--------|---------|------|:--:|
| 智谱（推荐） | `https://open.bigmodel.cn/api/paas/v4/chat/completions` | `glm-4v-flash`（免费） | ✅ |
| 通义千问 | `https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions` | `qwen-vl-plus` | ✅ |
| 本地 Ollama | `http://localhost:11434/v1/chat/completions` | `llava:13b` | ✅ |
| DeepSeek | — | — | ❌ 不支持 |

> 每个服务商需单独申请 API Key，填入设置页面对应字段。Ollama 本地无需 Key，留空即可。
> 打开 AI 对话窗后会自动发送"请分析一下图片内容"，后续问题可手动输入。

**第二步：使用**

1. 在图池中右键点击一张图片
2. 选择「AI 识别」
3. 弹出对话窗口，左侧显示图片，右侧 AI 发送"关于这张图片，我能为你做些什么？"
4. 在底部输入框键入问题（如"这张截图中有多少个人？"、"提取图中的所有文字"）
5. 按回车或点击 [发送]
6. AI 回复以气泡形式显示在对话区

**注意**：
- 图片超过 2048px 会自动压缩后发送
- 未配置 API Key 时会提示"未配置 API Key"
- API Key 无效（401）会提示检查设置
- DeepSeek 等纯文本模型不支持图片识别

---

## 九、文件清单

```
新增文件:
  common/include/fplayer/common/imagepool/imagemetadata.h
  service/include/fplayer/service/imagepoolservice.h
  service/src/imagepoolservice.cpp
  service/include/fplayer/service/aiservice.h
  service/src/aiservice.cpp
  widget/include/fplayer/widget/imagepoolsidebar.h
  widget/src/imagepoolsidebar.cpp
  widget/include/fplayer/widget/imageviewerdialog.h
  widget/src/imageviewerdialog.cpp
  widget/include/fplayer/widget/aichatdialog.h
  widget/src/aichatdialog.cpp

修改文件:
  widget/uis/capturewindow.ui                       ← 新增 btnImagePool
  widget/include/fplayer/widget/capturewindow.h      ← 新增成员/信号 + 配色成员
  widget/src/capturewindow.cpp                       ← 图池、菜单、AI 对话框、拉流预览图池按钮、配色 UI
  widget/include/fplayer/widget/imagepoolsidebar.h   ← setToolbarColor、closeEvent
  widget/src/imagepoolsidebar.cpp                    ← 等比缩略图、工具栏配色
  widget/include/fplayer/widget/imageviewerdialog.h  ← showEvent、fitToWindow
  widget/src/imageviewerdialog.cpp                   ← 等比缩放显示
  service/include/fplayer/service/systemsettingsrepository.h ← AI + 配色字段
  service/src/systemsettingsrepository.cpp           ← AI + 配色字段读写
  service/include/fplayer/service/aiservice.h        ← AiConfig 配色字段
  service/CMakeLists.txt                             ← 新增 Qt6::Network 依赖
```

---

## 十、构建注意事项

1. 新增了 `Q_OBJECT` 类（`ImagePoolService`、`AiService`、`ImagePoolSidebar`、`ImageViewerDialog`、`AiChatDialog`），首次构建需 **Reload CMake Project** 让 AUTOMOC 扫描
2. `service/CMakeLists.txt` 已添加 `Qt6::Network`（`AiService` 依赖 `QNetworkAccessManager`）
3. 所有 `.cpp`/`.h` 通过 `GLOB_RECURSE` 自动发现，无需手动添加
