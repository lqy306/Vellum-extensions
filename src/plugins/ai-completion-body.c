/*
 * ai-completion-body.c
 * 请求正文、上下文收集、候选调整（缩进/后缀去重）、候选生命周期。
 */

#include "ai-completion-private.h"
#include "ai-completion-features.h"

#include <json-glib/json-glib.h>
#include <string.h>

gchar *
ai_completion_trim_context(const gchar *text, glong limit)
{
    glong characters;
    const gchar *start;

    characters = g_utf8_strlen(text, -1);
    if (characters <= limit)
    {
        return g_strdup(text);
    }

    start = g_utf8_offset_to_pointer(text, characters - limit);
    return g_strdup(start);
}

gchar *
ai_completion_build_body(const gchar *model, const gchar *prefix, const gchar *suffix, const gchar *summary, const gchar *multifile)
{
    JsonBuilder *builder;
    JsonGenerator *generator;
    JsonNode *root;
    gchar *body;
    gchar *prompt;

    if (suffix != NULL && *suffix != '\0')
    {
        /* 与真实补全工具一致的 fill-in-the-middle 思路：同时给出光标前后文，
         * 让模型只补中间段；聊天模型用标记符描述前缀与后缀。指令刻意强调
         * "不是对话"：聊天模型常把"你好"当开场白接一句问候。 */
        prompt = g_strdup_printf("This is not a chat. Continue the code or prose at the cursor. A local AI-maintained summary may appear before the code; treat it as context, not output. The text before the cursor is between <fim_prefix> and <fim_suffix>; the text after the cursor follows <fim_suffix>. Output only the missing middle that flows from the prefix toward the suffix. Never greet, never answer a question, never explain, never use Markdown or fences, never repeat text that already exists.\n\n<document_summary>\n%s\n</document_summary>\n<related_files>\n%s\n</related_files>\n<fim_prefix>\n%s\n<fim_suffix>\n%s\n<fim_middle>\n",
                                 summary != NULL ? summary : "",
                                 multifile != NULL ? multifile : "",
                                 prefix,
                                 suffix);
    }
    else
    {
        prompt = g_strdup_printf("This is not a chat. Continue the code or prose at the cursor. A local AI-maintained summary may appear before the code; treat it as context, not output. Output only the characters that should immediately follow the cursor, as if the file keeps going. Never greet, never answer a question, never explain, never use Markdown or fences, never repeat text that already exists.\n\n<document_summary>\n%s\n</document_summary>\n<related_files>\n%s\n</related_files>\n%s",
                                 summary != NULL ? summary : "",
                                 multifile != NULL ? multifile : "",
                                 prefix);
    }
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
                                  "You are an inline text completion engine inside a text editor, not a chat assistant. "
                                  "Your only output is the immediate continuation at the cursor: raw characters, no dialogue, "
                                  "no greetings, no question answering, no commentary, no Markdown, no repetition.");
    json_builder_end_object(builder);
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "role");
    json_builder_add_string_value(builder, "user");
    json_builder_set_member_name(builder, "content");
    json_builder_add_string_value(builder, prompt);
    json_builder_end_object(builder);
    json_builder_end_array(builder);
    /* 内联补全需要短、快的正文，不请求默认开启的思考链。 */
    json_builder_set_member_name(builder, "thinking");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "disabled");
    json_builder_end_object(builder);
    json_builder_set_member_name(builder, "temperature");
    json_builder_add_double_value(builder, 0.2);
    json_builder_set_member_name(builder, "max_tokens");
    json_builder_add_int_value(builder, 160);
    /* 流式输出：候选随 token 到达渐进显示；不支持的兼容服务会忽略此字段，
     * 插件对非 text/event-stream 的响应按普通 JSON 回退解析。 */
    json_builder_set_member_name(builder, "stream");
    json_builder_add_boolean_value(builder, TRUE);
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

gchar *
ai_completion_find_project_root(const gchar *file_path)
{
    static const gchar *markers[] = {
        ".git", "meson.build", "Cargo.toml", "package.json", "Makefile",
        "CMakeLists.txt", "pyproject.toml", ".vellum", "go.mod", NULL
    };
    gchar *dir;

    if (file_path == NULL || *file_path == '\0')
    {
        return NULL;
    }
    dir = g_path_get_dirname(file_path);
    for (;;)
    {
        gboolean found = FALSE;
        gint i;

        for (i = 0; markers[i] != NULL; i++)
        {
            gchar *candidate = g_build_filename(dir, markers[i], NULL);

            if (g_file_test(candidate, G_FILE_TEST_EXISTS))
            {
                found = TRUE;
            }
            g_free(candidate);
            if (found)
            {
                break;
            }
        }
        if (found)
        {
            return dir;
        }
        {
            gchar *parent = g_path_get_dirname(dir);

            if (g_strcmp0(parent, dir) == 0)
            {
                g_free(parent);
                g_free(dir);
                return NULL;
            }
            g_free(dir);
            dir = parent;
        }
    }
}

void
ai_completion_scan_project(const gchar *root, GString *ctx)
{
    static const gchar *exts[] = {
        ".c", ".h", ".cpp", ".cc", ".cxx", ".hpp", ".py", ".js", ".ts", ".java",
        ".go", ".rs", ".rb", ".php", ".cs", ".swift", ".kt", ".sh", ".lua", ".sql",
        ".json", ".yaml", ".yml", ".toml", ".md", NULL
    };
    GDir *dir;
    const gchar *name;
    guint files = 0;

    if (root == NULL)
    {
        return;
    }
    dir = g_dir_open(root, 0, NULL);
    if (dir == NULL)
    {
        return;
    }
    while ((name = g_dir_read_name(dir)) != NULL && files < 40 &&
           ctx->len < AI_MULTIFILE_LIMIT)
    {
        gchar *full;
        gint i;
        gboolean ok = FALSE;

        if (name[0] == '.')
        {
            continue;
        }
        for (i = 0; exts[i] != NULL; i++)
        {
            if (g_str_has_suffix(name, exts[i]))
            {
                ok = TRUE;
                break;
            }
        }
        if (!ok)
        {
            continue;
        }
        full = g_build_filename(root, name, NULL);
        if (g_file_test(full, G_FILE_TEST_IS_REGULAR))
        {
            gchar *content = NULL;

            if (g_file_get_contents(full, &content, NULL, NULL) && content != NULL)
            {
                g_string_append_printf(ctx, "\n--- %s ---\n", full);
                g_string_append(ctx, content);
                g_free(content);
                files++;
            }
        }
        g_free(full);
    }
    g_dir_close(dir);
}

/* 按设置装配多文件上下文：0=当前文件 1=已打开文件 2=项目目录。
 * 返回新分配字符串（可能为空），调用者 g_free。 */
gchar *
ai_completion_gather_context(MtPluginHost *host)
{
    GKeyFile *settings;
    gint mode;
    GString *ctx;
    gchar *result = NULL;

    settings = ai_completion_load_settings();
    mode = g_key_file_get_integer(settings, "AI Completion", "context-mode", NULL);
    g_key_file_unref(settings);
    if (mode != 1 && mode != 2)
    {
        return NULL;
    }

    ctx = g_string_new(NULL);
    if (mode == 1 && host->get_open_documents != NULL)
    {
        gsize count = 0;
        gchar **paths = host->get_open_documents(host, &count);

        if (paths != NULL)
        {
            for (gsize i = 0; paths[i] != NULL && ctx->len < AI_MULTIFILE_LIMIT; i++)
            {
                gchar *content = NULL;

                if (g_file_get_contents(paths[i], &content, NULL, NULL) && content != NULL)
                {
                    g_string_append_printf(ctx, "\n--- %s ---\n", paths[i]);
                    g_string_append(ctx, content);
                    g_free(content);
                }
            }
            g_strfreev(paths);
        }
    }
    else if (mode == 2)
    {
        gchar *cur = host->get_current_file_path != NULL ?
                     host->get_current_file_path(host) : NULL;
        gchar *root = ai_completion_find_project_root(cur);

        if (root != NULL)
        {
            ai_completion_scan_project(root, ctx);
            g_free(root);
        }
        g_free(cur);
    }

    if (ctx->len > 0)
    {
        result = g_string_free(ctx, FALSE);
    }
    else
    {
        g_string_free(ctx, TRUE);
    }
    return result;
}

static gchar *
ai_completion_extract_content_from_node(JsonNode *content_node)
{
    const gchar *content;

    content = NULL;
    if (content_node == NULL)
    {
        return NULL;
    }
    if (JSON_NODE_HOLDS_VALUE(content_node) &&
        json_node_get_value_type(content_node) == G_TYPE_STRING)
    {
        content = json_node_get_string(content_node);
    }
    else if (JSON_NODE_HOLDS_ARRAY(content_node))
    {
        /* 部分 OpenAI 兼容服务把 content 返回为 [{ "type": "text", "text": "..." }] */
        JsonArray *parts;
        guint part_index;

        parts = json_node_get_array(content_node);
        for (part_index = 0; part_index < json_array_get_length(parts); part_index++)
        {
            JsonNode *part_node;

            part_node = json_array_get_element(parts, part_index);
            if (!JSON_NODE_HOLDS_OBJECT(part_node))
            {
                continue;
            }
            if (json_object_has_member(json_node_get_object(part_node), "text"))
            {
                JsonNode *text_node;

                text_node = json_object_get_member(json_node_get_object(part_node), "text");
                if (JSON_NODE_HOLDS_VALUE(text_node) &&
                    json_node_get_value_type(text_node) == G_TYPE_STRING)
                {
                    content = json_node_get_string(text_node);
                    if (content != NULL && *content != '\0')
                    {
                        break;
                    }
                }
            }
        }
    }

    return content != NULL && *content != '\0' ? g_strdup(content) : NULL;
}

/* 从单个 choice 对象里取出补全文本；SSE 的 delta 与普通响应的 message 共用。 */
gchar *
ai_completion_extract_choice_content(JsonObject *choice)
{
    JsonObject *message;
    JsonNode *content_node;
    gchar *content;

    content = NULL;
    if (choice == NULL)
    {
        return NULL;
    }
    if (json_object_has_member(choice, "message"))
    {
        message = json_object_get_object_member(choice, "message");
        if (message != NULL && json_object_has_member(message, "content"))
        {
            content_node = json_object_get_member(message, "content");
            content = ai_completion_extract_content_from_node(content_node);
        }
    }
    if (content == NULL && json_object_has_member(choice, "delta"))
    {
        message = json_object_get_object_member(choice, "delta");
        if (message != NULL && json_object_has_member(message, "content"))
        {
            content_node = json_object_get_member(message, "content");
            content = ai_completion_extract_content_from_node(content_node);
        }
    }
    if (content == NULL && json_object_has_member(choice, "text"))
    {
        content = ai_completion_extract_content_from_node(json_object_get_member(choice, "text"));
    }

    return content;
}

gchar *
ai_completion_extract_content(const gchar *response, gsize length, GError **error)
{
    JsonParser *parser;
    JsonNode *root;
    JsonObject *object;
    JsonArray *choices;
    JsonObject *choice;
    gchar *content;
    gchar *result;

    if (response == NULL || length == 0)
    {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "AI response is empty");
        return NULL;
    }

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
    if (!json_object_has_member(object, "choices"))
    {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "AI response has no choices array");
        g_object_unref(parser);
        return NULL;
    }

    choices = json_object_get_array_member(object, "choices");
    if (json_array_get_length(choices) == 0)
    {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "AI response has an empty choices array");
        g_object_unref(parser);
        return NULL;
    }

    choice = json_array_get_object_element(choices, 0);
    content = ai_completion_extract_choice_content(choice);
    if (content == NULL)
    {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "AI response contains no completion text");
        g_object_unref(parser);
        return NULL;
    }

    result = content;
    g_object_unref(parser);

    return result;
}

gchar *
ai_completion_error_detail_data(const gchar *data, gsize length)
{
    if (data == NULL || length == 0)
    {
        return g_strdup("");
    }

    return g_strndup(data, MIN(length, (gsize)160));
}

/* 预览首行：宿主 overlay 是光标后的单行标签，多行补全只能显示第一行。
 * 跳过开头的空行，直到找到第一条非空逻辑行。 */
gchar *
ai_completion_prepare_inline_candidate(const gchar *completion)
{
    gchar *trimmed;
    const gchar *p;

    if (completion == NULL)
    {
        return NULL;
    }

    /* 保留完整多行：流式补全过程中整段函数体会逐行长出，宿主负责多行渲染。
     * 仅裁掉尾部空白（含换行），全部为空白时视为尚无内容、继续累积。 */
    trimmed = g_strchomp(g_strdup(completion));
    if (*trimmed == '\0')
    {
        g_free(trimmed);
        return NULL;
    }
    for (p = trimmed; *p != '\0'; p++)
    {
        if (!g_ascii_isspace((guchar)*p))
        {
            break;
        }
    }
    if (*p == '\0')
    {
        g_free(trimmed);
        return NULL;
    }

    return trimmed;
}

/* 当前行（最后一个换行之后）只输入了空白时，补全开头的空白与已输入空白
 * 重叠的部分会被裁掉，避免 Tab 接受后把缩进重复一遍。刚回车（当前行为空）
 * 时缩进来自补全本身，不做裁剪——与 copilot.vim 的缩进调整一致。 */
gchar *
ai_completion_outdent(const gchar *completion, const gchar *context)
{
    const gchar *line_start;
    const gchar *walk;
    const gchar *leading_start;
    gsize leading_length;
    gsize typed_length;

    if (completion == NULL)
    {
        return NULL;
    }
    if (context == NULL)
    {
        return g_strdup(completion);
    }

    line_start = strrchr(context, '\n');
    line_start = line_start != NULL ? line_start + 1 : context;
    if (*line_start == '\0')
    {
        return g_strdup(completion);
    }
    for (walk = line_start; *walk != '\0'; walk++)
    {
        if (*walk != ' ' && *walk != '\t')
        {
            return g_strdup(completion);
        }
    }

    leading_start = completion;
    while (*leading_start == ' ' || *leading_start == '\t')
    {
        leading_start++;
    }
    leading_length = (gsize)(leading_start - completion);
    if (leading_length == 0)
    {
        return g_strdup(completion);
    }

    typed_length = strlen(line_start);
    if (typed_length < leading_length)
    {
        return g_strdup(completion);
    }
    if (strncmp(line_start, completion, leading_length) != 0)
    {
        return g_strdup(completion);
    }

    return g_strdup(completion + leading_length);
}

/* 接受时应用 Continue 的 SuffixOverlap：裁掉补全结尾与光标后已存在文本
 * 开头重叠的部分，避免接受后把已有内容重复一遍。 */
gchar *
ai_completion_trim_suffix_overlap(const gchar *completion, const gchar *suffix)
{
    gsize completion_length;
    gsize suffix_length;
    gsize largest_overlap;
    gsize i;

    if (completion == NULL)
    {
        return NULL;
    }
    if (suffix == NULL || *suffix == '\0')
    {
        return g_strdup(completion);
    }

    completion_length = strlen(completion);
    suffix_length = strlen(suffix);
    largest_overlap = 0;
    for (i = 1; i <= MIN(completion_length, suffix_length); i++)
    {
        if (strncmp(completion + completion_length - i, suffix, i) == 0)
        {
            largest_overlap = i;
        }
    }
    if (largest_overlap == 0)
    {
        return g_strdup(completion);
    }

    return g_strndup(completion, completion_length - largest_overlap);
}

/* 候选最终文本：出缩进 + 去尾部换行 + 后缀去重。展示与接受使用同一份文本，
 * 保证 "看到什么就插入什么"。 */
gchar *
ai_completion_adjust_candidate(const gchar *completion,
                               const gchar *context,
                               const gchar *suffix)
{
    gchar *outdented;
    gchar *stripped;
    gchar *trimmed;
    gsize length;

    outdented = ai_completion_outdent(completion, context);
    stripped = g_strdup(outdented != NULL ? outdented : "");
    g_free(outdented);
    length = strlen(stripped);
    while (length > 0 && (stripped[length - 1] == '\n' || stripped[length - 1] == '\r'))
    {
        stripped[--length] = '\0';
    }
    trimmed = ai_completion_trim_suffix_overlap(stripped, suffix);
    g_free(stripped);

    return trimmed;
}

void
ai_completion_clear_candidate(void)
{
    if (ai_candidate_timeout_source_id != 0)
    {
        g_source_remove(ai_candidate_timeout_source_id);
        ai_candidate_timeout_source_id = 0;
    }
    if (ai_candidate.host != NULL && ai_candidate.host->clear_inline_completion != NULL)
    {
        ai_candidate.host->clear_inline_completion(ai_candidate.host);
    }
    g_clear_pointer(&ai_candidate.text, g_free);
    g_clear_pointer(&ai_candidate.context, g_free);
    g_clear_pointer(&ai_candidate.suffix, g_free);
    ai_candidate.host = NULL;
    ai_features_clear();
}

void
ai_completion_reject_suppress(void)
{
    ai_reject_until_us = g_get_monotonic_time() +
                         AI_REJECT_SUPPRESS_MILLISECONDS * 1000;
}

void
ai_completion_auto_cancel(void)
{
    if (ai_auto_source_id != 0)
    {
        g_source_remove(ai_auto_source_id);
        ai_auto_source_id = 0;
    }
    if (ai_auto_fix_source_id != 0)
    {
        g_source_remove(ai_auto_fix_source_id);
        ai_auto_fix_source_id = 0;
    }
}

gboolean
ai_completion_candidate_timeout(gpointer user_data)
{
    MtPluginHost *host;

    host = user_data;
    ai_candidate_timeout_source_id = 0;
    if (ai_candidate.host == host)
    {
        ai_completion_clear_candidate();
    }
    return G_SOURCE_REMOVE;
}