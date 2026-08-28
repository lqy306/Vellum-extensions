/*
 * ai-code-summary.c
 * AI 代码总结的独立实现。自动摘要仅在用户编辑受选中的代码类型时运行，
 * 并按累计修改行数、而不是定时轮询触发，以便让 token 消耗可预估。
 */

#include "ai-code-summary.h"

#include <glib/gi18n.h>
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#include <string.h>

#define AI_SUMMARY_GROUP "AI Code Summary"
#define AI_SUMMARY_DEFAULT_INTERVAL 120
#define AI_SUMMARY_SAFE_MIN_INTERVAL 30
#define AI_SUMMARY_DEBOUNCE_MILLISECONDS 1200
#define AI_SUMMARY_CONTEXT_LIMIT 18000
#define AI_SUMMARY_CACHE_LIMIT 3500

typedef struct _AiSummaryRequest AiSummaryRequest;
typedef struct _AiSummaryState AiSummaryState;

struct _AiCodeSummaryConfigWidgets
{
    AdwSwitchRow *enabled_row;
    AdwSpinRow *interval_row;
};

struct _AiSummaryRequest
{
    MtPluginHost *host;
    SoupMessage *message;
    guint generation;
    gchar *document_identity;
    gchar *current_text;
};

struct _AiSummaryState
{
    MtPluginHost *host;
    SoupSession *session;
    guint debounce_source_id;
    guint pending_lines;
    guint generation;
    gboolean request_in_flight;
    gchar *document_identity;
    gchar *baseline_text;
    gchar *summary;
};

static AiSummaryState ai_summary_state;

static gboolean
ai_code_summary_get_boolean(GKeyFile *settings,
                            const gchar *key,
                            gboolean fallback)
{
    GError *error;
    gboolean value;

    error = NULL;
    value = g_key_file_get_boolean(settings, AI_SUMMARY_GROUP, key, &error);
    if (error != NULL)
    {
        g_clear_error(&error);
        return fallback;
    }

    return value;
}

static guint
ai_code_summary_get_uint(GKeyFile *settings, const gchar *key, guint fallback)
{
    GError *error;
    gint value;

    error = NULL;
    value = g_key_file_get_integer(settings, AI_SUMMARY_GROUP, key, &error);
    if (error != NULL || value <= 0)
    {
        g_clear_error(&error);
        return fallback;
    }

    return (guint)value;
}

static GKeyFile *
ai_code_summary_load_settings(void)
{
    GKeyFile *settings;
    gchar *path;

    settings = g_key_file_new();
    path = g_build_filename(g_get_user_config_dir(), "vellum", "ai-completion.ini", NULL);
    g_key_file_load_from_file(settings, path, G_KEY_FILE_NONE, NULL);
    g_free(path);

    return settings;
}

static gchar *
ai_code_summary_get_string(GKeyFile *settings,
                           const gchar *group,
                           const gchar *key)
{
    gchar *value;

    value = g_key_file_get_string(settings, group, key, NULL);
    if (value == NULL)
    {
        value = g_strdup("");
    }

    return value;
}

static gchar *
ai_code_summary_normalize_endpoint(const gchar *endpoint)
{
    gchar *normalized;
    GUri *uri;
    const gchar *path;
    gsize length;

    normalized = g_strdup(endpoint != NULL ? endpoint : "");
    g_strstrip(normalized);
    length = strlen(normalized);
    while (length > 1 && normalized[length - 1] == '/')
    {
        normalized[--length] = '\0';
    }
    if (g_str_has_suffix(normalized, "/v1"))
    {
        gchar *with_path;

        with_path = g_strconcat(normalized, "/chat/completions", NULL);
        g_free(normalized);
        return with_path;
    }
    if (g_str_has_suffix(normalized, "/chat/completions"))
    {
        return normalized;
    }
    uri = g_uri_parse(normalized, G_URI_FLAGS_NONE, NULL);
    path = uri != NULL ? g_uri_get_path(uri) : NULL;
    if (uri != NULL && (path == NULL || *path == '\0' || g_str_equal(path, "/")))
    {
        gchar *with_path;

        with_path = g_strconcat(normalized, "/chat/completions", NULL);
        g_free(normalized);
        normalized = with_path;
    }
    if (uri != NULL)
    {
        g_uri_unref(uri);
    }

    return normalized;
}

static gchar *
ai_code_summary_trim_tail(const gchar *text, glong limit)
{
    glong characters;
    const gchar *start;

    if (text == NULL)
    {
        return g_strdup("");
    }
    characters = g_utf8_strlen(text, -1);
    if (characters <= limit)
    {
        return g_strdup(text);
    }
    start = g_utf8_offset_to_pointer(text, characters - limit);
    return g_strdup(start);
}

static gboolean
ai_code_summary_is_chinese(void)
{
    const gchar * const *languages;

    languages = g_get_language_names();
    return languages != NULL && languages[0] != NULL &&
           g_str_has_prefix(languages[0], "zh");
}

static const gchar *
ai_code_summary_document_kind(const gchar *path)
{
    gchar *lower;
    const gchar *kind;

    if (path == NULL || *path == '\0')
    {
        return "source";
    }
    lower = g_ascii_strdown(path, -1);
    kind = "source";
    if (g_str_has_suffix(lower, ".py") || g_str_has_suffix(lower, ".sh") ||
        g_str_has_suffix(lower, ".bash") || g_str_has_suffix(lower, ".zsh") ||
        g_str_has_suffix(lower, ".fish") || g_str_has_suffix(lower, ".rb") ||
        g_str_has_suffix(lower, ".pl") || g_str_has_suffix(lower, ".php"))
    {
        kind = "script";
    }
    else if (g_str_has_suffix(lower, ".js") || g_str_has_suffix(lower, ".mjs") ||
             g_str_has_suffix(lower, ".ts") || g_str_has_suffix(lower, ".tsx") ||
             g_str_has_suffix(lower, ".jsx") || g_str_has_suffix(lower, ".html") ||
             g_str_has_suffix(lower, ".htm") || g_str_has_suffix(lower, ".css") ||
             g_str_has_suffix(lower, ".scss") || g_str_has_suffix(lower, ".vue") ||
             g_str_has_suffix(lower, ".svelte"))
    {
        kind = "web";
    }
    else if (g_str_has_suffix(lower, ".json") || g_str_has_suffix(lower, ".yaml") ||
             g_str_has_suffix(lower, ".yml") || g_str_has_suffix(lower, ".toml") ||
             g_str_has_suffix(lower, ".xml") || g_str_has_suffix(lower, ".sql"))
    {
        kind = "data";
    }
    g_free(lower);

    return kind;
}

static gboolean
ai_code_summary_current_document_enabled(MtPluginHost *host, GKeyFile *settings)
{
    gchar *path;
    const gchar *kind;

    if (host == NULL || (host->get_is_code_document != NULL && !host->get_is_code_document(host)))
    {
        return FALSE;
    }
    path = host->get_current_file_path != NULL ? host->get_current_file_path(host) : NULL;
    kind = ai_code_summary_document_kind(path);
    g_free(path);
    if (g_str_equal(kind, "script"))
    {
        return ai_code_summary_get_boolean(settings, "scripts", TRUE);
    }
    if (g_str_equal(kind, "web"))
    {
        return ai_code_summary_get_boolean(settings, "web", TRUE);
    }
    if (g_str_equal(kind, "data"))
    {
        return ai_code_summary_get_boolean(settings, "data", FALSE);
    }

    return ai_code_summary_get_boolean(settings, "source", TRUE);
}

static gchar *
ai_code_summary_document_identity(MtPluginHost *host)
{
    gchar *path;
    gchar *identity;

    path = host->get_current_file_path != NULL ? host->get_current_file_path(host) : NULL;
    if (path != NULL && *path != '\0')
    {
        identity = path;
    }
    else
    {
        g_free(path);
        identity = g_strdup_printf("untitled:%p", (void *)host);
    }

    return identity;
}

static void
ai_code_summary_reset_document(MtPluginHost *host)
{
    gchar *identity;
    gchar *contents;

    identity = ai_code_summary_document_identity(host);
    if (g_strcmp0(identity, ai_summary_state.document_identity) == 0)
    {
        g_free(identity);
        return;
    }
    contents = host->get_current_text != NULL ? host->get_current_text(host) : g_strdup("");
    g_free(ai_summary_state.document_identity);
    g_free(ai_summary_state.baseline_text);
    g_free(ai_summary_state.summary);
    ai_summary_state.document_identity = identity;
    ai_summary_state.baseline_text = contents;
    ai_summary_state.summary = NULL;
    ai_summary_state.pending_lines = 0;
}

/* 自动总结是否开启：由设置页的开关控制；手动总结不受影响。 */
gboolean
ai_code_summary_auto_enabled(GKeyFile *settings)
{
    return ai_code_summary_get_boolean(settings, "enabled", TRUE);
}

AiCodeSummaryConfigWidgets *
ai_code_summary_add_config_group(AdwPreferencesPage *page, GKeyFile *settings)
{
    AiCodeSummaryConfigWidgets *widgets;
    AdwPreferencesGroup *group;

    widgets = g_new0(AiCodeSummaryConfigWidgets, 1);
    group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(group, _("AI Auto Summary"));
    adw_preferences_group_set_description(group,
                                          _("Create a compact local summary after a configurable amount of code editing. The latest summary is sent with later AI requests for the same document."));

    widgets->enabled_row = ADW_SWITCH_ROW(adw_switch_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(widgets->enabled_row),
                                  _("Enable automatic code summary"));
    adw_action_row_set_subtitle(ADW_ACTION_ROW(widgets->enabled_row),
                                _("Summarize the whole document after the configured number of modified lines and send it with later AI requests."));
    adw_switch_row_set_active(widgets->enabled_row,
                              ai_code_summary_get_boolean(settings, "enabled", TRUE));
    adw_preferences_group_add(group, GTK_WIDGET(widgets->enabled_row));

    widgets->interval_row = ADW_SPIN_ROW(adw_spin_row_new_with_range(1.0, 5000.0, 1.0));
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(widgets->interval_row),
                                  _("Modified lines between automatic summaries"));
    adw_action_row_set_subtitle(ADW_ACTION_ROW(widgets->interval_row),
                                _("The recommended minimum is 30 lines; lower values can greatly increase token use."));
    adw_spin_row_set_digits(widgets->interval_row, 0);
    gtk_adjustment_set_value(adw_spin_row_get_adjustment(widgets->interval_row),
                             ai_code_summary_get_uint(settings,
                                                          "line-interval",
                                                          AI_SUMMARY_DEFAULT_INTERVAL));
    adw_preferences_group_add(group, GTK_WIDGET(widgets->interval_row));

    {
        /* 关闭自动总结时写文件注释的提示：与总结设置放在一起。 */
        GtkWidget *note;

        note = gtk_label_new(NULL);
        gtk_widget_set_margin_start(note, 4);
        gtk_widget_set_margin_end(note, 4);
        gtk_widget_set_margin_bottom(note, 4);
        gtk_widget_set_hexpand(note, TRUE);
        gtk_label_set_wrap(GTK_LABEL(note), TRUE);
        gtk_label_set_justify(GTK_LABEL(note), GTK_JUSTIFY_FILL);
        gtk_label_set_markup(GTK_LABEL(note),
                             g_strdup_printf(
                                 "<span foreground=\"#888888\" style=\"italic\">"
                                 "%s %s</span>",
                                 _("Tip: file-header comment when summaries are disabled"),
                                 _("If you disable auto-summary to reduce API costs, "
                                   "please add a clear header comment at the top of each source file describing what it does, "
                                   "its public interface, and any non-obvious invariants. This helps both human readers and AI tools "
                                   "understand the file without reading every line.")));
        adw_preferences_group_add(group, note);
    }

    adw_preferences_page_add(page, group);
    return widgets;
}

void
ai_code_summary_config_widgets_free(AiCodeSummaryConfigWidgets *widgets)
{
    g_free(widgets);
}

gboolean
ai_code_summary_config_requires_cost_warning(AiCodeSummaryConfigWidgets *widgets)
{
    if (widgets == NULL || !adw_switch_row_get_active(widgets->enabled_row))
    {
        return FALSE;
    }

    return gtk_adjustment_get_value(adw_spin_row_get_adjustment(widgets->interval_row)) <
           AI_SUMMARY_SAFE_MIN_INTERVAL;
}

void
ai_code_summary_config_save(GKeyFile *settings, AiCodeSummaryConfigWidgets *widgets)
{
    g_return_if_fail(settings != NULL);
    g_return_if_fail(widgets != NULL);

    g_key_file_set_boolean(settings, AI_SUMMARY_GROUP, "enabled",
                           adw_switch_row_get_active(widgets->enabled_row));
    g_key_file_set_integer(settings, AI_SUMMARY_GROUP, "line-interval",
                           (gint)gtk_adjustment_get_value(adw_spin_row_get_adjustment(widgets->interval_row)));
}

static gchar *
ai_code_summary_build_body(const gchar *model,
                           const gchar *previous_summary,
                           const gchar *current_text)
{
    JsonBuilder *builder;
    JsonGenerator *generator;
    JsonNode *root;
    gchar *prompt;
    gchar *body;
    const gchar *language_instruction;

    language_instruction = ai_code_summary_is_chinese() ?
                           "Respond in Simplified Chinese. Keep identifiers and code symbols unchanged." :
                           "Respond in English. Keep identifiers and code symbols unchanged.";
    prompt = g_strdup_printf("Previous summary (may be empty):\n%s\n\nCurrent code:\n%s\n\nWrite a concise maintenance summary. Cover purpose, changed behavior, important functions/data flow, and unresolved risks. Do not use a Markdown heading. %s",
                             previous_summary != NULL ? previous_summary : "",
                             current_text,
                             language_instruction);
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
                                  "You summarize code for its next AI assistance request. Be accurate, compact and concrete. Do not invent files, APIs or results.");
    json_builder_end_object(builder);
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "role");
    json_builder_add_string_value(builder, "user");
    json_builder_set_member_name(builder, "content");
    json_builder_add_string_value(builder, prompt);
    json_builder_end_object(builder);
    json_builder_end_array(builder);
    json_builder_set_member_name(builder, "temperature");
    json_builder_add_double_value(builder, 0.15);
    json_builder_set_member_name(builder, "max_tokens");
    json_builder_add_int_value(builder, 520);
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

static gchar *
ai_code_summary_extract_content(const gchar *response, gsize length, GError **error)
{
    JsonParser *parser;
    JsonNode *root;
    JsonObject *object;
    JsonArray *choices;
    JsonObject *choice;
    JsonObject *message;
    const gchar *content;

    parser = json_parser_new();
    if (!json_parser_load_from_data(parser, response, (gssize)length, error))
    {
        g_object_unref(parser);
        return NULL;
    }
    root = json_parser_get_root(parser);
    if (!JSON_NODE_HOLDS_OBJECT(root))
    {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "AI response is not a JSON object");
        g_object_unref(parser);
        return NULL;
    }
    object = json_node_get_object(root);
    choices = json_object_get_array_member(object, "choices");
    if (choices == NULL || json_array_get_length(choices) == 0)
    {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "AI response has no choices");
        g_object_unref(parser);
        return NULL;
    }
    choice = json_array_get_object_element(choices, 0);
    message = choice != NULL ? json_object_get_object_member(choice, "message") : NULL;
    content = message != NULL ? json_object_get_string_member(message, "content") : NULL;
    if (content == NULL || *content == '\0')
    {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "AI response has no summary text");
        g_object_unref(parser);
        return NULL;
    }
    content = g_strdup(content);
    g_object_unref(parser);

    return (gchar *)content;
}

static void
ai_code_summary_request_free(AiSummaryRequest *request)
{
    if (request != NULL)
    {
        g_clear_object(&request->message);
        g_free(request->document_identity);
        g_free(request->current_text);
        g_free(request);
    }
}

static void
ai_code_summary_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    AiSummaryRequest *request;
    GBytes *bytes;
    GError *error;
    const gchar *response;
    gsize length;
    gchar *summary;

    request = user_data;
    if (request->generation == ai_summary_state.generation)
    {
        ai_summary_state.request_in_flight = FALSE;
    }
    error = NULL;
    bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &error);
    if (request->generation != ai_summary_state.generation || ai_summary_state.session == NULL)
    {
        g_clear_error(&error);
        g_clear_pointer(&bytes, g_bytes_unref);
        ai_code_summary_request_free(request);
        return;
    }
    if (bytes == NULL || soup_message_get_status(request->message) < 200 ||
        soup_message_get_status(request->message) >= 300)
    {
        if (error != NULL)
        {
            gchar *message;

            message = g_strdup_printf(_("AI code summary failed: %s"), error->message);
            request->host->show_toast(request->host, message);
            g_free(message);
        }
        else
        {
            request->host->show_toast(request->host, _("AI code summary service returned an error"));
        }
        g_clear_error(&error);
        g_clear_pointer(&bytes, g_bytes_unref);
        ai_code_summary_request_free(request);
        return;
    }
    response = g_bytes_get_data(bytes, &length);
    summary = ai_code_summary_extract_content(response, length, &error);
    if (summary == NULL)
    {
        gchar *message;

        message = g_strdup_printf(_("Unable to parse AI code summary: %s"), error->message);
        request->host->show_toast(request->host, message);
        g_free(message);
        g_clear_error(&error);
    }
    else if (g_strcmp0(request->document_identity, ai_summary_state.document_identity) == 0)
    {
        g_free(ai_summary_state.summary);
        ai_summary_state.summary = ai_code_summary_trim_tail(summary, AI_SUMMARY_CACHE_LIMIT);
        g_free(ai_summary_state.baseline_text);
        ai_summary_state.baseline_text = g_strdup(request->current_text);
        ai_summary_state.pending_lines = 0;
        request->host->show_toast(request->host, _("AI code summary updated and will be included with later AI requests"));
        g_free(summary);
    }
    else
    {
        g_free(summary);
    }
    g_bytes_unref(bytes);
    ai_code_summary_request_free(request);
}

static gboolean
ai_code_summary_start(MtPluginHost *host, gboolean manual)
{
    GKeyFile *settings;
    gchar *endpoint;
    gchar *model;
    gchar *api_key;
    gchar *current_text;
    gchar *trimmed_text;
    gchar *body;
    gchar *authorization;
    SoupMessage *message;
    GBytes *body_bytes;
    AiSummaryRequest *request;

    settings = ai_code_summary_load_settings();
    if (!manual && !ai_code_summary_auto_enabled(settings))
    {
        g_key_file_unref(settings);
        return FALSE;
    }
    if (!manual && !ai_code_summary_current_document_enabled(host, settings))
    {
        g_key_file_unref(settings);
        return FALSE;
    }
    if (manual && (host->get_is_code_document == NULL || !host->get_is_code_document(host)))
    {
        host->show_toast(host, _("AI code summaries are available only for recognized code documents"));
        g_key_file_unref(settings);
        return FALSE;
    }
    endpoint = ai_code_summary_get_string(settings, "AI Completion", "endpoint");
    model = ai_code_summary_get_string(settings, "AI Completion", "model");
    api_key = ai_code_summary_get_string(settings, "AI Completion", "api-key");
    g_key_file_unref(settings);
    {
        gchar *normalized;

        normalized = ai_code_summary_normalize_endpoint(endpoint);
        g_free(endpoint);
        endpoint = normalized;
    }
    if (*endpoint == '\0' || *model == '\0' || *api_key == '\0')
    {
        if (manual)
        {
            host->show_toast(host, _("Configure an AI endpoint, model and API key in Extensions first"));
        }
        g_free(endpoint);
        g_free(model);
        g_free(api_key);
        return FALSE;
    }
    ai_code_summary_reset_document(host);
    current_text = host->get_current_text != NULL ? host->get_current_text(host) : g_strdup("");
    {
        gchar *trimmed_check;

        trimmed_check = g_strdup(current_text);
        g_strstrip(trimmed_check);
        if (*trimmed_check == '\0')
        {
            if (manual)
            {
                host->show_toast(host, _("Add some code before generating an AI summary"));
            }
            g_free(trimmed_check);
            g_free(current_text);
            g_free(endpoint);
            g_free(model);
            g_free(api_key);
            return FALSE;
        }
        g_free(trimmed_check);
    }
    trimmed_text = ai_code_summary_trim_tail(current_text, AI_SUMMARY_CONTEXT_LIMIT);
    body = ai_code_summary_build_body(model, ai_summary_state.summary, trimmed_text);
    message = soup_message_new("POST", endpoint);
    if (message == NULL)
    {
        if (manual)
        {
            host->show_toast(host, _("AI endpoint URL is invalid"));
        }
        g_free(body);
        g_free(trimmed_text);
        g_free(current_text);
        g_free(endpoint);
        g_free(model);
        g_free(api_key);
        return FALSE;
    }
    authorization = g_strdup_printf("Bearer %s", api_key);
    soup_message_headers_append(soup_message_get_request_headers(message), "Authorization", authorization);
    soup_message_headers_append(soup_message_get_request_headers(message), "Accept", "application/json");
    body_bytes = g_bytes_new_take(body, strlen(body));
    soup_message_set_request_body_from_bytes(message, "application/json", body_bytes);
    g_bytes_unref(body_bytes);
    if (ai_summary_state.request_in_flight)
    {
        ai_summary_state.generation++;
        soup_session_abort(ai_summary_state.session);
    }
    ai_summary_state.generation++;
    request = g_new0(AiSummaryRequest, 1);
    request->host = host;
    request->message = g_object_ref(message);
    request->generation = ai_summary_state.generation;
    request->document_identity = g_strdup(ai_summary_state.document_identity);
    request->current_text = g_strdup(current_text);
    ai_summary_state.request_in_flight = TRUE;
    soup_session_send_and_read_async(ai_summary_state.session, message, G_PRIORITY_DEFAULT,
                                     NULL, ai_code_summary_finished, request);
    if (manual)
    {
        host->show_toast(host, _("Generating AI code summary…"));
    }
    g_object_unref(message);
    g_free(authorization);
    g_free(trimmed_text);
    g_free(current_text);
    g_free(endpoint);
    g_free(model);
    g_free(api_key);

    return TRUE;
}

static gboolean
ai_code_summary_debounce_cb(gpointer user_data)
{
    MtPluginHost *host;

    host = user_data;
    ai_summary_state.debounce_source_id = 0;
    ai_code_summary_start(host, FALSE);
    return G_SOURCE_REMOVE;
}

static void
ai_code_summary_document_changed(MtPluginHost *host,
                                 guint changed_lines,
                                 gpointer user_data)
{
    GKeyFile *settings;
    guint interval;

    (void)user_data;
    settings = ai_code_summary_load_settings();
    if (!ai_code_summary_current_document_enabled(host, settings))
    {
        g_key_file_unref(settings);
        return;
    }
    interval = ai_code_summary_get_uint(settings, "line-interval", AI_SUMMARY_DEFAULT_INTERVAL);
    g_key_file_unref(settings);
    ai_code_summary_reset_document(host);
    ai_summary_state.pending_lines += changed_lines;
    if (ai_summary_state.pending_lines < interval || ai_summary_state.request_in_flight)
    {
        return;
    }
    if (ai_summary_state.debounce_source_id != 0)
    {
        g_source_remove(ai_summary_state.debounce_source_id);
    }
    ai_summary_state.debounce_source_id = g_timeout_add(AI_SUMMARY_DEBOUNCE_MILLISECONDS,
                                                         ai_code_summary_debounce_cb,
                                                         host);
}

static void
ai_code_summary_action(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action;
    (void)parameter;
    ai_code_summary_start(user_data, TRUE);
}

void
ai_code_summary_activate(MtPluginHost *host)
{
    static const gchar *accelerators[] = { "<Primary><Alt>s", NULL };

    memset(&ai_summary_state, 0, sizeof(ai_summary_state));
    ai_summary_state.host = host;
    ai_summary_state.session = soup_session_new();
    g_object_set(ai_summary_state.session, "timeout", 45, NULL);
    if (host->add_action != NULL)
    {
        host->add_action(host, "ai-summarize", ai_code_summary_action, host, NULL);
    }
    if (host->set_accelerators != NULL)
    {
        host->set_accelerators(host, "app.ai-summarize", accelerators);
    }
    if (host->add_document_change_handler != NULL)
    {
        host->add_document_change_handler(host, ai_code_summary_document_changed, NULL, NULL);
    }
}

void
ai_code_summary_deactivate(void)
{
    if (ai_summary_state.debounce_source_id != 0)
    {
        g_source_remove(ai_summary_state.debounce_source_id);
    }
    ai_summary_state.debounce_source_id = 0;
    ai_summary_state.generation++;
    if (ai_summary_state.session != NULL)
    {
        soup_session_abort(ai_summary_state.session);
        g_clear_object(&ai_summary_state.session);
    }
    g_free(ai_summary_state.document_identity);
    g_free(ai_summary_state.baseline_text);
    g_free(ai_summary_state.summary);
    memset(&ai_summary_state, 0, sizeof(ai_summary_state));
}

gchar *
ai_code_summary_get_current(MtPluginHost *host)
{
    gchar *identity;
    gchar *summary;

    if (host == NULL || ai_summary_state.summary == NULL)
    {
        return NULL;
    }
    identity = ai_code_summary_document_identity(host);
    summary = g_strcmp0(identity, ai_summary_state.document_identity) == 0 ?
              g_strdup(ai_summary_state.summary) : NULL;
    g_free(identity);

    return summary;
}
