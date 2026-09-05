/*
 * ai-completion-private.h
 * 同一插件内部共享的常量、类型与跨文件函数声明。
 * 宿主 ABI 见 mt-plugin.h，本文件只是插件自身的内部契约。
 */

#ifndef AI_COMPLETION_PRIVATE_H
#define AI_COMPLETION_PRIVATE_H

#include "mt-plugin.h"
#include "ai-code-summary.h"

#include <adwaita.h>
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>

/* ===== 常量 ===== */
#define AI_COMPLETION_GROUP "AI Completion"
#define AI_CONTEXT_LIMIT 8000
#define AI_SUFFIX_LIMIT 2000
/* 输入停顿后自动补全的防抖时长。Continue 默认 350 ms、copilot.vim 默认 45 ms；
 * 这里取 350 ms，在响应速度与 token 消耗之间折中。 */
#define AI_AUTO_DELAY_MILLISECONDS 350
/* 候选在屏幕上停留的最长时间；超时后自动消失，避免“鬼影长存”。 */
#define AI_CANDIDATE_TIMEOUT_SECONDS 15
/* 用户用其他文本拒绝候选后，这段时间内不再自动弹出补全。 */
#define AI_REJECT_SUPPRESS_MILLISECONDS 2000
/* 流式响应单次读取与原始缓冲上限；max_tokens 很小，超限说明服务端异常。 */
#define AI_STREAM_READ_SIZE 4096
#define AI_STREAM_BUFFER_LIMIT 262144
#define AI_MULTIFILE_LIMIT 16000

/* 文档类型的大类：表格按此分组，分隔行上显示类名，可整类开关。 */
typedef enum
{
    AI_LANGUAGE_CATEGORY_CODE,
    AI_LANGUAGE_CATEGORY_WEB,
    AI_LANGUAGE_CATEGORY_CONFIG,
    AI_LANGUAGE_CATEGORY_TEXT,
    AI_LANGUAGE_CATEGORY_OTHER,
    AI_LANGUAGE_CATEGORY_COUNT
} AiLanguageCategory;

/* 候选状态：展示中的补全及其请求时的前后文，用于接受/拒绝判断。 */
typedef struct _AiCandidate
{
    MtPluginHost *host;
    gchar *text;
    gchar *context;
    gchar *suffix;
} AiCandidate;

/* 配置对话框的控件引用：供保存按钮与释放回调使用。 */
typedef struct _AiConfigWidgets
{
    MtPluginHost *host;
    AdwEntryRow *endpoint_row;
    AdwEntryRow *model_row;
    AdwPasswordEntryRow *key_row;
    AdwSwitchRow *auto_row;
    AdwSwitchRow *include_row;
    AdwComboRow *context_row;
    AdwSwitchRow *save_ctx_row;
    AdwSwitchRow *auto_fix_row;
    AdwSpinRow *delay_row;
    GListStore *language_items;
    AiCodeSummaryConfigWidgets *summary_widgets;
    GtkWindow *window;
} AiConfigWidgets;

/* ===== 跨文件共享变量（在某个 .c 中定义，此处 extern 声明） ===== */
extern SoupSession *ai_session;
extern AiCandidate ai_candidate;
/* 每次请求或停用都会推进代际，过期回调绝不能再访问宿主。 */
extern guint ai_generation;
/* 输入停顿后自动补全的防抖源；停用插件时必须移除。 */
extern guint ai_auto_source_id;
/* 候选显示的自动过期计时器；任何清除候选的路径都必须移除它。 */
extern guint ai_candidate_timeout_source_id;
/* 用户拒绝候选的时刻（单调时钟微秒），用于抑制紧接着的自动补全。 */
extern gint64 ai_reject_until_us;
extern gboolean ai_auto_context_loaded;
extern gchar *ai_auto_context;
extern gboolean ai_request_in_flight;
/* 输入停顿后自动补全（无需快捷键）；由“偏好设置”中的开关控制并持久化。 */
extern gboolean ai_auto_enabled;
/* 停顿空闲后自动触发错误修复（红/绿 diff 预览，需 Tab 接受）；可在设置关闭。 */
extern gboolean ai_auto_fix_enabled;
/* 自动纠错空闲定时器（晚于补全触发，避免与行内幽灵冲突）。 */
extern guint ai_auto_fix_source_id;
/* 缓存“禁用补全的文档类型”集合：插件激活与设置保存时从 ini 重建。 */
extern GHashTable *ai_disabled_languages;
/* 自动请求序号：用于空结果只重试一次（关闭总结再试）。 */
extern guint ai_req_seq;
/* 单次自动重试时临时关闭总结。 */
extern gboolean ai_force_no_summary;

/* ===== ai-completion-settings.c ===== */
gchar *ai_completion_config_path(void);
GKeyFile *ai_completion_load_settings(void);
gchar *ai_completion_get_setting(GKeyFile *settings, const gchar *key);
gchar *ai_completion_normalize_endpoint(const gchar *endpoint);
gboolean ai_completion_get_include_summary(GKeyFile *settings);
gboolean ai_completion_get_auto_enabled(GKeyFile *settings);
void ai_completion_set_auto_enabled(gboolean enabled);
GHashTable *ai_completion_parse_disabled_languages(const gchar *value);
gchar *ai_completion_join_disabled_languages(GHashTable *disabled);
void ai_completion_languages_set(GHashTable *disabled);
void ai_completion_languages_load(GKeyFile *settings);
gboolean ai_completion_document_allowed(MtPluginHost *host, gboolean automatic);
GHashTable *ai_completion_disabled_from_items(GListStore *items);
gboolean ai_completion_save_settings(const gchar *endpoint,
                                     const gchar *model,
                                     const gchar *api_key,
                                     gboolean auto_enabled,
                                     GListStore *language_items,
                                     AiCodeSummaryConfigWidgets *summary_widgets,
                                     GError **error);

/* ===== ai-completion-body.c ===== */
gchar *ai_completion_trim_context(const gchar *text, glong limit);
gchar *ai_completion_build_body(const gchar *model, const gchar *prefix,
                                const gchar *suffix, const gchar *summary,
                                const gchar *multifile);
gchar *ai_completion_find_project_root(const gchar *file_path);
void ai_completion_scan_project(const gchar *root, GString *ctx);
gchar *ai_completion_gather_context(MtPluginHost *host);
gchar *ai_completion_extract_choice_content(JsonObject *choice);
gchar *ai_completion_extract_content(const gchar *response, gsize length,
                                     GError **error);
gchar *ai_completion_error_detail_data(const gchar *data, gsize length);
gchar *ai_completion_prepare_inline_candidate(const gchar *completion);
gchar *ai_completion_outdent(const gchar *completion, const gchar *context);
gchar *ai_completion_trim_suffix_overlap(const gchar *completion,
                                         const gchar *suffix);
gchar *ai_completion_adjust_candidate(const gchar *completion,
                                      const gchar *context,
                                      const gchar *suffix);
void ai_completion_clear_candidate(void);
void ai_completion_reject_suppress(void);
void ai_completion_auto_cancel(void);
gboolean ai_completion_candidate_timeout(gpointer user_data);

/* ===== ai-completion-stream.c ===== */
void ai_completion_request_start(MtPluginHost *host, gboolean automatic);
gboolean ai_completion_handle_key(MtPluginHost *host,
                                   guint keyval,
                                   guint keycode,
                                   guint state,
                                   gpointer user_data);
void ai_completion_activate_action(GSimpleAction *action, GVariant *parameter, gpointer user_data);

/* ===== ai-completion-config.c ===== */
void ai_completion_config_widgets_free(AiConfigWidgets *widgets);
void ai_completion_config_save_clicked(GtkButton *button, gpointer user_data);
gboolean ai_completion_pref_auto_get(gpointer user_data);
void ai_completion_pref_auto_set(gboolean value, gpointer user_data);

#endif