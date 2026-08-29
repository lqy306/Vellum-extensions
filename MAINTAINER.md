# Vellum Extensions — Maintainer Notes

面向维护者的补充文档。用户文档见各插件的扩展设置页与仓库 `README.md`。

## 1. 插件 ABI（宿主契约）

所有插件只能使用 `src/mt-plugin.h` 中声明的宿主服务，且必须导出：

```c
const MtPluginInfo *mt_plugin_query(void);
gboolean            mt_plugin_activate(MtPluginHost *host, GError **error);
void                mt_plugin_deactivate(MtPluginHost *host);   /* 可选 */
void                mt_plugin_configure(MtPluginHost *host, gpointer parent_window); /* 可选 */
```

`MtPluginHost`（API 版本 `MT_PLUGIN_API_VERSION`，当前 2）以函数指针表形式提供：
编辑器读写（`get_current_text` / `get_text_before_cursor` / `get_text_after_cursor` /
`insert_text` / `get_current_file_path`）、语言识别（`get_is_code_document` /
`get_document_language_id`）、UI（`show_inline_completion` / `clear_inline_completion` /
`set_panel` / `hide_panel` / `show_toast` / `add_preference_switch`）、命令与按键
（`add_action` / `set_accelerators` / `add_key_handler` / `run_editor_command`）、
生命周期（`add_document_change_handler`）等。

**约定**：插件不得假设宿主内部类型，只能通过该表交互；新增宿主能力时同时升
`MT_PLUGIN_API_VERSION` 并在 `mt-plugin.h` 注释说明。

## 2. AI 补全插件文件划分

为保持单文件 ≤100KB、职责清晰，AI 补全相关代码拆分为：

| 文件 | 职责 |
|---|---|
| `ai-completion-plugin.c` | 核心补全流：FIM prompt、SSE 流式解析、ghost-text 展示与接受、触发调度、配置页 |
| `ai-completion-features.c` | 扩展特性：活动感知触发、多行预览面板、错误修复（diff）、本地统计 |
| `ai-code-summary.c/.h` | 可选的全文自动总结（按语言可选、按修改行数节流），为补全提供上下文 |
| `ai-diff.c/.h` | 行级 LCS diff（无外部依赖），供错误修复红/绿预览使用 |
| `ai-completion-private.h` | 同一插件内部（主文件 ↔ features）共享的少量辅助声明 |
| `ai-completion-features.h` | 主文件调用的“扩展特性”接口 |
| `mt-plugin.h` | 宿主 ABI（跨插件，不可随意改） |

> 结构准则：**单个源文件不超过 100KB**。新增能力优先放进 `ai-completion-features.c`
> 或独立文件，而非继续膨胀主文件。

## 3. 已实现特性

- **A. 活动感知触发**：`ai_completion_auto_schedule` 调用
  `ai_features_adaptive_delay()`（编辑越频繁延时越长，省 token）与
  `ai_features_cursor_safe()`（标识符/字符串/注释中间不自动弹出，手动仍可用）。
- **B. 多行补全预览**：宿主 `show_inline_completion` 现支持多行 overlay，直接在
   光标处半透明（opacity 0.6）显示完整候选，多行时折到下一行缩进处；接受/取消即清除。
- **C. 错误修复（diff）**：动作 `ai-fix`（`<Primary>e`）或空闲自动纠错。取当前全文
   交 LLM 修正，用 `ai_diff_inline_span()` 求首个替换片段的行内最小 span，宿主
   `show_inline_diff` 在原文处叠加红删除线旧文本 + 绿新文本；`Tab` 应用（`apply_inline_diff`）、
   `Esc` 取消（`clear_inline_diff`）。
- **D. 触发点启发 + 空结果重试**：`ai_features_cursor_safe` 把关自动触发位点；
  自动请求返回空且为首次时，`ai_completion_finish_stream` 会临时关闭总结再请求一次。
- **E. 可选择全文总结**：`ai-code-summary.c` 按语言/类型可选、按修改行数触发；
  主插件配置新增“Include document summary in completions”开关（ini `include-summary`）。
- **F. 本地统计**：请求数 / 接受数 / 估算 token（chars÷4）持久化于 ini 的
  `AI Stats` 组，并在扩展设置页展示，可一键重置。

## 4. 构建

```sh
make            # 构建全部插件到 build/<name>.so
make build/ai-completion-plugin.so   # 仅构建 AI 补全插件（含 features/diff）
make clean
```

依赖（开发包）：`gtk4 libadwaita-1 gtksourceview-5 libsoup-3.0 json-glib-1.0
gio-2.0 gmodule-2.0`（见 `Makefile` 与 `BUILDING.md`）。编译用 `cc -std=c11
-Wall -Wextra`，新增代码不应引入新的编译告警。

## 5. 配置存储

所有 AI 配置位于 `~/.config/vellum/ai-completion.ini`（权限 0600），分组：
`AI Completion`（endpoint/model/api-key/auto-complete/disabled-languages/
include-summary）、`AI Code Summary`、`AI Stats`。密钥仅存于本地该文件。
