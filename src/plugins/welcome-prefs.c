/*
 * welcome-prefs.c
 * 新手引导的语言检测、安装偏好、构建环境检测与自定义软件源。
 */

#include "welcome-private.h"

#include <glib/gstdio.h>
#include <sys/utsname.h>

gboolean
welcome_is_zh(void)
{
    const gchar *language_env;
    const gchar * const *languages;

    /* 优先尊重 Vellum 首选项设置的 LANGUAGE，其次系统语言，避免中英混杂 */
    language_env = g_getenv("LANGUAGE");
    if (language_env != NULL && *language_env != '\0')
    {
        if (g_str_has_prefix(language_env, "zh"))
            return TRUE;
        if (g_str_has_prefix(language_env, "en"))
            return FALSE;
    }
    languages = g_get_language_names();
    return languages != NULL && languages[0] != NULL &&
           g_str_has_prefix(languages[0], "zh");
}

const gchar *
welcome_text(const gchar *zh, const gchar *en)
{
    return welcome_is_zh() ? zh : en;
}

gchar *
welcome_flag_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "vellum", "welcome-guide-shown", NULL);
}

gchar *
welcome_install_pref_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "vellum", "install-pref.ini", NULL);
}

gchar *
welcome_market_sources_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "vellum", "market-sources.ini", NULL);
}

gboolean
welcome_is_x64_arch(void)
{
    struct utsname info;

    if (uname(&info) != 0)
    {
        return FALSE;
    }
    return g_strcmp0(info.machine, "x86_64") == 0 ||
           g_strcmp0(info.machine, "amd64") == 0;
}

gboolean
welcome_load_install_pref(void)
{
    GKeyFile *key_file;
    gchar *path;
    gboolean prefer_source;
    GError *error;

    /* 默认：x64 用二进制，非 x64 只能源码 */
    prefer_source = !welcome_is_x64_arch();
    path = welcome_install_pref_path();
    key_file = g_key_file_new();
    error = NULL;
    if (g_key_file_load_from_file(key_file, path, G_KEY_FILE_NONE, &error))
    {
        /* 兼容历史：若文件存在则读取 */
        prefer_source = g_key_file_get_boolean(key_file, "Install", "prefer-source", NULL);
    }
    g_clear_error(&error);
    g_key_file_unref(key_file);
    g_free(path);
    return prefer_source;
}

void
welcome_save_install_pref(gboolean prefer_source)
{
    GKeyFile *key_file;
    gchar *path;
    gchar *directory;
    gchar *contents;
    gsize length;
    GError *error;

    key_file = g_key_file_new();
    path = welcome_install_pref_path();
    /* 尽量保留已有市场源配置，不覆盖 */
    g_key_file_load_from_file(key_file, path, G_KEY_FILE_NONE, NULL);
    g_key_file_set_boolean(key_file, "Install", "prefer-source", prefer_source);
    /* 同时记录选择时间，便于后续诊断 */
    g_key_file_set_string(key_file, "Install", "arch", welcome_is_x64_arch() ? "x86_64" : "other");
    error = NULL;
    contents = g_key_file_to_data(key_file, &length, &error);
    if (contents != NULL)
    {
        directory = g_path_get_dirname(path);
        g_mkdir_with_parents(directory, 0700);
        g_file_set_contents(path, contents, (gssize)length, NULL);
        g_chmod(path, 0600);
        g_free(directory);
        g_free(contents);
    }
    g_clear_error(&error);
    g_key_file_unref(key_file);
    g_free(path);
}

gchar *
welcome_check_build_env(gboolean *out_ready)
{
    const gchar *tools[] = { "make", "cc", "pkg-config", NULL };
    const gchar *pkgs[] = {
        "gtk4",
        "libadwaita-1",
        "gtksourceview-5",
        "gio-2.0",
        "gmodule-2.0",
        "libsoup-3.0",
        "json-glib-1.0",
        NULL
    };
    GString *missing;
    gboolean ready;
    guint i;

    missing = g_string_new(NULL);
    ready = TRUE;

    for (i = 0; tools[i] != NULL; i++)
    {
        gchar *found;

        found = g_find_program_in_path(tools[i]);
        if (found == NULL && g_strcmp0(tools[i], "cc") == 0)
        {
            /* cc 不存在时尝试 gcc */
            found = g_find_program_in_path("gcc");
        }
        if (found == NULL)
        {
            if (missing->len > 0)
                g_string_append(missing, ", ");
            g_string_append(missing, tools[i]);
            ready = FALSE;
        }
        else
        {
            g_free(found);
        }
    }

    /* 仅当基础工具齐全时再检查 pkg-config 依赖 */
    if (ready)
    {
        gchar *pkg_config;

        pkg_config = g_find_program_in_path("pkg-config");
        if (pkg_config != NULL)
        {
            for (i = 0; pkgs[i] != NULL; i++)
            {
                gchar *command;
                gint exit_status;

                command = g_strdup_printf("pkg-config --exists %s", pkgs[i]);
                if (!g_spawn_command_line_sync(command, NULL, NULL, &exit_status, NULL) ||
                    !g_spawn_check_exit_status(exit_status, NULL))
                {
                    if (missing->len > 0)
                        g_string_append(missing, ", ");
                    g_string_append(missing, pkgs[i]);
                    ready = FALSE;
                }
                g_free(command);
                if (!ready && missing->len > 80)
                    break;
            }
            g_free(pkg_config);
        }
    }

    if (out_ready != NULL)
        *out_ready = ready;

    if (ready)
    {
        g_string_free(missing, TRUE);
        return g_strdup(welcome_text("环境就绪：可直接编译源码扩展",
                                     "Environment ready: source extensions can be built"));
    }
    else
    {
        gchar *details;

        details = g_string_free(missing, FALSE);
        return g_strdup_printf(welcome_text("缺少：%s", "Missing: %s"), details);
    }
}

gchar *
welcome_get_market_sources_display(void)
{
    GKeyFile *key_file;
    gchar *path;
    gchar *value;
    GError *error;

    path = welcome_market_sources_path();
    key_file = g_key_file_new();
    error = NULL;
    value = NULL;
    if (g_key_file_load_from_file(key_file, path, G_KEY_FILE_NONE, &error))
    {
        value = g_key_file_get_string(key_file, "Sources", "urls", NULL);
    }
    g_clear_error(&error);
    g_key_file_unref(key_file);
    g_free(path);
    if (value == NULL || *value == '\0')
    {
        g_free(value);
        return g_strdup(welcome_text("（当前仅使用默认官方源）",
                                     "(currently using default official source only)"));
    }
    return value;
}

gboolean
welcome_add_market_source(const gchar *url, GError **error)
{
    GKeyFile *key_file;
    gchar *path;
    gchar *existing;
    gchar **parts;
    GPtrArray *urls;
    guint i;
    gboolean found;
    gchar *joined;
    gchar *contents;
    gsize length;

    if (url == NULL || *url == '\0')
    {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "%s", welcome_text("请输入有效的 URL", "Please enter a valid URL"));
        return FALSE;
    }
    if (!g_str_has_prefix(url, "http://") && !g_str_has_prefix(url, "https://"))
    {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "%s", welcome_text("URL 需以 http:// 或 https:// 开头",
                                       "URL must start with http:// or https://"));
        return FALSE;
    }

    path = welcome_market_sources_path();
    key_file = g_key_file_new();
    g_key_file_load_from_file(key_file, path, G_KEY_FILE_NONE, NULL);
    existing = g_key_file_get_string(key_file, "Sources", "urls", NULL);
    urls = g_ptr_array_new_with_free_func(g_free);
    found = FALSE;
    if (existing != NULL && *existing != '\0')
    {
        parts = g_strsplit(existing, ";", -1);
        for (i = 0; parts[i] != NULL; i++)
        {
            gchar *trimmed;

            trimmed = g_strdup(parts[i]);
            g_strstrip(trimmed);
            if (*trimmed == '\0')
            {
                g_free(trimmed);
                continue;
            }
            if (g_strcmp0(trimmed, url) == 0)
                found = TRUE;
            g_ptr_array_add(urls, trimmed);
        }
        g_strfreev(parts);
    }
    g_free(existing);
    if (!found)
    {
        g_ptr_array_add(urls, g_strdup(url));
    }
    else
    {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                    "%s", welcome_text("该源已存在", "Source already exists"));
        g_ptr_array_unref(urls);
        g_key_file_unref(key_file);
        g_free(path);
        return FALSE;
    }

    /* 重新拼接 */
    joined = NULL;
    if (urls->len > 0)
    {
        GString *builder;

        builder = g_string_new(NULL);
        for (i = 0; i < urls->len; i++)
        {
            if (i > 0)
                g_string_append(builder, ";");
            g_string_append(builder, g_ptr_array_index(urls, i));
        }
        joined = g_string_free(builder, FALSE);
    }
    g_ptr_array_unref(urls);
    g_key_file_set_string(key_file, "Sources", "urls", joined != NULL ? joined : "");
    g_free(joined);
    contents = g_key_file_to_data(key_file, &length, error);
    if (contents == NULL)
    {
        g_key_file_unref(key_file);
        g_free(path);
        return FALSE;
    }
    {
        gchar *directory;

        directory = g_path_get_dirname(path);
        g_mkdir_with_parents(directory, 0700);
        g_free(directory);
    }
    if (!g_file_set_contents(path, contents, (gssize)length, error))
    {
        g_free(contents);
        g_key_file_unref(key_file);
        g_free(path);
        return FALSE;
    }
    g_chmod(path, 0600);
    g_free(contents);
    g_key_file_unref(key_file);
    g_free(path);
    return TRUE;
}