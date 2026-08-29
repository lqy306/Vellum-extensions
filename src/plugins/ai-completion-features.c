/*
 * ai-completion-features.c
 * AI 补全插件的“扩展特性”模块，与主插件共享 ai-completion-private.h 的少量辅助：
 *  - 活动感知的自动触发节奏（参考 autoCodeCompletion 的自适应策略，改为成本感知）；
 *  - 多行补全预览面板（宿主 overlay 仅单行，用辅助面板展示完整候选）；
 *  - 错误修复：把当前全文交给 LLM 修正，以红/绿 diff 预览并应用；
 *  - 本地统计（请求数 / 接受数 / 估算 token）。
 *
 * 这些都不依赖参考插件驱动的 Copilot/Tabnine，而是直接复用我们自己的 LLM 调用。
 */

#include "ai-completion-features.h"
#include "ai-completion-private.h"
#include "ai-diff.h"

#include <adwaita.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#include <string.h>

/* —— 状态 —— */
static MtPluginHost *ai_features_host;
static SoupSession *ai_fix_session;
static GtkWidget *ai_panel;
static GtkWidget *ai_panel_scroll;
static GtkTextView *ai_panel_text;
static GtkLabel *ai_panel_hint;
static GtkWidget *ai_panel_actions;
/* 面板模式：0 无，1 补全预览，2 错误修复 diff。 */
static gint ai_panel_mode;

static gchar *ai_fix_original;
static gchar *ai_fix_fixed;
static gboolean ai_applying_fix;
/* 行内 diff 是否正在展示（供按键路由判断是否吞掉 Tab/Esc）。 */
static gboolean ai_fix_inline_visible = FALSE;

/* 活动监视：最近编辑时间戳与 3 秒窗口内的编辑次数。 */
static gint64 ai_last_edit_us;
static guint ai_recent_edits;

/* 本地统计。 */
static guint ai_stats_requests;
static guint ai_stats_accepted;
static guint ai_stats_chars;
static GtkLabel *ai_stats_req_label;
static GtkLabel *ai_stats_acc_label;
static GtkLabel *ai_stats_tok_label;

/* 仅本文件内部使用的前向声明。 */
static void ai_features_apply_fix(GtkButton *button, gpointer user_data);
static void ai_features_copy_fix(GtkButton *button, gpointer user_data);

/* —— 活动感知触发（A） —— */

gboolean
ai_features_cursor_safe(MtPluginHost *host)
{
    (void)host;
    /* 放开自动补全触发：注释、标识符、字符串中间都允许自动弹出，
     * 贴合 Copilot 式“边打边补 / 注释驱动生成”的灵敏体验。手动补全始终可用。 */
    return TRUE;
}

static guint ai_base_delay_ms = 200;

void
ai_features_set_base_delay(guint ms)
{
    ai_base_delay_ms = ms < 80 ? 80 : (ms > 2000 ? 2000 : ms);
}

guint
ai_features_adaptive_delay(void)
{
    /* 编辑越频繁延时略增，但更短更跟手；基础延时可在设置中调整，越小越灵敏。 */
    guint delay = ai_base_delay_ms;

    if (ai_recent_edits > 1)
    {
        delay += MIN(ai_recent_edits, 6) * 60;
    }
    return delay;
}

static void
ai_features_activity_changed(MtPluginHost *host, guint changed_lines, gpointer user_data)
{
    gint64 now;

    (void)host;
    (void)changed_lines;
    (void)user_data;
    now = g_get_monotonic_time();
    if (now - ai_last_edit_us < 3 * G_USEC_PER_SEC)
    {
        ai_recent_edits++;
    }
    else
    {
        ai_recent_edits = 1;
    }
    ai_last_edit_us = now;
}

/* —— 预览面板（B） —— */

static void
ai_features_panel_clear_actions(void)
{
    if (ai_panel_actions == NULL)
    {
        return;
    }
    while (gtk_widget_get_first_child(ai_panel_actions) != NULL)
    {
        gtk_widget_unparent(gtk_widget_get_first_child(ai_panel_actions));
    }
}

static void
ai_features_panel_init(MtPluginHost *host)
{
    /* 错误修复改为输入区行内红/绿 diff，不再使用侧边预览面板。 */
    (void)host;
}

void
ai_features_clear(void)
{
    if (ai_features_host != NULL && ai_features_host->clear_inline_diff != NULL)
    {
        ai_features_host->clear_inline_diff(ai_features_host);
    }
    ai_fix_inline_visible = FALSE;
    g_clear_pointer(&ai_fix_original, g_free);
    g_clear_pointer(&ai_fix_fixed, g_free);
    ai_panel_mode = 0;
}

void
ai_features_on_candidate(MtPluginHost *host, const gchar *text)
{
    /* 补全预览现由宿主行内幽灵文本承担，无需辅助面板。 */
    (void)host;
    (void)text;
}

/* —— 错误修复（C） —— */

gchar *
ai_features_build_fix_body(const gchar *model, const gchar *code, const gchar *language_id, const gchar *multifile)
{
    JsonBuilder *builder;
    JsonGenerator *generator;
    JsonNode *root;
    gchar *prompt;
    gchar *body;

    prompt = g_strdup_printf("The following is %s source. It may contain bugs or errors, "
                             "or it may be a comment describing desired behavior. "
                             "If it is a comment or description, generate the corresponding complete code. "
                             "If it is code, fix syntax errors, logic defects and potential exceptions. "
                             "Return ONLY the full resulting code, with no explanation, no Markdown, no fences. "
                             "Keep the same language and overall structure.\n\n"
                             "Related project files for context:\n%s\n\n%s",
                             language_id != NULL ? language_id : "source",
                             multifile != NULL ? multifile : "",
                             code);
    builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "model");
    json_builder_add_string_value(builder, model);
    json_builder_set_member_name(builder, "messages");
    json_builder_begin_array(builder);
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "role");
    json_builder_add_string_value(builder, "system");
    json_builder_set_member_name(builder, "content");
    json_builder_add_string_value(builder,
                                  "You are a careful code reviewer. You fix bugs and return "
                                  "the complete corrected file as raw code only.");
    json_builder_end_object(builder);
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "role");
    json_builder_add_string_value(builder, "user");
    json_builder_set_member_name(builder, "content");
    json_builder_add_string_value(builder, prompt);
    json_builder_end_object(builder);
    json_builder_end_array(builder);
    json_builder_set_member_name(builder, "temperature");
    json_builder_add_double_value(builder, 0.1);
    json_builder_set_member_name(builder, "max_tokens");
    json_builder_add_int_value(builder, 4000);
    json_builder_set_member_name(builder, "stream");
    json_builder_add_boolean_value(builder, FALSE);
    json_builder_end_object(builder);

    root = json_builder_get_root(builder);
    generator = json_generator_new();
    json_generator_set_root(generator, root);
    body = json_generator_to_data(generator, NULL);
    json_node_free(root);
    g_object_unref(generator);
    g_object_unref(builder);
    g_free(prompt);

    return body;
}

static void
ai_features_show_fix(MtPluginHost *host, const gchar *orig, const gchar *fixed)
{
    gint offset;
    gchar *old_text;
    gchar *new_text;

    if (host->show_inline_diff == NULL)
    {
        /* 宿主不支持行内 diff：静默放弃（original/fixed 仍保留供统计）。 */
        ai_fix_inline_visible = FALSE;
        return;
    }
    if (ai_diff_inline_span(orig, fixed, &offset, &old_text, &new_text))
    {
        host->show_inline_diff(host, offset, old_text, new_text);
        g_free(old_text);
        g_free(new_text);
        ai_fix_inline_visible = TRUE;
    }
    else
    {
        ai_fix_inline_visible = FALSE;
    }
}

/* 应用/复制按钮已移除：行内 diff 由 Tab 接受、Esc 关闭（见宿主 apply_inline_diff）。 */

static void
ai_features_fix_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    MtPluginHost *host;
    GBytes *bytes;
    GError *error;
    const gchar *response;
    gsize length;
    gchar *fixed;

    host = user_data;
    error = NULL;
    bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &error);
    if (bytes == NULL)
    {
        if (error != NULL)
        {
            gchar *message = g_strdup_printf(_("AI fix failed: %s"), error->message);
            host->show_toast(host, message);
            g_free(message);
        }
        g_clear_error(&error);
        return;
    }
    response = g_bytes_get_data(bytes, &length);
    fixed = ai_completion_extract_content(response, length, &error);
    if (fixed == NULL)
    {
        host->show_toast(host, _("AI fix returned no code"));
        g_clear_error(&error);
        g_bytes_unref(bytes);
        return;
    }
    g_free(ai_fix_fixed);
    ai_fix_fixed = fixed;
    ai_features_show_fix(host, ai_fix_original, ai_fix_fixed);
    g_bytes_unref(bytes);
}

void
ai_features_fix_start(MtPluginHost *host)
{
    GKeyFile *settings;
    gchar *endpoint;
    gchar *model;
    gchar *api_key;
    gchar *text;
    gchar *body;
    gchar *authorization;
    SoupMessage *message;
    GBytes *body_bytes;

    if (host->get_is_code_document != NULL && !host->get_is_code_document(host))
    {
        host->show_toast(host, _("AI fix is available only for recognized code documents"));
        return;
    }
    settings = ai_completion_load_settings();
    endpoint = ai_completion_get_setting(settings, "endpoint");
    model = ai_completion_get_setting(settings, "model");
    api_key = ai_completion_get_setting(settings, "api-key");
    g_key_file_unref(settings);
    {
        gchar *normalized = ai_completion_normalize_endpoint(endpoint);
        g_free(endpoint);
        endpoint = normalized;
    }
    if (*endpoint == '\0' || *model == '\0' || *api_key == '\0')
    {
        host->show_toast(host, _("Configure an AI endpoint, model and API key in Extensions first"));
        g_free(endpoint);
        g_free(model);
        g_free(api_key);
        return;
    }
    text = host->get_current_text != NULL ? host->get_current_text(host) : g_strdup("");
    {
        gchar *check = g_strdup(text);
        g_strstrip(check);
        if (*check == '\0')
        {
            host->show_toast(host, _("Add some code before fixing"));
            g_free(check);
            g_free(text);
            g_free(endpoint);
            g_free(model);
            g_free(api_key);
            return;
        }
        g_free(check);
    }
    g_free(ai_fix_original);
    ai_fix_original = g_strdup(text);
    {
        gchar *multifile = ai_completion_gather_context(host);

        body = ai_features_build_fix_body(model, text,
                                          host->get_document_language_id != NULL ?
                                          host->get_document_language_id(host) : NULL,
                                          multifile);
        g_free(multifile);
    }
    g_free(text);

    if (ai_fix_session == NULL)
    {
        ai_fix_session = soup_session_new();
        g_object_set(ai_fix_session, "timeout", 60, NULL);
    }
    message = soup_message_new("POST", endpoint);
    g_free(endpoint);
    if (message == NULL)
    {
        host->show_toast(host, _("AI endpoint URL is invalid"));
        g_free(body);
        g_free(model);
        g_free(api_key);
        return;
    }
    authorization = g_strdup_printf("Bearer %s", api_key);
    g_free(api_key);
    soup_message_headers_append(soup_message_get_request_headers(message),
                                "Authorization", authorization);
    g_free(authorization);
    soup_message_headers_append(soup_message_get_request_headers(message),
                                "Accept", "application/json");
    body_bytes = g_bytes_new_take(body, strlen(body));
    soup_message_set_request_body_from_bytes(message, "application/json", body_bytes);
    g_bytes_unref(body_bytes);

    ai_features_host = host;
    soup_session_send_and_read_async(ai_fix_session, message, G_PRIORITY_DEFAULT,
                                     NULL, ai_features_fix_finished, host);
    g_object_unref(message);
        /* 纠错改为行内红/绿 diff，不再弹 toast。 */
}

/* —— 统计（F） —— */

static void
ai_features_stats_refresh_labels(void)
{
    gchar *req;
    gchar *acc;
    gchar *tok;

    if (ai_stats_req_label == NULL)
    {
        return;
    }
    req = g_strdup_printf("%u", ai_stats_requests);
    acc = g_strdup_printf("%u", ai_stats_accepted);
    tok = g_strdup_printf("%u", ai_stats_chars / 4);
    gtk_label_set_text(ai_stats_req_label, req);
    gtk_label_set_text(ai_stats_acc_label, acc);
    gtk_label_set_text(ai_stats_tok_label, tok);
    g_free(req);
    g_free(acc);
    g_free(tok);
}

void
ai_features_stats_add_request(void)
{
    ai_stats_requests++;
}

void
ai_features_stats_add_accepted(void)
{
    ai_stats_accepted++;
}

void
ai_features_stats_add_chars(gsize chars)
{
    ai_stats_chars += (guint)chars;
}

void
ai_features_stats_load(void)
{
    GKeyFile *settings;

    settings = ai_completion_load_settings();
    ai_stats_requests = (guint)g_key_file_get_integer(settings, "AI Stats", "requests", NULL);
    ai_stats_accepted = (guint)g_key_file_get_integer(settings, "AI Stats", "accepted", NULL);
    ai_stats_chars = (guint)g_key_file_get_integer(settings, "AI Stats", "chars", NULL);
    g_key_file_unref(settings);
}

void
ai_features_stats_save(void)
{
    GKeyFile *settings;
    gchar *path;
    gchar *contents;

    settings = ai_completion_load_settings();
    g_key_file_set_integer(settings, "AI Stats", "requests", (gint)ai_stats_requests);
    g_key_file_set_integer(settings, "AI Stats", "accepted", (gint)ai_stats_accepted);
    g_key_file_set_integer(settings, "AI Stats", "chars", (gint)ai_stats_chars);
    contents = g_key_file_to_data(settings, NULL, NULL);
    path = g_build_filename(g_get_user_config_dir(), "vellum", "ai-completion.ini", NULL);
    if (contents != NULL)
    {
        if (g_file_set_contents(path, contents, (gssize)strlen(contents), NULL))
        {
            g_chmod(path, 0600);
        }
        g_free(contents);
    }
    g_free(path);
    g_key_file_unref(settings);
}

static void
ai_features_stats_reset_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    (void)user_data;
    ai_stats_requests = 0;
    ai_stats_accepted = 0;
    ai_stats_chars = 0;
    ai_features_stats_save();
    ai_features_stats_refresh_labels();
}

void
ai_features_configure_add_stats(MtPluginHost *host, AdwPreferencesPage *page)
{
    AdwPreferencesGroup *group;
    AdwActionRow *row;
    GtkWidget *reset;

    (void)host;
    group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(group, _("AI Completion Stats"));
    adw_preferences_group_set_description(group,
                                         _("Local estimates since the last reset. Tokens ≈ characters ÷ 4."));

    row = ADW_ACTION_ROW(adw_action_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), _("Requests sent"));
    ai_stats_req_label = GTK_LABEL(gtk_label_new("0"));
    adw_action_row_add_suffix(row, GTK_WIDGET(ai_stats_req_label));
    adw_preferences_group_add(group, GTK_WIDGET(row));

    row = ADW_ACTION_ROW(adw_action_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), _("Completions accepted"));
    ai_stats_acc_label = GTK_LABEL(gtk_label_new("0"));
    adw_action_row_add_suffix(row, GTK_WIDGET(ai_stats_acc_label));
    adw_preferences_group_add(group, GTK_WIDGET(row));

    row = ADW_ACTION_ROW(adw_action_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), _("Estimated tokens"));
    ai_stats_tok_label = GTK_LABEL(gtk_label_new("0"));
    adw_action_row_add_suffix(row, GTK_WIDGET(ai_stats_tok_label));
    adw_preferences_group_add(group, GTK_WIDGET(row));

    reset = gtk_button_new_with_label(_("Reset stats"));
    g_signal_connect(reset, "clicked", G_CALLBACK(ai_features_stats_reset_clicked), NULL);
    adw_preferences_group_add(group, reset);

    ai_features_stats_refresh_labels();
    adw_preferences_page_add(page, group);
}

/* —— 生命周期 —— */

static void
ai_features_fix_action(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action;
    (void)parameter;
    ai_features_fix_start((MtPluginHost *)user_data);
}

void
ai_features_init(MtPluginHost *host)
{
    static const gchar *accelerators[] = { "<Primary>e", NULL };

    ai_features_host = host;
    ai_features_stats_load();
    ai_features_panel_init(host);
    if (host->add_document_change_handler != NULL)
    {
        host->add_document_change_handler(host, ai_features_activity_changed, NULL, NULL);
    }
    if (host->add_action != NULL)
    {
        host->add_action(host, "ai-fix", ai_features_fix_action, host, NULL);
    }
    if (host->set_accelerators != NULL)
    {
        host->set_accelerators(host, "app.ai-fix", accelerators);
    }
}

void
ai_features_shutdown(void)
{
    ai_features_stats_save();
    if (ai_panel != NULL)
    {
        ai_panel_mode = 0;
        gtk_widget_set_visible(ai_panel, FALSE);
    }
    g_clear_pointer(&ai_fix_original, g_free);
    g_clear_pointer(&ai_fix_fixed, g_free);
    ai_applying_fix = FALSE;
    if (ai_fix_session != NULL)
    {
        soup_session_abort(ai_fix_session);
        g_clear_object(&ai_fix_session);
    }
}

gboolean
ai_features_is_fix_visible(void)
{
    return ai_fix_inline_visible;
}
