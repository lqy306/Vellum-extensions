/*
 * ai-completion-settings.c
 * 设置/配置数据：配置文件路径、读写、端点归一化、文档类型开关、保存逻辑。
 */

#include "ai-completion-private.h"

#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <string.h>

gchar *
ai_completion_config_path(void)
{
    gchar *directory;
    gchar *path;

    directory = g_build_filename(g_get_user_config_dir(), "vellum", NULL);
    g_mkdir_with_parents(directory, 0700);
    path = g_build_filename(directory, "ai-completion.ini", NULL);
    g_free(directory);

    return path;
}

GKeyFile *
ai_completion_load_settings(void)
{
    GKeyFile *settings;
    gchar *path;

    settings = g_key_file_new();
    path = ai_completion_config_path();
    g_key_file_load_from_file(settings, path, G_KEY_FILE_NONE, NULL);
    g_free(path);

    return settings;
}

gchar *
ai_completion_get_setting(GKeyFile *settings, const gchar *key)
{
    gchar *value;

    value = g_key_file_get_string(settings, AI_COMPLETION_GROUP, key, NULL);
    if (value == NULL)
    {
        value = g_strdup("");
    }

    return value;
}

gboolean
ai_completion_get_include_summary(GKeyFile *settings)
{
    GError *error;
    gboolean value;

    error = NULL;
    value = g_key_file_get_boolean(settings, AI_COMPLETION_GROUP, "include-summary", &error);
    if (error != NULL)
    {
        g_clear_error(&error);
        return TRUE;
    }

    return value;
}

gboolean
ai_completion_get_auto_enabled(GKeyFile *settings)
{
    GError *error;
    gboolean value;

    error = NULL;
    value = g_key_file_get_boolean(settings, AI_COMPLETION_GROUP, "auto-complete", &error);
    if (error != NULL)
    {
        g_clear_error(&error);
        return TRUE;
    }

    return value;
}

void
ai_completion_set_auto_enabled(gboolean enabled)
{
    GKeyFile *settings;
    gchar *path;
    gchar *contents;

    settings = ai_completion_load_settings();
    g_key_file_set_boolean(settings, AI_COMPLETION_GROUP, "auto-complete", enabled);
    contents = g_key_file_to_data(settings, NULL, NULL);
    if (contents != NULL)
    {
        path = ai_completion_config_path();
        if (g_file_set_contents(path, contents, (gssize)strlen(contents), NULL))
        {
            g_chmod(path, 0600);
        }
        g_free(path);
        g_free(contents);
    }
    g_key_file_unref(settings);
}

/* 解析 "disabled-languages" 的分号分隔列表；空值或缺失表示全部启用。 */
GHashTable *
ai_completion_parse_disabled_languages(const gchar *value)
{
    GHashTable *disabled;
    gchar **parts;
    gchar **part;

    disabled = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    if (value == NULL || *value == '\0')
    {
        return disabled;
    }
    parts = g_strsplit(value, ";", -1);
    for (part = parts; part != NULL && *part != NULL; part++)
    {
        gchar *trimmed;

        trimmed = g_strdup(*part);
        g_strstrip(trimmed);
        if (*trimmed != '\0')
        {
            g_hash_table_add(disabled, trimmed);
        }
        else
        {
            g_free(trimmed);
        }
    }
    g_strfreev(parts);

    return disabled;
}

/* 把禁用集合序列化为分号分隔的字符串（按 id 排序，便于比较与存储）。 */
gchar *
ai_completion_join_disabled_languages(GHashTable *disabled)
{
    GString *joined;
    GList *ids;
    GList *iter;

    if (disabled == NULL)
    {
        return g_strdup("");
    }
    joined = g_string_new(NULL);
    ids = g_list_sort(g_hash_table_get_keys(disabled), (GCompareFunc)g_strcmp0);
    for (iter = ids; iter != NULL; iter = iter->next)
    {
        if (joined->len > 0)
        {
            g_string_append_c(joined, ';');
        }
        g_string_append(joined, iter->data);
    }
    g_list_free(ids);

    return g_string_free(joined, FALSE);
}

void
ai_completion_languages_set(GHashTable *disabled)
{
    g_clear_pointer(&ai_disabled_languages, g_hash_table_unref);
    ai_disabled_languages = disabled;
}

/* 从设置重建禁用集合缓存；插件激活与配置保存后调用。 */
void
ai_completion_languages_load(GKeyFile *settings)
{
    gchar *value;

    value = ai_completion_get_setting(settings, "disabled-languages");
    ai_completion_languages_set(ai_completion_parse_disabled_languages(value));
    g_free(value);
}

static gboolean
ai_completion_language_is_disabled(const gchar *language_id)
{
    return language_id != NULL && ai_disabled_languages != NULL &&
           g_hash_table_contains(ai_disabled_languages, language_id);
}

/* 当前文档是否允许 AI 补全：语言已知时按设置的类型开关判断；
 * 语言未知（纯文本或系统未装语言定义）时，自动补全仍只服务代码文档，
 * 手动请求保持可用，与历史行为一致。 */
gboolean
ai_completion_document_allowed(MtPluginHost *host, gboolean automatic)
{
    const gchar *language_id;

    if (host->get_document_language_id != NULL)
    {
        language_id = host->get_document_language_id(host);
        if (language_id != NULL && *language_id != '\0')
        {
            return !ai_completion_language_is_disabled(language_id);
        }
        return !automatic;
    }
    if (host->get_is_code_document != NULL && automatic)
    {
        return host->get_is_code_document(host);
    }

    return TRUE;
}

gchar *
ai_completion_normalize_endpoint(const gchar *endpoint)
{
    gchar *normalized;
    GUri *uri;
    const gchar *path;
    gsize length;

    normalized = g_strdup(endpoint != NULL ? endpoint : "");
    g_strstrip(normalized);

    /* 统一去掉末尾斜杠，便于后续后缀匹配。 */
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

    /* 裸主机地址（如 https://api.deepseek.com 或 https://api.deepseek.com/）
     * 自动补上补全端点，与主流 OpenAI 兼容客户端一致。 */
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

gboolean
ai_completion_save_settings(const gchar *endpoint,
                            const gchar *model,
                            const gchar *api_key,
                            gboolean auto_enabled,
                            GListStore *language_items,
                            AiCodeSummaryConfigWidgets *summary_widgets,
                            GError **error)
{
    GKeyFile *settings;
    gchar *path;
    gchar *contents;
    gsize length;
    gboolean saved;
    GHashTable *disabled_languages;
    gchar *disabled_value;

    gchar *normalized_endpoint;

    normalized_endpoint = ai_completion_normalize_endpoint(endpoint);
    disabled_languages = ai_completion_disabled_from_items(language_items);
    disabled_value = ai_completion_join_disabled_languages(disabled_languages);
    settings = g_key_file_new();
    g_key_file_set_string(settings, AI_COMPLETION_GROUP, "endpoint", normalized_endpoint);
    g_key_file_set_string(settings, AI_COMPLETION_GROUP, "model", model);
    g_key_file_set_string(settings, AI_COMPLETION_GROUP, "api-key", api_key);
    g_key_file_set_boolean(settings, AI_COMPLETION_GROUP, "auto-complete", auto_enabled);
    g_key_file_set_string(settings, AI_COMPLETION_GROUP, "disabled-languages", disabled_value);
    ai_code_summary_config_save(settings, summary_widgets);
    contents = g_key_file_to_data(settings, &length, error);

    if (contents == NULL)
    {
        g_free(disabled_value);
        g_hash_table_unref(disabled_languages);
        g_free(normalized_endpoint);
        g_key_file_unref(settings);
        return FALSE;
    }

    path = ai_completion_config_path();
    saved = g_file_set_contents(path, contents, (gssize)length, error);
    if (saved)
    {
        g_chmod(path, 0600);
        /* 让运行中的插件立即使用新选择，无需重启。 */
        ai_completion_languages_set(disabled_languages);
        disabled_languages = NULL;
    }

    g_free(disabled_value);
    if (disabled_languages != NULL)
    {
        g_hash_table_unref(disabled_languages);
    }
    g_free(path);
    g_free(contents);
    g_free(normalized_endpoint);
    g_key_file_unref(settings);

    return saved;
}