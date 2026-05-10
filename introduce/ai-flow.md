# FPlayer Desktop — AI 图像分析流程

## 1. 概述

AI 模块由 `AiService`（业务层）与 `AiChatDialog` / `ChatBubbleWidget`（UI 层）协作完成。用户可在聊天界面输入文本并附加图片，系统将图片 Base64 编码后嵌入 OpenAI 兼容请求体，通过 HTTP POST 发送至配置的 API 端点，以 SSE (Server-Sent Events) 协议流式接收回复，最终以打字机效果逐字渲染至对话气泡。

---

## 2. 配置管理

`AiChatDialog` 启动时从 `SystemSettingsRepository`（YAML 持久化）读取三项配置：

| 配置项 | 说明 |
|--------|------|
| `apiEndpoint` | OpenAI 兼容 API 的 base URL，默认 `https://api.openai.com` |
| `apiKey` | Bearer Token 认证密钥 |
| `model` | 模型名称，如 `gpt-4o` / `gpt-4o-mini` |

用户可在设置界面修改以上配置，`Service::saveSystemSettings()` 负责持久化。

---

## 3. UI 交互层

### 3.1 AiChatDialog

- 继承 `QDialog`，顶部 QTextEdit 输入文本，支持拖拽/粘贴/文件对话框添加图片
- 发送后以 `ChatBubbleWidget` 分别渲染用户消息气泡（右对齐）与 AI 回复气泡（左对齐）
- SSE 流未完成前显示加载动画；流结束后恢复输入框可用状态

### 3.2 ChatBubbleWidget

- 自定义 `QWidget`，以 `QPainter` 自绘圆角气泡背景
- 内置 `QTimer`，每 30ms 从缓冲区取一个字符追加到 `QLabel`，产生打字机效果
- 支持 Markdown 基本语法渲染（加粗、代码块）

---

## 4. AiService 层

### 4.1 请求构建

```
POST {apiEndpoint}/v1/chat/completions
Content-Type: application/json
Authorization: Bearer {apiKey}

{
  "model": "gpt-4o",
  "stream": true,
  "messages": [
    {
      "role": "user",
      "content": [
        { "type": "text", "text": "用户输入文本" },
        { "type": "image_url", "image_url": { "url": "data:image/jpeg;base64,..." } }
      ]
    }
  ]
}
```

- 图片经 `QImage → QByteArray → Base64` 编码，前缀 `data:image/{format};base64,`
- 请求通过 `QNetworkAccessManager::post()` 异步发送，不阻塞 UI 线程

### 4.2 SSE 解析

`QNetworkReply::readyRead` 信号驱动增量读取。解析逻辑：

1. 从 reply 缓冲区逐行读取 UTF-8 文本
2. 匹配 `data: {...}` 行，跳过空行和 `data: [DONE]`
3. 解析内部 JSON，提取 `choices[0].delta.content` 字段
4. `emit tokenReceived(QString delta)` 通知 UI 追加文本
5. 收到 `[DONE]` 或 reply 触发 `finished` 信号时 `emit streamFinished()`

### 4.3 错误处理

- 网络错误（超时/拒绝连接）→ `emit streamError(QString message)`
- HTTP 非 200 响应 → 解析响应体中的 `error.message` 字段
- 错误信息在 UI 中以红色气泡展示

---

## 5. 完整调用时序

```
User                    AiChatDialog           AiService              OpenAI API
 |                           |                      |                      |
 |-- 输入文本 + 选择图片 --->|                      |                      |
 |                           |-- 调用 analyze() --->|                      |
 |                           |                      |-- HTTP POST -------->|
 |                           |                      |<-- SSE stream -------|
 |                           |<-- tokenReceived() ---|                      |
 |                           |-- 追加字符至气泡      |                      |
 |                           |<-- tokenReceived() ---|                      |
 |                           |-- ...逐 token 渲染    |                      |
 |                           |<-- streamFinished() --|                      |
 |                           |-- 完成气泡，启用输入  |                      |
 |<-- 查看完整回复           |                      |                      |
```

---

## 6. 设计要点

- **异步非阻塞**：所有网络 I/O 基于 Qt 事件循环，AiService 不创建额外线程
- **协议兼容**：请求格式兼容 OpenAI / Azure OpenAI / 本地 vLLM 等任意 OpenAI 兼容端点
- **UI 体验**：打字机效果降低感知延迟，逐 token 渲染使用户在长回复时无需等待完整响应
