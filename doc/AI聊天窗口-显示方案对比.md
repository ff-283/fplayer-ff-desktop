# AI 聊天窗口 — 显示方案对比

## 当前方案：QTextEdit + HTML

聊天内容通过 `QTextEdit::setHtml()` 渲染，气泡由 `bubbleHtml()` 方法拼接 HTML 字符串生成。

### 优点
- 零额外依赖，Qt Widgets 自带
- MinGW/MSVC 均可构建
- 同步渲染，无加载延迟
- 包体积不变

### 缺点
- QTextEdit 的 Rich Text 引擎只支持 HTML4/CSS2 子集
- 不支持 flexbox、max-width、`display: inline-block`
- `border-radius` 在 `<table>` 元素上表现不稳定
- 表格列数不匹配会导致文字折行异常
- 复杂布局只能靠 `<table>` hack，维护困难

---

## 方案 A：QWebEngineView（Chromium 浏览器引擎）

用 `QWebEngineView` 替代 `QTextEdit`，HTML/CSS 直接写现代网页。

### 优点
- 完整 HTML5/CSS3/JS 支持（flexbox、max-width、动画等）
- 气泡布局一行 CSS 搞定：`align-self: flex-end` / `align-self: flex-start`
- `border-radius` 完美渲染
- 支持 CSS 动画（流式光标闪烁、消息滑入等）
- 后续可加 Markdown 渲染、代码高亮等

### 缺点
- 包体积增加约 120MB（`Qt6WebEngineCore.dll` 等）
- 运行时多一个独立 Chromium 进程，内存多占约 100MB
- MinGW 构建兼容性差，主要支持 MSVC
- 许可证为 LGPL v3（其他 Qt 模块为 LGPL v2.1）
- 内容更新是异步的（`loadFinished` 信号），流式更新需要 JS 桥接

### CMake 依赖变更
```cmake
find_package(Qt6 COMPONENTS ... WebEngineWidgets REQUIRED)
target_link_libraries(... Qt6::WebEngineWidgets)
```

---

## 方案 B：自定义 QWidget + QPainter 自绘（本次采用）

用 `QScrollArea` + 自定义 `ChatBubbleWidget`（QPainter 绘制圆角气泡 + 文字），外层用 `QHBoxLayout` + `addStretch()` 控制左右对齐。

### 优点
- 零额外依赖，包体积不变
- MinGW/MSVC 均可构建
- 完全控制渲染，无 HTML 兼容问题
- 内存占用极低
- 可精确实现 WeChat/QQ 风格圆角（不同角不同半径）

### 缺点
- 需要手动计算文字折行（QFontMetrics）
- 无法直接复用 HTML 模板
- 复杂排版（如 Markdown 渲染）工作量较大

### 架构设计
```
AiChatDialog
  └── QScrollArea (m_chatScroll)
       └── QWidget (m_chatContent)
            └── QVBoxLayout (m_chatLayout)
                 ├── QHBoxLayout (row)
                 │    ├── QSpacerItem (stretch)     ← 用户消息左侧弹簧
                 │    └── ChatBubbleWidget          ← 用户气泡贴右
                 ├── QHBoxLayout (row)
                 │    ├── ChatBubbleWidget          ← AI 气泡贴左
                 │    └── QSpacerItem (stretch)     ← AI 消息右侧弹簧
                 └── QHBoxLayout (row)
                      ├── QSpacerItem (stretch)
                      ├── ChatBubbleWidget          ← 系统消息居中
                      └── QSpacerItem (stretch)
```

### 耦合度设计
- `ChatBubbleWidget` 是纯绘制控件，不依赖 `AiChatDialog`，可独立复用
- `AiChatDialog` 通过操作 `ChatBubbleWidget` 列表管理消息，不感知绘制细节
- 若切换到方案 A，只需：移除 `ChatBubbleWidget` → 替换为 `QWebEngineView` → 改为注入 HTML
- `AiConfig` 结构体保持不变，两个方案共用同一套颜色配置

---

## 切换指南

从方案 B 切到方案 A 的步骤：

1. CMakeLists.txt 添加 `WebEngineWidgets`
2. 新建 `WebEngineChatView` 类，封装 `QWebEngineView` + HTML 模板
3. 修改 `AiChatDialog`：将 `QScrollArea` + `ChatBubbleWidget` 替换为 `WebEngineChatView`
4. `appendMessage()` 改为 `page()->runJavaScript("appendMessage(...)")`
5. 流式更新改为 JS 调用 `page()->runJavaScript("updateStream(...)")`
6. 删除 `ChatBubbleWidget` 相关代码
