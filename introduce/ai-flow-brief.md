# FPlayer Desktop — AI 图像分析流程

## 1. 架构

AI 模块由 `AiService` 与 `AiChatDialog`/`ChatBubbleWidget` 构成。`AiService` 封装 OpenAI 兼容 HTTP 客户端，通过 `QNetworkAccessManager` 异步通信；`ChatBubbleWidget` 以 `QPainter` 自绘气泡，QTimer 驱动打字机效果逐字追加文本。API 配置（endpoint / key / model）经 `SystemSettingsRepository` 持久化至 YAML。

## 2. 请求与响应

用户输入文本及图片后，图片经 `QImage → Base64` 编码，以 `data:image/{fmt};base64,...` 格式嵌入请求体 content 数组。AiService 构建标准 Chat Completions 请求 (`POST /v1/chat/completions`, `stream: true`)，通过 `QNetworkReply::readyRead` 信号增量读取 SSE 数据帧，逐行匹配 `data:` 前缀，解析 JSON 提取 `choices[0].delta.content`，以 `tokenReceived(delta)` 信号推送 UI。收到 `data: [DONE]` 后触发 `streamFinished()`。兼容 OpenAI / Azure OpenAI / vLLM 等端点。

## 3. 时序概要

`AiChatDialog → AiService::analyze() → HTTP POST → SSE stream → tokenReceived() → ChatBubbleWidget 追加字符 → streamFinished() → 完成气泡`。错误响应以红色气泡展示，所有网络 I/O 基于 Qt 事件循环异步执行，不创建额外线程。
