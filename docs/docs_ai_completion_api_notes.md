# AI Completion API 兼容性验证说明

**记录日期：** 2026-08-26  
**适用版本：** Vellum

本轮真实兼容性冒烟验证仅使用一次用户明确授权的短请求；测试密钥仅存在于权限受限临时文件中，执行后已删除。本文件不包含任何密钥、请求正文、文档内容或服务响应文本。

DeepSeek 的官方 API 文档将 OpenAI 兼容基础地址列为 `https://api.deepseek.com`，并将 `deepseek-v4-flash` 列为当前文本模型标识；Chat Completions 端点为 `/chat/completions`。因此，Vellum 的 AI Completion 扩展可使用完整地址 `https://api.deepseek.com/chat/completions` 以及模型名 `deepseek-v4-flash` 进行配置。[1]

官方思考模式文档说明：OpenAI 格式请求可通过 `"thinking": {"type": "enabled/disabled"}` 控制思考模式，且默认开启思考模式。Vellum 的内联补全只需短文本候选，因此在请求中显式发送 `"thinking": {"type": "disabled"}`，以减少不必要的推理延迟；该字段应由 OpenAI 兼容服务按自身能力处理。[2]

| 验证项目 | 结果 | 限制 |
| --- | --- | --- |
| `deepseek-v4-flash` 模型名 | 通过 | 仅验证一次短 Chat Completions 请求。 |
| OpenAI 兼容 Chat Completions 解析 | 通过 | 只验证 Vellum 所需的首个 `choices[].message.content` 候选路径。 |
| 非思考模式字段 | 通过 | 不代表所有第三方 OpenAI 兼容服务都支持该扩展字段。 |
| 密钥处理 | 通过 | 临时 0600 文件；未写入源代码、测试输出或发行物。 |

## References

[1] [DeepSeek API Docs — Your First API Call](https://api-docs.deepseek.com/)  
[2] [DeepSeek API Docs — Thinking Mode](https://api-docs.deepseek.com/guides/thinking_mode/)  
[3] [DeepSeek API Docs — Models & Pricing](https://api-docs.deepseek.com/quick_start/pricing/)

# 真实 AI 补全产品调研与 Vellum 对齐情况

**更新日期：** 2026-08-28

调研对象：GitHub Copilot（含 copilot.vim 客户端源码）、Continue（含其公开文档与 post-processing 实现说明）、Fitten Code、CodeGemma 提示词规范等。主流内联补全的共同设计如下，Vellum 已在本轮按可行性对齐：

| 真实补全做法 | 说明 | Vellum 现状 |
| --- | --- | --- |
| 幽灵文本 | 灰色内联建议显示在光标处；`Tab` 接受整段，`Escape` 取消，继续输入自动作废 | 已有；继续输入会清空候选并按新上下文重新请求 |
| 多行候选 | Copilot/Continue 的候选是多行文本：首行显示在光标后，后续行显示在下方虚拟行；`Tab` 接受完整多行补全 | 新增：候选保留完整多行文本；宿主 overlay 为单行，预览仅显示首行，但 `Tab` 接受完整多行补全（历史实现只保留第一行，多行被丢弃） |
| 接受时后缀去重 | Continue 的 SuffixOverlap：补全结尾与光标后已有文本开头重叠的部分会被裁掉，避免接受后把已有内容重复一遍 | 新增：接受文本先做最大后缀重叠裁剪，再插入 |
| 缩进对齐 | copilot.vim 的 SuggestionTextWithAdjustments：当前行只输入了空白时，裁掉补全开头与已输入空白重复的缩进；刚回车时空行保留补全自带的缩进 | 新增：同规则实现 |
| 输入即触发 + 防抖 | 停止输入约 150–350 ms 后自动请求（Continue 默认 350 ms、copilot.vim 默认 45 ms），快速输入期间取消前一个请求 | 防抖由约 1000 ms 调整为 350 ms（Continue 默认值，在响应速度与 token 消耗间折中）；手动 `Ctrl+Shift+Space` 仍可用；设置页可关闭自动 |
| 前缀 + 后缀（FIM） | 同时发送光标前/后的代码，用 FIM 格式让模型只补中间；生产集成通常取 100–200 token 后缀 | 已有：请求携带前缀（8000 字符内）与后缀（2000 字符内），以 `<fim_prefix>/<fim_suffix>/<fim_middle>` 标记组织聊天提示词 |
| 流式（SSE） | 大多数工具用 SSE 流式渲染幽灵文本，延迟更可控 | 新增：请求携带 `"stream": true`；`text/event-stream` 响应按 SSE 增量解析，幽灵文本随 token 到达渐进更新；非流式兼容服务（忽略 stream 字段、返回普通 JSON）自动回退解析 |
| 请求取消 | 用户继续输入时取消在途请求，避免返回过期建议 | 已有：在途请求通过代际令牌 + `soup_session_abort()` 作废，旧回调只释放 I/O；拒绝（Escape/继续输入）后 2 秒内迟到的流式响应不再弹回 |
| 端点归一化 | 客户端把基础地址自动补成 `/chat/completions`；裸主机（如 DeepSeek 的 `https://api.deepseek.com`）也能直接用 | 已有：裸主机与 `/v1` 地址自动补齐，完整 URL 原样使用 |
| 静默失败 | 自动补全的网络/解析错误不打断输入（仅手动请求提示） | 已有：自动请求错误不弹 Toast；手动请求仍提示 |

## 已知边界

- DeepSeek 官方 API 把 OpenAI 兼容基础地址列为 `https://api.deepseek.com`（无 `/v1` 路径段），补全端点为 `/chat/completions`；因此**基础地址和完整端点都可在 Vellum 中直接使用**。
- 自动补全只在光标前文本非空时触发，且与上次请求上下文相同时去重，避免重复消耗额度。
- 第三方 OpenAI 兼容服务不保证支持 `thinking` 扩展字段；Vellum 显式发送 `{"thinking":{"type":"disabled"}}`，不支持的实现会按自身策略忽略。
- 流式请求仍以 `max_tokens=160` 与 256 KiB 原始缓冲上限保护内存与额度；超限视为服务端异常并中断。
- 多行候选的预览受宿主单行 overlay 限制只显示首行；补全以空行开头时，预览会在找到首条非空行后显示，`Tab` 接受的仍是完整多行文本。
- 文档类型设置：全部大类（代码/Web/配置/文本/其他，归类为粗略约定，见 `ai_completion_language_category`）以复选框块放在最上方，分隔线之下是原生 GTK4 `GtkColumnView` 三列表格（方形复选框、文件类型、备注（`globs` 扩展名或 MIME 类型），按名称排序）。大类开关一键启停整类（部分勾选显示三态），单行切换同步大类状态。`disabled-languages` 持久化未勾选的 id，缺失即全部启用。语言未知的文档（纯文本）自动补全仍不触发，手动请求保持可用。若系统未安装语言定义，设置页会给出提示而非空列表。
