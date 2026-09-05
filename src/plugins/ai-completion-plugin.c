/*
 * ai-completion-plugin.c
 * 通用 OpenAI 兼容 AI 补全扩展。密钥仅写入当前用户 0600 权限的本地配置文件。
 *
 * 行为对齐真实补全工具（copilot.vim / Continue）：
 * - SSE 流式输出：候选文本随 token 到达渐进显示，而不是等整段返回；
 * - 多行候选：预览仅显示首行（宿主 overlay 为单行），Tab 接受完整多行补全；
 * - 接受时裁掉补全结尾与光标后已存在文本重叠的部分（Continue 的 SuffixOverlap），
 *   避免把文件里已有的内容重复一遍；
 * - 当前行只输入了空白时，裁掉补全开头与已输入空白重复的缩进（copilot.vim
 *   的 SuggestionTextWithAdjustments）；刚回车时空行保留补全自带的缩进；
 * - 输入停顿约 350 ms 后自动请求（Continue 默认值），期间继续输入会取消旧请求。
 *
 * 保留的独创设计：AI 代码总结（全文摘要随请求复用）、手动快捷键、费用警告、
 * 仅代码文档自动触发、拒绝后短暂抑制与端点归一化。
 *
 * 本文件只保留插件 ABI（入口函数）与配置页面组装；具体功能拆分到：
 * - ai-completion-settings.c   配置读写与端点归一化
 * - ai-completion-language.c   文档类型表格
 * - ai-completion-config.c     配置对话框控件与保存
 * - ai-completion-body.c       请求正文、上下文、候选调整
 * - ai-completion-stream.c     SSE 流式输出与请求生命周期
 */

#include "mt-plugin.h"
#include "ai-code-summary.h"
#include "ai-completion-private.h"
#include "ai-completion-features.h"
#include "ai-completion-language.h"

#include <adwaita.h>
#include <glib/gi18n.h>
#include <string.h>

static const MtPluginInfo ai_completion_plugin_info = {
    MT_PLUGIN_API_VERSION,
    "io.github.vellum.ai-completion",
    "AI Completion",
    "Complete text through a user-configured OpenAI-compatible API",
    "0.3.0"
};

G_MODULE_EXPORT const MtPluginInfo *
mt_plugin_query(void)
{
    return &ai_completion_plugin_info;
}

/* —— 跨编译单元共享的全局状态（private.h 中 extern 声明，此处唯一持有定义） —— */
SoupSession *ai_session = NULL;
AiCandidate ai_candidate = { 0 };
guint ai_generation = 0;
guint ai_auto_source_id = 0;
guint ai_candidate_timeout_source_id = 0;
gint64 ai_reject_until_us = 0;
gboolean ai_auto_context_loaded = FALSE;
gchar *ai_auto_context = NULL;
gboolean ai_request_in_flight = FALSE;
gboolean ai_auto_enabled = FALSE;
gboolean ai_auto_fix_enabled = FALSE;
guint ai_auto_fix_source_id = 0;
GHashTable *ai_disabled_languages = NULL;
guint ai_req_seq = 0;
gboolean ai_force_no_summary = FALSE;

G_MODULE_EXPORT gboolean
mt_plugin_activate(MtPluginHost *host, GError **error)
{
    static const gchar *accelerators[] = { "<Primary><Shift>space", NULL };

    if (host->show_inline_completion == NULL || host->clear_inline_completion == NULL ||
        host->add_key_handler == NULL)
    {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_NOT_SUPPORTED,
                    "Vellum host does not provide inline completion services");
        return FALSE;
    }

    if (ai_session == NULL)
    {
        ai_session = soup_session_new();
        g_object_set(ai_session, "timeout", 30, NULL);
    }
    ai_auto_context_loaded = FALSE;
    g_clear_pointer(&ai_auto_context, g_free);
    ai_request_in_flight = FALSE;
    {
        GKeyFile *settings;

        settings = ai_completion_load_settings();
        ai_auto_enabled = ai_completion_get_auto_enabled(settings);
        {
            gboolean fix_enabled = TRUE;

            if (g_key_file_has_key(settings, AI_COMPLETION_GROUP, "auto-error-fix", NULL))
            {
                fix_enabled = g_key_file_get_boolean(settings,
                                                     AI_COMPLETION_GROUP,
                                                     "auto-error-fix",
                                                     NULL);
            }
            ai_auto_fix_enabled = fix_enabled;
        }
        {
            gint d = g_key_file_get_integer(settings, AI_COMPLETION_GROUP, "auto-delay", NULL);
            if (d <= 0) d = 200;
            ai_features_set_base_delay((guint)d);
        }
        ai_completion_languages_load(settings);
        g_key_file_unref(settings);
    }

    if (!host->add_action(host,
                          "ai-complete",
                          ai_completion_activate_action,
                          host,
                          NULL))
    {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_EXISTS,
                    "The ai-complete action is already registered");
        return FALSE;
    }

    host->set_accelerators(host, "app.ai-complete", accelerators);
    host->add_key_handler(host, ai_completion_handle_key, NULL, NULL);
    ai_code_summary_activate(host);
    ai_features_init(host);

    if (host->add_preference_switch != NULL)
    {
        host->add_preference_switch(host,
                                    _("AI Completion"),
                                    _("Automatic AI completion"),
                                    _("Wait for a short pause after typing, then request a completion without pressing a shortcut."),
                                    ai_completion_pref_auto_get,
                                    ai_completion_pref_auto_set,
                                    NULL,
                                    NULL);
    }

    return TRUE;
}

G_MODULE_EXPORT void
mt_plugin_deactivate(MtPluginHost *host)
{
    (void)host;

    ai_completion_auto_cancel();
    ai_completion_clear_candidate();
    ai_code_summary_deactivate();
    ai_features_shutdown();
    g_clear_pointer(&ai_auto_context, g_free);
    ai_auto_context_loaded = FALSE;
    ai_request_in_flight = FALSE;
    ai_generation++;
    ai_completion_languages_set(NULL);
    if (ai_session != NULL)
    {
        soup_session_abort(ai_session);
        g_clear_object(&ai_session);
    }
}

G_MODULE_EXPORT void
mt_plugin_configure(MtPluginHost *host, gpointer parent_window)
{
    GKeyFile *settings;
    gchar *endpoint;
    gchar *model;
    gchar *api_key;
    gboolean auto_enabled;
    AdwPreferencesWindow *window;
    AdwPreferencesPage *page;
    AdwPreferencesGroup *group;
    AdwActionRow *save_row;
    GtkWidget *save_button;
    AiConfigWidgets *widgets;

    settings = ai_completion_load_settings();
    endpoint = ai_completion_get_setting(settings, "endpoint");
    model = ai_completion_get_setting(settings, "model");
    api_key = ai_completion_get_setting(settings, "api-key");
    auto_enabled = ai_completion_get_auto_enabled(settings);
    /* settings 在页面全部组构建完成后再释放：文档类型组还要读它。 */

    window = ADW_PREFERENCES_WINDOW(adw_preferences_window_new());
    gtk_window_set_title(GTK_WINDOW(window), _("AI Completion Settings"));
    gtk_window_set_transient_for(GTK_WINDOW(window), GTK_WINDOW(parent_window));
    gtk_window_set_modal(GTK_WINDOW(window), TRUE);
    page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(group, _("OpenAI-Compatible Service"));
    adw_preferences_group_set_description(group,
                                          _("Enter a full Chat Completions URL, an OpenAI-compatible URL ending in /v1, or a bare service host. Text around the cursor is sent to this service."));

    widgets = g_new0(AiConfigWidgets, 1);
    widgets->host = host;
    widgets->window = GTK_WINDOW(window);
    widgets->endpoint_row = ADW_ENTRY_ROW(adw_entry_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(widgets->endpoint_row), _("API Endpoint URL"));
    gtk_editable_set_text(GTK_EDITABLE(widgets->endpoint_row), endpoint);
    adw_preferences_group_add(group, GTK_WIDGET(widgets->endpoint_row));

    widgets->model_row = ADW_ENTRY_ROW(adw_entry_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(widgets->model_row), _("Model"));
    gtk_editable_set_text(GTK_EDITABLE(widgets->model_row), model);
    adw_preferences_group_add(group, GTK_WIDGET(widgets->model_row));

    widgets->key_row = ADW_PASSWORD_ENTRY_ROW(adw_password_entry_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(widgets->key_row), _("API Key"));
    gtk_editable_set_text(GTK_EDITABLE(widgets->key_row), api_key);
    adw_preferences_group_add(group, GTK_WIDGET(widgets->key_row));

    widgets->auto_row = ADW_SWITCH_ROW(adw_switch_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(widgets->auto_row),
                                  _("Suggest automatically while typing"));
    adw_action_row_set_subtitle(ADW_ACTION_ROW(widgets->auto_row),
                                _("Wait for a short pause after typing, then request a completion without pressing a shortcut."));
    adw_switch_row_set_active(widgets->auto_row, auto_enabled);
    adw_preferences_group_add(group, GTK_WIDGET(widgets->auto_row));

    widgets->auto_fix_row = ADW_SWITCH_ROW(adw_switch_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(widgets->auto_fix_row),
                                  _("Fix errors automatically while idle"));
    adw_action_row_set_subtitle(ADW_ACTION_ROW(widgets->auto_fix_row),
                                _("After you stop typing, scan the current line for errors and suggest a red/green fix. Tab to apply, Esc to dismiss."));
    {
        gboolean fix_enabled = TRUE;

        if (g_key_file_has_key(settings, AI_COMPLETION_GROUP, "auto-error-fix", NULL))
        {
            fix_enabled = g_key_file_get_boolean(settings,
                                                 AI_COMPLETION_GROUP,
                                                 "auto-error-fix",
                                                 NULL);
        }
        adw_switch_row_set_active(widgets->auto_fix_row, fix_enabled);
    }
    adw_preferences_group_add(group, GTK_WIDGET(widgets->auto_fix_row));

    widgets->delay_row = ADW_SPIN_ROW(adw_spin_row_new_with_range(80, 1500, 20));
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(widgets->delay_row),
                                  _("Completion sensitivity"));
    adw_action_row_set_subtitle(ADW_ACTION_ROW(widgets->delay_row),
                                _("Pause (ms) after typing before requesting a completion. Lower is more eager."));
    {
        gint d = g_key_file_get_integer(settings, AI_COMPLETION_GROUP, "auto-delay", NULL);
        if (d <= 0) d = 200;
        adw_spin_row_set_value(widgets->delay_row, (gdouble)d);
    }
    adw_preferences_group_add(group, GTK_WIDGET(widgets->delay_row));

    widgets->include_row = ADW_SWITCH_ROW(adw_switch_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(widgets->include_row),
                                  _("Include document summary in completions"));
    adw_action_row_set_subtitle(ADW_ACTION_ROW(widgets->include_row),
                                _("Send the AI-maintained summary of this file with each completion request."));
    adw_switch_row_set_active(widgets->include_row, ai_completion_get_include_summary(settings));
    adw_preferences_group_add(group, GTK_WIDGET(widgets->include_row));

    {
        GtkStringList *modes;
        gint context_mode;

        modes = gtk_string_list_new((const gchar * const[]){
            _("Current file only"),
            _("Open files"),
            _("Project directory"),
            NULL
        });
        widgets->context_row = ADW_COMBO_ROW(adw_combo_row_new());
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(widgets->context_row),
                                      _("Completion context"));
        adw_action_row_set_subtitle(ADW_ACTION_ROW(widgets->context_row),
                                    _("Which files to send as extra context: the current file, "
                                      "all open files, or the whole project directory."));
        adw_combo_row_set_model(widgets->context_row, G_LIST_MODEL(modes));
        context_mode = g_key_file_get_integer(settings, "AI Completion", "context-mode", NULL);
        if (context_mode != 1 && context_mode != 2)
        {
            context_mode = 0;
        }
        adw_combo_row_set_selected(widgets->context_row, context_mode);
        adw_preferences_group_add(group, GTK_WIDGET(widgets->context_row));

        widgets->save_ctx_row = ADW_SWITCH_ROW(adw_switch_row_new());
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(widgets->save_ctx_row),
                                      _("Cache context to a hidden file"));
        adw_action_row_set_subtitle(ADW_ACTION_ROW(widgets->save_ctx_row),
                                    _("When using project context, write the assembled context to "
                                      ".ai-context-cache in the project root."));
        adw_switch_row_set_active(widgets->save_ctx_row,
                                  g_key_file_get_boolean(settings,
                                                          "AI Completion",
                                                          "save-context-hidden",
                                                          NULL));
        adw_preferences_group_add(group, GTK_WIDGET(widgets->save_ctx_row));
    }

    {
        /* 保存设置固定在页面最顶部，长列表设置（文档类型）不再被保存行截断。 */
        AdwPreferencesGroup *save_group;

        save_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
        save_row = ADW_ACTION_ROW(adw_action_row_new());
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(save_row), _("Save AI Settings"));
        save_button = gtk_button_new_with_label(_("Save"));
        gtk_widget_set_valign(save_button, GTK_ALIGN_CENTER);
        adw_action_row_add_suffix(save_row, save_button);
        adw_preferences_group_add(save_group, GTK_WIDGET(save_row));
        g_signal_connect(save_button,
                         "clicked",
                         G_CALLBACK(ai_completion_config_save_clicked),
                         widgets);
        adw_preferences_page_add(page, save_group);
    }
    adw_preferences_page_add(page, group);


    {
        /* AI 自动总结设置组：位于 API 配置与文档类型之间。
         * 开启开关 + 修改行数间隔 + 提示说明。 */
        widgets->summary_widgets = ai_code_summary_add_config_group(page, settings);
    }

    {
        /* 本地统计分组：请求数 / 接受数 / 估算 token。 */
        ai_features_configure_add_stats(host, page);
    }

    {
        /* 文档类型表格：列出系统安装的全部 GtkSourceView 语言。
         * 第一列方形复选框（勾选=启用该格式），第二列文件类型，第三列备注
         * （扩展名或 MIME 类型）。全部使用原生 GTK4 组件。 */
        AdwPreferencesGroup *language_group;
        GListStore *language_items;

        language_group = ai_completion_build_language_group(settings, &language_items);
        widgets->language_items = language_items;
        adw_preferences_page_add(page, language_group);
    }

    g_object_set_data_full(G_OBJECT(window),
                           "vellum-ai-config-widgets",
                           widgets,
                           (GDestroyNotify)ai_completion_config_widgets_free);

    adw_preferences_window_add(window, page);
    gtk_window_present(GTK_WINDOW(window));

    g_key_file_unref(settings);
    g_free(endpoint);
    g_free(model);
    g_free(api_key);
}