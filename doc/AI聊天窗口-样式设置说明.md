# AI 聊天窗口样式设置说明

> 当前实现方案：**方案 B — 自定义 QWidget + QPainter 自绘 + QTextDocument Markdown 渲染**。
> 方案对比见 [AI聊天窗口-显示方案对比.md](AI聊天窗口-显示方案对比.md)。

## 涉及文件

| 文件 | 作用 |
|------|------|
| [service/include/fplayer/service/aiservice.h](../service/include/fplayer/service/aiservice.h#L11-L23) | `AiConfig` 结构体：颜色/字体配置项 |
| [service/src/aiservice.cpp](../service/src/aiservice.cpp) | `AiService` 实现：SSE 流解析、错误响应 JSON 提取 |
| [widget/include/fplayer/widget/aichatdialog.h](../widget/include/fplayer/widget/aichatdialog.h) | `AiChatDialog` 类声明：聊天窗口主控件、打字机定时器 |
| [widget/src/aichatdialog.cpp](../widget/src/aichatdialog.cpp) | `AiChatDialog` 实现：布局管理、打字机逐字输出、消息事件 |
| [widget/include/fplayer/widget/chatbubblewidget.h](../widget/include/fplayer/widget/chatbubblewidget.h) | `ChatBubbleWidget` 声明：单个气泡绘制控件、QTextDocument 成员 |
| [widget/src/chatbubblewidget.cpp](../widget/src/chatbubblewidget.cpp) | `ChatBubbleWidget` 实现：QPainter 气泡 + QTextDocument Markdown 渲染 |

---

## 一、可配置项 — `AiConfig` 结构体

定义位置：[service/include/fplayer/service/aiservice.h:11-23](../service/include/fplayer/service/aiservice.h#L11-L23)

```cpp
struct AiConfig {
    QString endpoint;          // API 地址
    QString apiKey;            // API 密钥
    QString model;             // 模型名称，默认 "gpt-4o"
    QString userBubbleColor;   // 用户气泡背景色，默认 "#2563eb"（蓝色）
    QString aiBubbleColor;     // AI 气泡背景色，默认 "#ffffff"（白色）
    QString chatBgColor;       // 聊天区背景色，默认 "#f3f4f6"（浅灰）
    QString fontFamily;        // 字体，空字符串表示使用系统默认
    int     fontSize;          // 字号，默认 13
    QString aiTextColor;       // AI 气泡文字颜色，默认 "#374151"（深灰）
    QString userTextColor;     // 用户气泡文字颜色，默认 "#ffffff"（白色）
    QString systemBubbleColor; // 系统气泡背景色，默认 "#fef3c7"（淡黄）
    QString systemTextColor;   // 系统气泡文字颜色，默认 "#92400e"（深棕）
    QString senderColor;       // 发送者名称颜色（用户/AI），默认 "#9ca3af"（灰）
    QString systemSenderColor; // 系统发送者名称颜色，默认 "#d97706"（橙）
};
```

---

## 二、控件样式表（QSS）

### 2.1 `QScrollArea`（聊天显示区）

**构造时初始样式** — [aichatdialog.cpp:38-40](../widget/src/aichatdialog.cpp#L38-L40)
```cpp
m_chatScroll->setStyleSheet(
    "QScrollArea { border: 1px solid #d1d5db; border-radius: 4px; "
    "background-color: #f3f4f6; }");
```

**`startChat()` / `reconfigure()` 中按配置覆写** — [aichatdialog.cpp:91-93](../widget/src/aichatdialog.cpp#L91-L93)
```cpp
m_chatScroll->setStyleSheet(
    "QScrollArea { border: 1px solid #d1d5db; border-radius: 4px; "
    "background-color: %1; }")
    .arg(m_colors.chatBgColor);
```

背景色由 `AiConfig::chatBgColor` 控制，通过 QSS 设置在 QScrollArea 上。

### 2.2 `QLineEdit`（输入框）

- 构造时：[aichatdialog.cpp:56](../widget/src/aichatdialog.cpp#L56)
- 覆写：[aichatdialog.cpp:95-97](../widget/src/aichatdialog.cpp#L95-L97)

```cpp
m_input->setStyleSheet(
    "QLineEdit { border: 1px solid #ccc; border-radius: 4px; "
    "padding: 6px; font-size: %1px; %2 }")
    .arg(m_colors.fontSize).arg(fontFamilyCss);
```

### 2.3 `QLabel`（图片预览区）

固定样式，不受 `AiConfig` 影响 — [aichatdialog.cpp:26](../widget/src/aichatdialog.cpp#L26)
```cpp
m_imageLabel->setStyleSheet(
    "QLabel { background-color: #e5e7eb; border-radius: 4px; }");
```

---

## 三、气泡绘制 — `ChatBubbleWidget`

定义位置：[chatbubblewidget.cpp](../widget/src/chatbubblewidget.cpp)

每个聊天气泡是一个独立的 `QWidget`：气泡背景用 `QPainter` 绘制，文字内容用 `QTextDocument` 渲染（支持 Markdown）。

### 3.1 控件层次结构

```text
QScrollArea (m_chatScroll)
  └── QWidget (m_chatContent)
       └── QVBoxLayout (m_chatLayout)
            ├── QHBoxLayout (消息行)
            │    ├── QSpacerItem (addStretch)      ← 用户消息：左侧弹簧挤到右边
            │    └── ChatBubbleWidget               ← QPainter + QTextDocument
            ├── QHBoxLayout (消息行)
            │    ├── ChatBubbleWidget               ← AI 消息：贴左
            │    └── QSpacerItem (addStretch)       ← 右侧弹簧
            ├── QHBoxLayout (消息行)
            │    ├── QSpacerItem (addStretch)
            │    ├── ChatBubbleWidget               ← 系统消息：居中
            │    └── QSpacerItem (addStretch)
            └── QSpacerItem (addStretch)            ← 底部弹簧（推气泡向上）
```

关键：[aichatdialog.cpp:appendBubble](../widget/src/aichatdialog.cpp)

> **滚动实现**：`QScrollArea` 使用 `setWidgetResizable(false)` + 手动管理 content widget 尺寸。
> 每次增/改消息后，`applyContentSize()` 对比 `m_chatLayout->sizeHint()` 和 viewport 尺寸，
> 若 layout 需求高度超过 viewport 则主动 resize content widget 撑出滚动条。
> 这样避免了 `setWidgetResizable(true)` 在动态内容下不主动撑高 widget 的问题。

### 3.2 绘制流程

`ChatBubbleWidget::paintEvent()` 分三步：

1. **发送者名称**：`QPainter::drawText()` 在 `m_senderRect` 中绘制
   - 用户/AI：`#9ca3af`（灰色）
   - 系统：`#d97706`（橙色）

2. **气泡背景**：`QPainterPath` 绘制自定义圆角矩形（每角不同半径）
   - 辅助函数 `roundedRectPath()` 用 `arcTo()` 手动构建路径

3. **消息文字（Markdown）**：`QTextDocument::documentLayout()->draw()` 渲染
   - 支持标题、粗体、斜体、行内代码、代码块、列表、链接等
   - 颜色通过 `PaintContext::palette` 传入（见 3.7 节）

### 3.3 三种消息类型的样式

| 属性 | 用户消息（"我"） | AI 消息 | 系统消息 |
|------|-----------------|---------|---------|
| 布局对齐 | `addStretch` + widget（贴右） | widget + `addStretch`（贴左） | stretch + widget + stretch（居中） |
| 气泡背景色 | `userBubbleColor` | `aiBubbleColor` | `systemBubbleColor` |
| 文字颜色 | `userTextColor` | `aiTextColor` | `systemTextColor` |
| 发送者名颜色 | `senderColor` | `senderColor` | `systemSenderColor` |
| 圆角 (TL/TR/BR/BL) | `12 / 4 / 12 / 12` | `4 / 12 / 12 / 12` | `8 / 8 / 8 / 8` |
| 发送者字号 | `fontSize - 2`（≥8px） | `fontSize - 2`（≥8px） | `fontSize - 2`（≥8px） |
| 正文字号 | `fontSize` | `fontSize` | `fontSize` |

### 3.4 圆角绘制

辅助函数 `roundedRectPath()` — [chatbubblewidget.cpp 静态函数](../widget/src/chatbubblewidget.cpp)

- **用户气泡** `(12, 4, 12, 12)`：右上角较平，模拟微信"发送"气泡的尖角
- **AI 气泡** `(4, 12, 12, 12)`：左上角较平，模拟微信"接收"气泡的尖角
- **系统气泡** `(8, 8, 8, 8)`：四角均匀圆角

### 3.5 气泡宽度控制 — `recalculateLayout()`

布局计算流程：

1. 通过 CSS `body { font-family: '...'; font-size: ...px; }` 设置文档字体
2. `m_doc->setMarkdown(text)` 解析 Markdown 内容
3. `m_doc->setTextWidth(maxTextWidth)` 设定最大宽度，触发文字折行
4. `m_doc->idealWidth()` 获取不折行的理想宽度
   - 若 idealWidth ≤ maxTextWidth → 气泡缩窄（shrink-to-fit）
   - 否则 → 气泡宽度 = maxTextWidth（文字自动折行）
5. `m_doc->size()` 获取折行后的文档高度

宽度更新时机：
- 窗口大小变化时，通过 `eventFilter` 监听 `QEvent::Resize`
- `AiChatDialog::updateAllMaxWidths()` 遍历所有气泡调用 `setMaxBubbleWidth(viewportWidth * 0.7)`

### 3.6 文本颜色机制（双重保险）

由于 `QTextDocument::setDefaultStyleSheet()` 的 CSS `color` 属性在 `setMarkdown()` 后不保证生效，颜色通过两层机制设置：

**第一层 — `recalculateLayout()` 中 QTextCursor**：
```cpp
m_doc->setMarkdown(displayText);
QTextCursor cursor(m_doc);
cursor.select(QTextCursor::Document);
QTextCharFormat fmt;
fmt.setForeground(textColor);     // 按 sender type 取 AiConfig 颜色
cursor.mergeCharFormat(fmt);
```

**第二层 — `paintEvent()` 中 PaintContext 兜底**：
```cpp
QAbstractTextDocumentLayout::PaintContext ctx;
ctx.palette.setColor(QPalette::Text, textColor);
m_doc->documentLayout()->draw(&painter, ctx);
```

两层中总有一层会生效，确保颜色始终跟随 `AiConfig` 配置。

### 3.7 Markdown 渲染

使用 Qt 6 内置的 `QTextDocument::setMarkdown()` 解析，支持：

- 标题（`#` `##` `###`）
- 粗体（`**text**`）、斜体（`*text*`）
- 行内代码（`` `code` ``）、代码块（` ``` `）
- 有序/无序列表
- 链接、引用块（`>`）
- 表格

> 注意：`setMarkdown()` 对 Qt ≥ 5.14 可用。本项目的 CSS 只设置字体，不设颜色（颜色走 QTextCursor + PaintContext）。

### 3.8 流式输出 & 打字机效果

**打字机定时器** — AiChatDialog 中用 `QTimer`（30ms 间隔）逐字吐出：

```
API 返回 chunk → m_pendingChars 缓冲区 → 定时器 tick → 取首字符 → appendToAiMessage()
```

关键成员（[aichatdialog.h](../widget/include/fplayer/widget/aichatdialog.h)）：
- `QTimer* m_typewriterTimer` — 30ms 间隔定时器
- `QString m_pendingChars` — 待输出字符队列
- `bool m_responseComplete` — API 响应是否已结束

**流式光标** — 在气泡内容末尾追加 `▌`：
```cpp
// recalculateLayout() 中：
if (m_streaming && m_text.isEmpty())
    displayText = "● ● ●";       // 等待状态
else if (m_streaming)
    displayText = m_text + " ▌"; // 输入光标
```

API 响应结束后不立即 `finishAiMessage()`，等缓冲区清空后再固化气泡：
```cpp
// onTypewriterTick() 中：
if (m_pendingChars.isEmpty()) {
    m_typewriterTimer->stop();
    if (m_responseComplete) {
        finishAiMessage();
        setInputEnabled(true);
    }
}
```

### 3.9 流式期间的输入行为

AI 回答时输入框保持可编辑（用户可以提前打字），但发送按钮禁用：
```cpp
void setInputEnabled(bool enabled) {
    m_input->setEnabled(true);        // 始终可输入
    m_btnSend->setEnabled(enabled);   // 流式时禁用
}
```
回车也不会发送 — `onSendClicked()` 有 `m_aiStreaming` 守卫。

### 3.10 窗口关闭安全

`AiChatDialog::closeEvent()` 在窗口关闭时同步停止所有异步操作：
- 停止打字机定时器
- 清空缓冲区和流式状态
- 断开 `AiService` 所有信号连接

`AiService::~AiService()` 析构时 abort 活跃的 `QNetworkReply`，防止回调访问已销毁对象导致程序卡死。

### 3.11 错误气泡提示

当 API 返回错误（如 400、401）时，`AiService::onReplyFinished()` 会解析响应体 JSON：

```cpp
// 提取 error.message 字段
const QJsonObject errObj = doc.object()["error"].toObject();
errorMsg = errObj["message"].toString();
emit requestFailed(tr("API 错误 (HTTP %1)：%2").arg(status).arg(errorMsg));
```

错误消息以**系统气泡**（黄色背景、居中）形式显示在聊天中。

---

## 四、样式生效流程

```text
用户调用 startChat(imagePath, config)
  │
  ├─ 1. 保存 config → m_colors
  ├─ 2. clearChat()：重建空白聊天区，重置打字机状态
  ├─ 3. 覆写 m_chatScroll QSS（背景色）
  ├─ 4. 覆写 m_input QSS（字体、字号）
  ├─ 5. appendBubble(AI, ...) → 创建 ChatBubbleWidget，插入布局
  │
  └─ 6. autoSend() 自动发送首轮分析请求
       │
       ├─ appendBubble(User, ...) → 创建用户气泡
       └─ beginAiMessage() → 创建流式气泡，重置打字机状态
            │
            ├─ onResponseChunk() → 追加到 m_pendingChars → 启动定时器
            ├─ onTypewriterTick() → 逐字取字符 → appendToAiMessage()
            └─ onResponseFinished() → m_responseComplete = true
                 └─ 缓冲区清空后 → finishAiMessage() + 恢复输入
```

用户手动发送消息走 `onSendClicked()`，流程相同。

当配置变更时调用 `reconfigure()`：

1. 更新 `m_chatScroll` / `m_input` QSS
2. 遍历所有 `ChatBubbleWidget` 调用 `setColors()` → `recalculateLayout()` → 重新应用 CSS + QTextCursor + PaintContext
3. `updateAllMaxWidths()` 更新宽度约束

---

## 五、常见样式调整

| 需求 | 修改位置 | 改什么 |
| ---- | -------- | ------ |
| 改用户气泡颜色 | `AiConfig::userBubbleColor` | 默认 `"#2563eb"` |
| 改 AI 气泡颜色 | `AiConfig::aiBubbleColor` | 默认 `"#ffffff"` |
| 改聊天背景色 | `AiConfig::chatBgColor` | 默认 `"#f3f4f6"` |
| 改字号 | `AiConfig::fontSize` | 默认 `13` |
| 改字体 | `AiConfig::fontFamily` | 默认空（系统默认） |
| 调气泡最大宽度 | `updateAllMaxWidths()` | 改 `0.7` 系数 — [aichatdialog.cpp](../widget/src/aichatdialog.cpp) |
| 调气泡圆角 | `paintEvent()` 中 `tl/tr/br/bl` | 每种类型有不同的圆角值 |
| 改系统气泡颜色 | `AiConfig::systemBubbleColor` | 默认 `"#fef3c7"` |
| 改系统文字颜色 | `AiConfig::systemTextColor` | 默认 `"#92400e"` |
| 改发送者名称颜色 | `AiConfig::senderColor` | 默认 `"#9ca3af"` |
| 改系统发送者颜色 | `AiConfig::systemSenderColor` | 默认 `"#d97706"` |
| 调发送者名称字号 | `recalculateLayout()` / `paintEvent()` | 改 `fontSize - 2` 的偏移量 |
| 调气泡内边距 | `recalculateLayout()` 顶部常量 | `paddingH = 12`, `paddingV = 8` |
| 改流式光标样式 | `recalculateLayout()` 中 displayText 拼接 | 修改 `▌` / `● ● ●` 字符串 |
| 改打字机速度 | `m_typewriterTimer` 初始化 | 改 `setInterval(30)` 毫秒数 |
