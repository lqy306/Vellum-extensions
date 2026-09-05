/*
 * ai-completion-stream.c
 * SSE 流式输出、请求生命周期、按键路由、自动调度。
 * AiRequest 结构在此文件内部定义。
 */

#include "ai-completion-private.h"
#include "ai-completion-features.h"

#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <glib/gi18n.h>
#include <string.h>

typedef struct _AiRequest
{
    MtPluginHost *host;
    SoupMessage *message;
    GInputStream *stream;
    GString *buffer;
    guchar read_buffer[AI_STREAM_READ_SIZE];
    gchar *context;
    gchar *suffix;
    gchar *completion;
    guint generation;
    gboolean automatic;
    gboolean streaming;
    gboolean error_mode;
    guint http_status;
    guint attempt;
} AiRequest;

static void
ai_completion_finish_stream(AiRequest *request);

static void
ai_completion_show_candidate(AiRequest *request, const gchar *completion);

void
ai_completion_request_start(MtPluginHost *host, gboolean automatic);

static gchar *
ai_completion_sse_payload_delta(const gchar *payload)
{
    JsonParser *parser;
    JsonNode *root;
    JsonObject *object;
    JsonArray *choices;
    JsonObject *choice;
    gchar *delta;

    parser = json_parser_new();
    if (!json_parser_load_from_data(parser, payload, -1, NULL))
    {
        g_object_unref(parser);
        return NULL;
    }
    root = json_parser_get_root(parser);
    if (!JSON_NODE_HOLDS_OBJECT(root))
    {
        g_object_unref(parser);
        return NULL;
    }
    object = json_node_get_object(root);
    choices = json_object_get_array_member(object, "choices");
    if (choices == NULL || json_array_get_length(choices) == 0)
    {
        g_object_unref(parser);
        return NULL;
    }
    choice = json_array_get_object_element(choices, 0);
    delta = ai_completion_extract_choice_content(choice);
    g_object_unref(parser);

    return delta;
}

static gboolean
ai_completion_sse_line(AiRequest *request, const gchar *line, gsize line_length)
{
    gchar *owned;
    const gchar *payload;
    gchar *delta;
    gboolean done;

    if (line_length == 0)
    {
        return FALSE;
    }
    owned = g_strndup(line, line_length);
    if (owned[0] == ':')
    {
        /* SSE 注释行 */
        g_free(owned);
        return FALSE;
    }
    done = FALSE;
    payload = NULL;
    if (g_str_has_prefix(owned, "data:"))
    {
        payload = owned + 5;
        while (*payload == ' ')
        {
            payload++;
        }
    }
    else if (owned[0] == '{')
    {
        /* 少数兼容服务不写 data: 前缀，直接输出 JSON 行。 */
        payload = owned;
    }
    if (payload != NULL)
    {
        if (g_str_equal(payload, "[DONE]"))
        {
            ai_completion_finish_stream(request);
            done = TRUE;
        }
        else
        {
            delta = ai_completion_sse_payload_delta(payload);
            if (delta != NULL && *delta != '\0')
            {
                gchar *joined;

                joined = g_strconcat(request->completion != NULL ? request->completion : "",
                                     delta,
                                     NULL);
                g_free(request->completion);
                request->completion = joined;
                ai_completion_show_candidate(request, request->completion);
            }
            g_free(delta);
        }
    }
    g_free(owned);

    return done;
}

/* 处理缓冲区中所有完整的 SSE 行；返回 TRUE 表示已收到 [DONE]，
 * 此时请求已被终结并释放，调用方不得再访问。 */
static gboolean
ai_completion_process_sse(AiRequest *request)
{
    gchar *cursor;
    gboolean done;

    cursor = request->buffer->str;
    done = FALSE;
    while (*cursor != '\0')
    {
        gchar *newline;
        gchar *line;
        gsize line_length;

        newline = strchr(cursor, '\n');
        if (newline == NULL)
        {
            break;
        }
        line = cursor;
        line_length = (gsize)(newline - cursor);
        if (line_length > 0 && line[line_length - 1] == '\r')
        {
            line_length--;
        }
        done = ai_completion_sse_line(request, line, line_length);
        if (done)
        {
            break;
        }
        cursor = newline + 1;
    }
    if (!done)
    {
        g_string_erase(request->buffer, 0, (gssize)(cursor - request->buffer->str));
    }

    return done;
}

static void
ai_completion_request_free(AiRequest *request)
{
    if (request == NULL)
    {
        return;
    }
    g_clear_object(&request->message);
    if (request->stream != NULL)
    {
        g_object_unref(request->stream);
    }
    if (request->buffer != NULL)
    {
        g_string_free(request->buffer, TRUE);
    }
    g_free(request->context);
    g_free(request->suffix);
    g_free(request->completion);
    g_free(request);
}

static void
ai_completion_finish_stream(AiRequest *request)
{
    /* 流在 [DONE] 或 EOF 时结束。若仍有未换行的残余 data 行，按一个事件处理。 */
    if (request->buffer->len > 0 && request->buffer->str[0] != '\0')
    {
        gchar *tail;
        gchar *delta;

        tail = g_strdup(request->buffer->str);
        g_strstrip(tail);
        delta = NULL;
        if (*tail != '\0')
        {
            const gchar *payload;

            if (g_str_has_prefix(tail, "data:"))
            {
                payload = tail + 5;
                while (*payload == ' ')
                {
                    payload++;
                }
            }
            else
            {
                payload = tail;
            }
            if (!g_str_equal(payload, "[DONE]"))
            {
                delta = ai_completion_sse_payload_delta(payload);
            }
            if (delta != NULL && *delta != '\0')
            {
                gchar *joined;

                joined = g_strconcat(request->completion != NULL ? request->completion : "",
                                     delta,
                                     NULL);
                g_free(request->completion);
                request->completion = joined;
            }
            g_free(delta);
        }
        g_free(tail);
    }

    if (request->completion == NULL || *request->completion == '\0')
    {
        if (request->automatic && request->attempt == 1)
        {
            /* 空结果重试一次：临时关闭总结再请求，避免总结误导模型。 */
            ai_request_in_flight = FALSE;
            ai_force_no_summary = TRUE;
            ai_completion_request_start(request->host, TRUE);
            ai_completion_request_free(request);
            return;
        }
        if (!request->automatic)
        {
            request->host->show_toast(request->host, _("AI service returned an empty completion"));
        }
        ai_request_in_flight = FALSE;
        ai_completion_request_free(request);
        return;
    }
    ai_completion_show_candidate(request, request->completion);
    ai_features_stats_add_chars(strlen(request->completion));
    if (!request->automatic)
    {
        request->host->show_toast(request->host, _("AI completion ready: press Tab to accept or Escape to dismiss"));
    }
    ai_request_in_flight = FALSE;
    ai_completion_request_free(request);
}

static void
ai_completion_finish_json(AiRequest *request)
{
    GError *error;
    gchar *completion;

    error = NULL;
    completion = ai_completion_extract_content(request->buffer->str,
                                               request->buffer->len,
                                               &error);
    if (completion != NULL)
    {
        ai_completion_show_candidate(request, completion);
        ai_features_stats_add_chars(strlen(completion));
        if (!request->automatic)
        {
            /* 补全提示已改为行内半透明幽灵，不再弹 toast 干扰。 */
        }
        g_free(completion);
    }
    else
    {
        gchar *message;

        message = g_strdup_printf(_("Unable to parse AI completion: %s"), error->message);
        if (!request->automatic)
        {
            request->host->show_toast(request->host, message);
        }
        g_free(message);
        g_clear_error(&error);
    }
    ai_request_in_flight = FALSE;
    ai_completion_request_free(request);
}

static void
ai_completion_finish_error(AiRequest *request)
{
    gchar *detail;
    gchar *message;

    detail = ai_completion_error_detail_data(request->buffer->str, request->buffer->len);
    message = g_strdup_printf(_("AI service returned HTTP %u: %s"),
                              request->http_status,
                              (detail != NULL && *detail != '\0') ?
                              detail : soup_message_get_reason_phrase(request->message));
    if (!request->automatic)
    {
        request->host->show_toast(request->host, message);
    }
    g_free(detail);
    g_free(message);
    ai_request_in_flight = FALSE;
    ai_completion_request_free(request);
}

static void
ai_completion_read_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    AiRequest *request;
    GError *error;
    gssize count;

    request = user_data;
    error = NULL;
    count = g_input_stream_read_finish(G_INPUT_STREAM(source), result, &error);

    /* 会话停用或有较新的请求时，只完成并释放 I/O，不再触及插件宿主。 */
    if (request->generation != ai_generation || ai_session == NULL)
    {
        g_clear_error(&error);
        ai_completion_request_free(request);
        return;
    }

    if (count > 0)
    {
        g_string_append_len(request->buffer,
                            (const gchar *)request->read_buffer,
                            (gssize)count);
        if (request->buffer->len > AI_STREAM_BUFFER_LIMIT)
        {
            if (!request->automatic)
            {
                request->host->show_toast(request->host, _("AI completion response was too large"));
            }
            ai_request_in_flight = FALSE;
            ai_completion_request_free(request);
            return;
        }
        if (request->streaming && ai_completion_process_sse(request))
        {
            /* [DONE]：请求已在处理过程中终结并释放。 */
            return;
        }
        g_input_stream_read_async(request->stream,
                                  request->read_buffer,
                                  sizeof(request->read_buffer),
                                  G_PRIORITY_DEFAULT,
                                  NULL,
                                  ai_completion_read_finished,
                                  request);
        return;
    }

    if (error != NULL)
    {
        /* 中断：abort（新请求或停用）在上面的代际检查已释放，这里只处理真实错误。 */
        if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
            gchar *message;

            message = g_strdup_printf(_("AI completion request failed: %s"), error->message);
            if (!request->automatic)
            {
                request->host->show_toast(request->host, message);
            }
            g_free(message);
            ai_request_in_flight = FALSE;
        }
        g_clear_error(&error);
        ai_completion_request_free(request);
        return;
    }

    if (request->error_mode)
    {
        ai_completion_finish_error(request);
    }
    else if (request->streaming)
    {
        ai_completion_finish_stream(request);
    }
    else
    {
        ai_completion_finish_json(request);
    }
}

static void
ai_completion_send_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    AiRequest *request;
    GInputStream *stream;
    GError *error;
    guint status;
    const gchar *content_type;

    request = user_data;
    error = NULL;
    stream = soup_session_send_finish(SOUP_SESSION(source), result, &error);

    if (request->generation != ai_generation || ai_session == NULL)
    {
        g_clear_error(&error);
        if (stream != NULL)
        {
            g_object_unref(stream);
        }
        ai_completion_request_free(request);
        return;
    }

    if (stream == NULL)
    {
        if (error == NULL || !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
            gchar *message;

            message = g_strdup_printf(_("AI completion request failed: %s"),
                                      error != NULL ? error->message : _("No response received"));
            if (!request->automatic)
            {
                request->host->show_toast(request->host, message);
            }
            g_free(message);
        }
        g_clear_error(&error);
        ai_request_in_flight = FALSE;
        ai_completion_request_free(request);
        return;
    }

    request->stream = stream;
    request->buffer = g_string_new(NULL);
    status = soup_message_get_status(request->message);
    if (status < 200 || status >= 300)
    {
        /* 读取错误正文，让 toast 能显示服务端的错误详情。 */
        request->error_mode = TRUE;
        request->http_status = status;
        g_input_stream_read_async(stream,
                                  request->read_buffer,
                                  sizeof(request->read_buffer),
                                  G_PRIORITY_DEFAULT,
                                  NULL,
                                  ai_completion_read_finished,
                                  request);
        return;
    }
    content_type = soup_message_headers_get_content_type(
        soup_message_get_response_headers(request->message), NULL);
    request->streaming = content_type != NULL &&
                         strstr(content_type, "text/event-stream") != NULL;
    g_input_stream_read_async(stream,
                              request->read_buffer,
                              sizeof(request->read_buffer),
                              G_PRIORITY_DEFAULT,
                              NULL,
                              ai_completion_read_finished,
                              request);
}

/* 把累积的补全文本作为候选展示：只在光标前后文与请求时一致时生效，
 * 流式过程中每次文本增长都会调用，因此会不断刷新幽灵文本。 */
static void
ai_completion_show_candidate(AiRequest *request, const gchar *completion)
{
    gchar *adjusted;
    gchar *preview;
    gchar *current_context;
    gchar *current_suffix;
    gboolean contexts_match;

    adjusted = ai_completion_adjust_candidate(completion, request->context, request->suffix);
    preview = ai_completion_prepare_inline_candidate(adjusted);
    if (preview == NULL)
    {
        /* 补全以空行开头：首行还没有内容，继续累积，暂不显示。 */
        g_free(adjusted);
        return;
    }

    if (g_get_monotonic_time() < ai_reject_until_us)
    {
        /* 用户刚拒绝过候选（Escape 或继续输入）：迟到的流式响应不再弹回。 */
        g_free(adjusted);
        g_free(preview);
        return;
    }

    current_context = request->host->get_text_before_cursor(request->host);
    current_suffix = request->host->get_text_after_cursor != NULL ?
                     request->host->get_text_after_cursor(request->host) : g_strdup("");
    contexts_match = g_strcmp0(current_context, request->context) == 0 &&
                     g_strcmp0(current_suffix, request->suffix) == 0;
    g_free(current_context);
    g_free(current_suffix);
    if (!contexts_match)
    {
        g_free(adjusted);
        g_free(preview);
        return;
    }

    if (g_strcmp0(adjusted, ai_candidate.text) != 0 || ai_candidate.host != request->host)
    {
        ai_completion_clear_candidate();
        ai_candidate.host = request->host;
        ai_candidate.text = adjusted;
        adjusted = NULL;
        ai_candidate.context = g_strdup(request->context);
        ai_candidate.suffix = g_strdup(request->suffix);
        if (ai_candidate_timeout_source_id != 0)
        {
            g_source_remove(ai_candidate_timeout_source_id);
        }
        ai_candidate_timeout_source_id =
            g_timeout_add_seconds(AI_CANDIDATE_TIMEOUT_SECONDS,
                                  ai_completion_candidate_timeout,
                                  request->host);
        request->host->show_inline_completion(request->host, preview);
    }
    g_free(adjusted);
    g_free(preview);
}

static gboolean
ai_completion_key_is_editing(guint keyval, guint state)
{
    if ((state & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK)) != 0)
    {
        return FALSE;
    }
    if ((keyval >= GDK_KEY_Left && keyval <= GDK_KEY_Down) ||
        (keyval >= GDK_KEY_F1 && keyval <= GDK_KEY_F12) ||
        keyval == GDK_KEY_Home || keyval == GDK_KEY_End ||
        keyval == GDK_KEY_Page_Up || keyval == GDK_KEY_Page_Down ||
        keyval == GDK_KEY_Insert || keyval == GDK_KEY_Tab)
    {
        return FALSE;
    }

    return TRUE;
}

static gboolean
ai_completion_auto_cb(gpointer user_data)
{
    MtPluginHost *host;

    host = user_data;
    ai_auto_source_id = 0;
    if (ai_session == NULL)
    {
        return G_SOURCE_REMOVE;
    }
    ai_completion_request_start(host, TRUE);
    return G_SOURCE_REMOVE;
}

static gboolean
ai_completion_auto_fix_cb(gpointer user_data)
{
    MtPluginHost *host;

    host = user_data;
    ai_auto_fix_source_id = 0;
    if (ai_session == NULL)
    {
        return G_SOURCE_REMOVE;
    }
    if (ai_candidate.text != NULL)
    {
        /* 行内补全候选优先：已有幽灵文本时不叠加纠错预览。 */
        return G_SOURCE_REMOVE;
    }
    if (ai_features_is_fix_visible())
    {
        return G_SOURCE_REMOVE;
    }
    if (!ai_completion_document_allowed(host, TRUE))
    {
        return G_SOURCE_REMOVE;
    }
    ai_features_fix_start(host);
    return G_SOURCE_REMOVE;
}

static void
ai_completion_auto_schedule(MtPluginHost *host)
{
    if (g_get_monotonic_time() < ai_reject_until_us)
    {
        /* 用户刚拒绝过候选：短时间内不再自动弹出，避免 "阴魂不散"。 */
        return;
    }
    if (!ai_completion_document_allowed(host, TRUE))
    {
        /* 自动补全只服务设置里勾选的文档类型：纯文本、对话或已取消勾选的
         * 格式不主动弹，否则模型会把 "你好" 续成一句问候，看起来像阴魂不散的聊天。 */
        return;
    }
    if (ai_auto_source_id != 0)
    {
        g_source_remove(ai_auto_source_id);
        ai_auto_source_id = 0;
    }
    if (ai_features_is_fix_visible())
    {
        /* 继续输入即撤销上一次空闲弹出的纠错预览。 */
        ai_features_clear();
    }
    if (!ai_features_cursor_safe(host))
    {
        /* 光标在标识符/字符串/注释中间时不自动弹出补全（手动仍可用）。 */
        return;
    }
    ai_auto_source_id = g_timeout_add(ai_features_adaptive_delay(),
                                      ai_completion_auto_cb,
                                      host);
    if (ai_auto_fix_enabled && ai_candidate.text == NULL &&
        !ai_features_is_fix_visible())
    {
        if (ai_auto_fix_source_id != 0)
        {
            g_source_remove(ai_auto_fix_source_id);
            ai_auto_fix_source_id = 0;
        }
        ai_auto_fix_source_id = g_timeout_add(ai_features_adaptive_delay() + 700,
                                              ai_completion_auto_fix_cb,
                                              host);
    }
}

gboolean
ai_completion_handle_key(MtPluginHost *host,
                         guint keyval,
                         guint keycode,
                         guint state,
                         gpointer user_data)
{
    gchar *current_context;

    (void)keycode;
    (void)user_data;
    if (ai_features_is_fix_visible())
    {
        if (keyval == GDK_KEY_Tab && state == 0)
        {
            /* 错误修复预览优先于补全候选：Tab 在光标处应用红/绿 diff。 */
            if (host->apply_inline_diff != NULL)
            {
                host->apply_inline_diff(host);
            }
            ai_features_clear();
            return TRUE;
        }
        if (keyval == GDK_KEY_Escape && state == 0)
        {
            /* Esc 关闭修复 diff。 */
            ai_features_clear();
            return TRUE;
        }
    }
    if (ai_candidate.text != NULL && ai_candidate.host == host)
    {
        current_context = host->get_text_before_cursor(host);
        if (g_strcmp0(current_context, ai_candidate.context) == 0)
        {
            if (keyval == GDK_KEY_Tab && state == 0)
            {
                gchar *accepted;

                /* 先移除文本视图覆盖层，再作为一次用户操作插入同一份候选文本。
                 * 避免 buffer 的 changed 回调与 GTK overlay 重绘交错造成半透明残影。
                 * 候选文本在展示时就已完成缩进对齐与后缀去重，接受即所见。 */
                accepted = g_strdup(ai_candidate.text);
                g_free(current_context);
                ai_features_stats_add_accepted();
                ai_completion_clear_candidate();
                host->insert_text(host, accepted);
                g_free(accepted);
                return TRUE;
            }
            if (keyval == GDK_KEY_Escape && state == 0)
            {
                g_free(current_context);
                ai_completion_reject_suppress();
                ai_completion_clear_candidate();
                return TRUE;
            }
        }
        g_free(current_context);
        ai_completion_reject_suppress();
        ai_completion_clear_candidate();
    }

    if (ai_auto_enabled && ai_completion_key_is_editing(keyval, state))
    {
        ai_completion_auto_schedule(host);
    }
    return FALSE;
}

void
ai_completion_request_start(MtPluginHost *host, gboolean automatic)
{
    GKeyFile *settings;
    gchar *endpoint;
    gchar *model;
    gchar *api_key;
    gchar *context;
    gchar *suffix;
    gchar *trimmed_context;
    gchar *trimmed_suffix;
    gboolean include_summary;
    gchar *body;
    gchar *authorization;
    gchar *summary;
    SoupMessage *message;
    GBytes *body_bytes;
    AiRequest *request;

    if (automatic && !ai_auto_enabled)
    {
        /* 自动补全已在 "偏好设置" 中关闭，忽略迟到的防抖回调。 */
        return;
    }
    if (!ai_completion_document_allowed(host, automatic))
    {
        if (!automatic)
        {
            host->show_toast(host, _("AI completion is disabled for this file type in the Extensions settings"));
        }
        return;
    }

    settings = ai_completion_load_settings();
    endpoint = ai_completion_get_setting(settings, "endpoint");
    model = ai_completion_get_setting(settings, "model");
    api_key = ai_completion_get_setting(settings, "api-key");
    g_key_file_unref(settings);

    /* 兼容旧版本已保存的 /v1 基础地址，实际请求始终指向补全端点。 */
    {
        gchar *normalized_endpoint;

        normalized_endpoint = ai_completion_normalize_endpoint(endpoint);
        g_free(endpoint);
        endpoint = normalized_endpoint;
    }

    if (*endpoint == '\0' || *model == '\0' || *api_key == '\0')
    {
        if (!automatic)
        {
            host->show_toast(host, _("Configure an AI endpoint, model and API key in Extensions first"));
        }
        g_free(endpoint);
        g_free(model);
        g_free(api_key);
        return;
    }

    context = host->get_text_before_cursor(host);
    {
        gchar *trimmed_check;

        trimmed_check = g_strdup(context);
        g_strstrip(trimmed_check);
        if (*trimmed_check == '\0')
        {
            g_free(trimmed_check);
            if (!automatic)
            {
                host->show_toast(host, _("Type some text before requesting AI completion"));
            }
            g_free(context);
            g_free(endpoint);
            g_free(model);
            g_free(api_key);
            return;
        }
        g_free(trimmed_check);
    }

    suffix = host->get_text_after_cursor != NULL ?
             host->get_text_after_cursor(host) : g_strdup("");
    trimmed_context = ai_completion_trim_context(context, AI_CONTEXT_LIMIT);
    trimmed_suffix = ai_completion_trim_context(suffix, AI_SUFFIX_LIMIT);
    if (automatic && ai_auto_context_loaded && g_strcmp0(trimmed_context, ai_auto_context) == 0)
    {
        /* 与上次自动请求相同的上下文，避免重复消耗额度。 */
        g_free(trimmed_suffix);
        g_free(trimmed_context);
        g_free(suffix);
        g_free(context);
        g_free(endpoint);
        g_free(model);
        g_free(api_key);
        return;
    }
    g_free(ai_auto_context);
    ai_auto_context = g_strdup(trimmed_context);
    ai_auto_context_loaded = TRUE;
    if (ai_force_no_summary)
        include_summary = FALSE;
    summary = include_summary ? ai_code_summary_get_current(host) : NULL;
    ai_force_no_summary = FALSE;
    {
        gchar *multifile = ai_completion_gather_context(host);

        body = ai_completion_build_body(model, trimmed_context, trimmed_suffix,
                                        summary, multifile);
        g_free(multifile);
    }
    g_free(summary);
    message = soup_message_new("POST", endpoint);
    if (message == NULL)
    {
        if (!automatic)
        {
            host->show_toast(host, _("AI endpoint URL is invalid"));
        }
        g_free(body);
        g_free(trimmed_context);
        g_free(trimmed_suffix);
        g_free(context);
        g_free(suffix);
        g_free(endpoint);
        g_free(model);
        g_free(api_key);
        return;
    }

    authorization = g_strdup_printf("Bearer %s", api_key);
    soup_message_headers_append(soup_message_get_request_headers(message), "Authorization", authorization);
    soup_message_headers_append(soup_message_get_request_headers(message), "Accept", "application/json");
    body_bytes = g_bytes_new_take(body, strlen(body));
    soup_message_set_request_body_from_bytes(message, "application/json", body_bytes);
    g_bytes_unref(body_bytes);

    if (ai_request_in_flight)
    {
        /* 新输入使旧请求作废：推进代际并中止会话，旧回调只释放 I/O。 */
        ai_generation++;
        soup_session_abort(ai_session);
    }
    ai_completion_clear_candidate();
    ai_generation++;
    request = g_new0(AiRequest, 1);
    request->host = host;
    request->message = g_object_ref(message);
    request->context = g_strdup(context);
    request->suffix = g_strdup(suffix);
    request->generation = ai_generation;
    request->attempt = ++ai_req_seq;
    request->automatic = automatic;
    ai_request_in_flight = TRUE;
    ai_features_stats_add_request();
    soup_session_send_async(ai_session,
                            message,
                            G_PRIORITY_DEFAULT,
                            NULL,
                            ai_completion_send_finished,
                            request);
    if (!automatic)
    {
        /* 不再弹 toast：补全以行内幽灵形式直接显示。 */
        /* 手动请求是用户的明确意图：取消拒绝抑制，让响应正常展示。 */
        ai_reject_until_us = 0;
    }

    g_object_unref(message);
    g_free(authorization);
    g_free(trimmed_context);
    g_free(trimmed_suffix);
    g_free(context);
    g_free(suffix);
    g_free(endpoint);
    g_free(model);
    g_free(api_key);
}

void
ai_completion_activate_action(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    MtPluginHost *host;

    (void)action;
    (void)parameter;

    host = user_data;
    ai_completion_auto_cancel();
    ai_completion_request_start(host, FALSE);
}