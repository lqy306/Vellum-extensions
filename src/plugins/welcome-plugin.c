/*
 * welcome-plugin.c
 * Vellum 的分关卡新手引导。引导把首次输入与扩展选择拆成明确、可完成的交互，
 * 每个扩展的介绍信息由独立 INI 配置提供，便于新增扩展时无需改写引导流程。
 * 第二关新增：源码/二进制安装方式选择、x86_64 架构检测、多发行版环境说明、
 * 本地构建环境检测与自定义软件源配置。
 */

#include "mt-plugin.h"

#include <adwaita.h>
#include <gmodule.h>
#include <glib/gstdio.h>
#include <sys/utsname.h>

#ifndef VELLUM_EXTENSION_INTRO_DIR
#define VELLUM_EXTENSION_INTRO_DIR "/usr/local/share/vellum/extension-intros"
#endif

typedef struct _WelcomeExtensionIntro WelcomeExtensionIntro;
typedef struct _MtWelcomeData MtWelcomeData;

struct _WelcomeExtensionIntro
{
    gchar *id;
    gchar *icon;
    gchar *title;
    gchar *description;
    gchar *lesson;
};

struct _MtWelcomeData
{
    MtPluginHost *host;
    gchar *plugin_id;
    GtkWidget *guide_window;
    GtkStack *pages;
    GtkLabel *progress_label;
    GtkButton *back_button;
    GtkButton *next_button;
    GPtrArray *extension_buttons;
    guint page;
    guint show_source_id;
    guint remove_source_id;
    gboolean guide_complete;
    /* 新增：安装方式与环境检测 */
    GtkCheckButton *binary_radio;
    GtkCheckButton *source_radio;
    GtkLabel *arch_label;
    GtkWidget *source_detail_box;
    GtkLabel *env_status_label;
    GtkButton *check_env_button;
    AdwEntryRow *custom_source_entry;
    GtkLabel *custom_source_status;
    gboolean is_x64;
    gboolean prefer_source;
    /* 热更新安装队列 */
    GPtrArray *pending_install_ids;
    guint install_index;
    guint install_success;
    guint install_failed;
};

static MtWelcomeData *welcome_data;

static void welcome_remove_response(AdwMessageDialog *dialog,
                                    const gchar *response,
                                    gpointer user_data);
static void welcome_install_mode_toggled(GtkCheckButton *button,
                                         gpointer user_data);
static void welcome_check_env_clicked(GtkButton *button,
                                      gpointer user_data);
static void welcome_custom_source_add_clicked(GtkButton *button,
                                              gpointer user_data);
static void welcome_install_next(MtWelcomeData *data);
static void welcome_install_done(GObject *source,
                                 GAsyncResult *result,
                                 gpointer user_data);
static void welcome_close_guide(MtWelcomeData *data);
static void welcome_ask_remove(MtWelcomeData *data);

static const gchar * const welcome_extension_files[] = {
    "ai-completion.ini",
    "timestamp.ini",
    "word-count.ini",
    "link-check.ini",
    "project-sidebar.ini",
    "build-run.ini",
    "vim-mode.ini",
    "screenshot.ini",
    NULL
};

static gboolean
welcome_is_zh(void)
{
    const gchar * const *languages;

    languages = g_get_language_names();
    return languages != NULL && languages[0] != NULL &&
           g_str_has_prefix(languages[0], "zh");
}

static const gchar *
welcome_text(const gchar *zh, const gchar *en)
{
    return welcome_is_zh() ? zh : en;
}

static gchar *
welcome_flag_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "vellum", "welcome-guide-shown", NULL);
}

static gchar *
welcome_install_pref_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "vellum", "install-pref.ini", NULL);
}

static gchar *
welcome_market_sources_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "vellum", "market-sources.ini", NULL);
}

static gboolean
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

static gboolean
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

static void
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

static gchar *
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

static gchar *
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

static gboolean
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

static void
welcome_install_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
    MtWelcomeData *data;
    GError *error;

    (void)source;
    data = user_data;
    error = NULL;
    if (data->host->install_extension_finish != NULL &&
        !data->host->install_extension_finish(data->host, result, &error))
    {
        data->install_failed++;
        g_message("Welcome install failed for '%s': %s",
                  (const gchar *)g_ptr_array_index(data->pending_install_ids, data->install_index - 1),
                  error != NULL ? error->message : "unknown");
        g_clear_error(&error);
    }
    else
    {
        data->install_success++;
    }
    welcome_install_next(data);
}

static void
welcome_install_next(MtWelcomeData *data)
{
    const gchar *plugin_id;

    if (data->pending_install_ids == NULL ||
        data->install_index >= data->pending_install_ids->len)
    {
        /* 全部完成：热更新已生效，无需重启 */
        gchar *message;

        if (data->install_success > 0 || data->install_failed > 0)
        {
            message = g_strdup_printf(welcome_text("已安装 %u 个扩展，失败 %u 个（热更新，无需重启）",
                                                   "Installed %u extensions, %u failed (hot update, no restart needed)"),
                                      data->install_success, data->install_failed);
            if (data->host->show_toast != NULL)
                data->host->show_toast(data->host, message);
            g_free(message);
        }
        /* 清理并进入下一阶段 */
        g_clear_pointer(&data->pending_install_ids, g_ptr_array_unref);
        data->guide_complete = TRUE;
        welcome_close_guide(data);
        welcome_ask_remove(data);
        return;
    }

    plugin_id = g_ptr_array_index(data->pending_install_ids, data->install_index);
    data->install_index++;

    /* 更新按钮为安装中 */
    if (data->next_button != NULL)
    {
        gchar *label;

        label = g_strdup_printf(welcome_text("正在安装 %u/%u…", "Installing %u/%u…"),
                                data->install_index, (guint)data->pending_install_ids->len);
        gtk_button_set_label(data->next_button, label);
        g_free(label);
        gtk_widget_set_sensitive(GTK_WIDGET(data->next_button), FALSE);
        gtk_widget_set_sensitive(GTK_WIDGET(data->back_button), FALSE);
    }

    if (data->host->install_extension_async != NULL)
    {
        data->host->install_extension_async(data->host, plugin_id, data->prefer_source,
                                            NULL, welcome_install_done, data);
    }
    else
    {
        g_warning("Host does not support install_extension_async");
        welcome_install_next(data);
    }
}

static void
welcome_start_installs(MtWelcomeData *data)
{
    guint i;

    /* 收集勾选且尚未安装的扩展 */
    data->pending_install_ids = g_ptr_array_new_with_free_func(g_free);
    for (i = 0; i < data->extension_buttons->len; i++)
    {
        GtkSwitch *sw;
        const gchar *id;

        sw = g_ptr_array_index(data->extension_buttons, i);
        if (!gtk_switch_get_active(sw))
            continue;
        id = g_object_get_data(G_OBJECT(sw), "vellum-plugin-id");
        if (id == NULL)
            continue;
        /* 若已安装则跳过（热更新无需重装） */
        if (data->host->has_plugin != NULL && data->host->has_plugin(data->host, id))
            continue;
        g_ptr_array_add(data->pending_install_ids, g_strdup(id));
    }

    if (data->pending_install_ids->len == 0)
    {
        g_clear_pointer(&data->pending_install_ids, g_ptr_array_unref);
        data->guide_complete = TRUE;
        welcome_close_guide(data);
        welcome_ask_remove(data);
        return;
    }

    data->install_index = 0;
    data->install_success = 0;
    data->install_failed = 0;
    welcome_install_next(data);
}

static void
welcome_extension_intro_free(WelcomeExtensionIntro *intro)
{
    if (intro != NULL)
    {
        g_free(intro->id);
        g_free(intro->icon);
        g_free(intro->title);
        g_free(intro->description);
        g_free(intro->lesson);
        g_free(intro);
    }
}

static gchar *
welcome_intro_path(const gchar *filename)
{
    gchar *installed;
    gchar *development;

    installed = g_build_filename(VELLUM_EXTENSION_INTRO_DIR, filename, NULL);
    if (g_file_test(installed, G_FILE_TEST_IS_REGULAR))
    {
        return installed;
    }
    development = g_build_filename("data", "extension-intros", filename, NULL);
    if (g_file_test(development, G_FILE_TEST_IS_REGULAR))
    {
        g_free(installed);
        return development;
    }

    g_free(development);
    return installed;
}

static WelcomeExtensionIntro *
welcome_load_intro(const gchar *filename)
{
    GKeyFile *key_file;
    WelcomeExtensionIntro *intro;
    gchar *path;
    const gchar *group;

    path = welcome_intro_path(filename);
    key_file = g_key_file_new();
    if (!g_key_file_load_from_file(key_file, path, G_KEY_FILE_NONE, NULL))
    {
        g_key_file_unref(key_file);
        g_free(path);
        return NULL;
    }
    group = welcome_is_zh() ? "zh_CN" : "en";
    intro = g_new0(WelcomeExtensionIntro, 1);
    intro->id = g_key_file_get_string(key_file, "Extension", "id", NULL);
    intro->icon = g_key_file_get_string(key_file, "Extension", "icon", NULL);
    intro->title = g_key_file_get_string(key_file, group, "title", NULL);
    intro->description = g_key_file_get_string(key_file, group, "description", NULL);
    intro->lesson = g_key_file_get_string(key_file, group, "lesson", NULL);
    if (intro->id == NULL || intro->title == NULL || intro->description == NULL)
    {
        welcome_extension_intro_free(intro);
        intro = NULL;
    }
    g_key_file_unref(key_file);
    g_free(path);

    return intro;
}

static void
welcome_update_navigation(MtWelcomeData *data)
{
    gchar *progress;

    progress = g_strdup_printf(welcome_text("关卡 %u / 2", "Level %u / 2"), data->page + 1);
    gtk_label_set_text(data->progress_label, progress);
    g_free(progress);
    gtk_widget_set_sensitive(GTK_WIDGET(data->back_button), data->page > 0);
    gtk_button_set_label(data->next_button,
                         data->page == 1 ? welcome_text("完成引导", "Finish guide") :
                         welcome_text("下一关", "Next level"));
    gtk_widget_set_sensitive(GTK_WIDGET(data->next_button), TRUE);
}

static void
welcome_show_page(MtWelcomeData *data, guint page)
{
    gchar *name;

    data->page = MIN(page, 1);
    name = g_strdup_printf("page-%u", data->page);
    gtk_stack_set_visible_child_name(data->pages, name);
    g_free(name);
    welcome_update_navigation(data);
}

static GtkWidget *
welcome_build_intro_page(void)
{
    GtkWidget *box;
    GtkWidget *icon;
    GtkWidget *title;
    GtkWidget *description;
    GtkWidget *hint;

    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
    icon = gtk_image_new_from_icon_name("io.github.vellum.Vellum");
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 84);
    gtk_widget_set_halign(icon, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(box), icon);
    title = gtk_label_new("Vellum");
    gtk_widget_add_css_class(title, "title-1");
    gtk_widget_set_halign(title, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(box), title);
    description = gtk_label_new(welcome_text("欢迎来到 Vellum。接下来用两个简短关卡了解编辑器并选择扩展，建立自己的工作流。",
                                              "Welcome to Vellum. Complete two short levels to learn the editor, choose extensions and establish your workflow."));
    gtk_label_set_wrap(GTK_LABEL(description), TRUE);
    gtk_label_set_justify(GTK_LABEL(description), GTK_JUSTIFY_CENTER);
    gtk_widget_set_size_request(description, 400, -1);
    gtk_box_append(GTK_BOX(box), description);
    hint = gtk_label_new(welcome_text("无需背记；每一关都可以随时重新打开。", "Nothing needs to be memorized; you can reopen every level later."));
    gtk_widget_add_css_class(hint, "dim-label");
    gtk_widget_set_halign(hint, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(box), hint);

    return box;
}

static void
welcome_add_extension_action_row(GtkBox *list,
                                 MtWelcomeData *data,
                                 WelcomeExtensionIntro *intro)
{
    AdwActionRow *row;
    GtkSwitch *sw;

    row = ADW_ACTION_ROW(adw_action_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), intro->title);
    adw_action_row_set_subtitle(row, intro->description);
    if (intro->lesson != NULL)
    {
        GtkWidget *lesson;

        lesson = gtk_label_new(intro->lesson);
        gtk_label_set_wrap(GTK_LABEL(lesson), TRUE);
        gtk_label_set_justify(GTK_LABEL(lesson), GTK_JUSTIFY_FILL);
        adw_action_row_add_suffix(row, lesson);
    }
    sw = GTK_SWITCH(gtk_switch_new());
    gtk_switch_set_active(sw, TRUE);
    gtk_widget_set_valign(GTK_WIDGET(sw), GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(GTK_WIDGET(sw),
                                welcome_text("关闭将移除该扩展", "Turning off will remove this extension"));
    g_object_set_data_full(G_OBJECT(sw), "vellum-plugin-id", g_strdup(intro->id), g_free);
    g_ptr_array_add(data->extension_buttons, sw);
    adw_action_row_add_suffix(row, GTK_WIDGET(sw));
    gtk_box_append(list, GTK_WIDGET(row));
}

static void
welcome_install_mode_toggled(GtkCheckButton *button, gpointer user_data)
{
    MtWelcomeData *data;

    (void)button;
    data = user_data;
    if (data->source_radio == NULL || data->binary_radio == NULL)
        return;
    data->prefer_source = gtk_check_button_get_active(data->source_radio);
    if (data->source_detail_box != NULL)
        gtk_widget_set_visible(data->source_detail_box, data->prefer_source);
    welcome_save_install_pref(data->prefer_source);
}

static void
welcome_check_env_clicked(GtkButton *button, gpointer user_data)
{
    MtWelcomeData *data;
    gboolean ready;
    gchar *status;

    (void)button;
    data = user_data;
    status = welcome_check_build_env(&ready);
    gtk_label_set_text(data->env_status_label, status);
    if (ready)
        gtk_widget_add_css_class(GTK_WIDGET(data->env_status_label), "success");
    else
        gtk_widget_remove_css_class(GTK_WIDGET(data->env_status_label), "success");
    g_free(status);
    if (ready && data->host != NULL && data->host->show_toast != NULL)
        data->host->show_toast(data->host,
                               welcome_text("构建环境就绪，可安装源码扩展",
                                            "Build environment ready, source extensions can be installed"));
}

static void
welcome_custom_source_add_clicked(GtkButton *button, gpointer user_data)
{
    MtWelcomeData *data;
    const gchar *text;
    gchar *trimmed;
    GError *error;

    (void)button;
    data = user_data;
    text = gtk_editable_get_text(GTK_EDITABLE(data->custom_source_entry));
    trimmed = g_strdup(text != NULL ? text : "");
    g_strstrip(trimmed);
    if (*trimmed == '\0')
    {
        gtk_label_set_text(data->custom_source_status,
                           welcome_text("请输入 URL", "Please enter a URL"));
        g_free(trimmed);
        return;
    }
    error = NULL;
    if (welcome_add_market_source(trimmed, &error))
    {
        gchar *display;

        gtk_label_set_text(data->custom_source_status,
                           welcome_text("已添加自定义源", "Custom source added"));
        gtk_editable_set_text(GTK_EDITABLE(data->custom_source_entry), "");
        display = welcome_get_market_sources_display();
        gtk_label_set_text(data->custom_source_status, display);
        g_free(display);
        if (data->host != NULL && data->host->show_toast != NULL)
            data->host->show_toast(data->host,
                                   welcome_text("已保存自定义软件源，扩展市场刷新后生效",
                                                "Custom source saved, will take effect after marketplace refresh"));
    }
    else
    {
        gtk_label_set_text(data->custom_source_status,
                           error != NULL ? error->message :
                           welcome_text("添加失败", "Failed to add"));
        g_clear_error(&error);
    }
    g_free(trimmed);
}

static GtkWidget *
welcome_build_extensions_page(MtWelcomeData *data)
{
    GtkWidget *outer_scroll;
    GtkWidget *box;
    GtkWidget *title;
    GtkWidget *description;
    GtkWidget *list_scroll;
    GtkWidget *list;
    guint index;

    outer_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(outer_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(outer_scroll, TRUE);

    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_widget_set_margin_start(box, 4);
    gtk_widget_set_margin_end(box, 4);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(outer_scroll), box);

    title = gtk_label_new(welcome_text("第二关：选择你的扩展与安装方式", "Level 2: choose extensions & install method"));
    gtk_widget_add_css_class(title, "title-2");
    gtk_label_set_xalign(GTK_LABEL(title), 0.0);
    gtk_box_append(GTK_BOX(box), title);
    description = gtk_label_new(welcome_text("勾选想要保留的工作流工具；关闭的扩展将在完成引导后被移除。下方选择扩展的安装方式：二进制开箱即用，源码则在本机编译。",
                                              "Toggle the workflow tools you want to keep; turned-off extensions will be removed after completing the guide. Choose how to install extensions below: binary is ready-to-use, source is built locally."));
    gtk_label_set_wrap(GTK_LABEL(description), TRUE);
    gtk_label_set_xalign(GTK_LABEL(description), 0.0);
    gtk_widget_add_css_class(description, "dim-label");
    gtk_box_append(GTK_BOX(box), description);

    /* 扩展列表 */
    list_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(list_scroll), GTK_POLICY_NEVER, GTK_POLICY_NEVER);
    /* 固定高度，避免外层滚动与内层滚动冲突 */
    gtk_widget_set_size_request(list_scroll, -1, 220);
    list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(list, "card");
    for (index = 0; welcome_extension_files[index] != NULL; index++)
    {
        WelcomeExtensionIntro *intro;

        intro = welcome_load_intro(welcome_extension_files[index]);
        if (intro != NULL)
        {
            welcome_add_extension_action_row(GTK_BOX(list), data, intro);
            welcome_extension_intro_free(intro);
        }
    }
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(list_scroll), list);
    gtk_box_append(GTK_BOX(box), list_scroll);

    /* 安装方式 */
    {
        GtkWidget *mode_box;
        GtkWidget *mode_title;
        GtkWidget *radio_box;

        mode_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        gtk_widget_add_css_class(mode_box, "card");
        gtk_widget_set_margin_top(mode_box, 4);
        mode_title = gtk_label_new(welcome_text("安装方式 / Install Method", "Install Method"));
        gtk_widget_add_css_class(mode_title, "heading");
        gtk_label_set_xalign(GTK_LABEL(mode_title), 0.0);
        gtk_box_append(GTK_BOX(mode_box), mode_title);

        data->is_x64 = welcome_is_x64_arch();
        data->prefer_source = welcome_load_install_pref();
        /* 非 x64 强制源码 */
        if (!data->is_x64)
            data->prefer_source = TRUE;

        radio_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        data->binary_radio = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(welcome_text("二进制 .so（开箱即用，推荐 x86_64）", "Binary .so (ready-to-use, recommended for x86_64)")));
        data->source_radio = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(welcome_text("源码 .vut（本机编译，跨架构通用）", "Source .vut (build locally, cross-arch)")));
        gtk_check_button_set_group(data->source_radio, data->binary_radio);
        gtk_check_button_set_active(data->binary_radio, !data->prefer_source);
        gtk_check_button_set_active(data->source_radio, data->prefer_source);
        if (!data->is_x64)
        {
            gtk_widget_set_sensitive(GTK_WIDGET(data->binary_radio), FALSE);
            gtk_widget_set_tooltip_text(GTK_WIDGET(data->binary_radio),
                                        welcome_text("当前架构非 x86_64，二进制包不可用，已自动选择源码",
                                                     "Current arch is not x86_64, binary unavailable, source selected"));
        }
        else
        {
            gtk_widget_set_tooltip_text(GTK_WIDGET(data->binary_radio),
                                        welcome_text("直接下载 .so，无需编译环境", "Download .so directly, no build env needed"));
        }
        gtk_widget_set_tooltip_text(GTK_WIDGET(data->source_radio),
                                    welcome_text("下载源码包后本机 make/cc 编译，需开发依赖", "Download source and build with make/cc, needs dev packages"));
        g_signal_connect(data->binary_radio, "toggled", G_CALLBACK(welcome_install_mode_toggled), data);
        g_signal_connect(data->source_radio, "toggled", G_CALLBACK(welcome_install_mode_toggled), data);
        gtk_box_append(GTK_BOX(radio_box), GTK_WIDGET(data->binary_radio));
        gtk_box_append(GTK_BOX(radio_box), GTK_WIDGET(data->source_radio));
        gtk_box_append(GTK_BOX(mode_box), radio_box);

        data->arch_label = GTK_LABEL(gtk_label_new(NULL));
        {
            gchar *arch_text;
            struct utsname info;

            if (uname(&info) == 0)
                arch_text = g_strdup_printf(welcome_text("当前架构：%s  —  %s", "Current arch: %s — %s"),
                                            info.machine,
                                            data->is_x64 ?
                                            welcome_text("支持二进制", "binary supported") :
                                            welcome_text("仅支持源码", "source only"));
            else
                arch_text = g_strdup(welcome_text("当前架构检测失败，默认提供源码",
                                                  "Arch detection failed, default to source"));
            gtk_label_set_text(data->arch_label, arch_text);
            gtk_label_set_xalign(data->arch_label, 0.0);
            gtk_widget_add_css_class(GTK_WIDGET(data->arch_label), "caption");
            gtk_widget_add_css_class(GTK_WIDGET(data->arch_label), "dim-label");
            g_free(arch_text);
        }
        gtk_box_append(GTK_BOX(mode_box), GTK_WIDGET(data->arch_label));

        /* 源码详情：仅源码模式可见 */
        data->source_detail_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        {
            GtkWidget *note;
            GtkWidget *detail_label;
            GtkWidget *check_row;

            note = gtk_label_new(NULL);
            gtk_label_set_wrap(GTK_LABEL(note), TRUE);
            gtk_label_set_xalign(GTK_LABEL(note), 0.0);
            gtk_widget_add_css_class(note, "caption");
            gtk_label_set_markup(GTK_LABEL(note),
                                 welcome_text("源码扩展需本机构建环境：<b>make、cc、pkg-config</b> 及开发包。按发行版执行：",
                                              "Source extensions need local build env: <b>make, cc, pkg-config</b> and dev packages. Run per distro:"));
            gtk_box_append(GTK_BOX(data->source_detail_box), note);

            detail_label = gtk_label_new(NULL);
            gtk_label_set_selectable(GTK_LABEL(detail_label), TRUE);
            gtk_label_set_wrap(GTK_LABEL(detail_label), TRUE);
            gtk_label_set_xalign(GTK_LABEL(detail_label), 0.0);
            gtk_widget_add_css_class(detail_label, "monospace");
            gtk_label_set_text(GTK_LABEL(detail_label),
                               "Debian/Ubuntu:\n"
                               "  sudo apt install make gcc pkg-config libgtk-4-dev libadwaita-1-dev libgtksourceview-5-dev libsoup-3.0-dev libjson-glib-dev\n"
                               "Fedora/RHEL (Red Hat):\n"
                               "  sudo dnf install make gcc pkgconf-pkg-config gtk4-devel libadwaita-devel gtksourceview5-devel libsoup-devel json-glib-devel\n"
                               "Arch Linux:\n"
                               "  sudo pacman -S make gcc pkgconf gtk4 libadwaita gtksourceview5 libsoup json-glib\n"
                               "openSUSE:\n"
                               "  sudo zypper install make gcc pkgconf gtk4-devel libadwaita-devel gtksourceview5-devel libsoup-devel json-glib-devel");
            gtk_box_append(GTK_BOX(data->source_detail_box), detail_label);

            check_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
            data->check_env_button = GTK_BUTTON(gtk_button_new_with_label(welcome_text("检测环境", "Check Environment")));
            gtk_widget_add_css_class(GTK_WIDGET(data->check_env_button), "pill");
            g_signal_connect(data->check_env_button, "clicked", G_CALLBACK(welcome_check_env_clicked), data);
            gtk_box_append(GTK_BOX(check_row), GTK_WIDGET(data->check_env_button));
            data->env_status_label = GTK_LABEL(gtk_label_new(welcome_text("点击检测是否可编译", "Click to check if build is possible")));
            gtk_label_set_xalign(data->env_status_label, 0.0);
            gtk_widget_add_css_class(GTK_WIDGET(data->env_status_label), "caption");
            gtk_label_set_wrap(GTK_LABEL(data->env_status_label), TRUE);
            gtk_box_append(GTK_BOX(check_row), GTK_WIDGET(data->env_status_label));
            gtk_box_append(GTK_BOX(data->source_detail_box), check_row);
        }
        gtk_box_append(GTK_BOX(mode_box), data->source_detail_box);
        gtk_widget_set_visible(data->source_detail_box, data->prefer_source);

        gtk_box_append(GTK_BOX(box), mode_box);
    }

    /* 自定义软件源 */
    {
        GtkWidget *source_box;
        GtkWidget *source_title;
        GtkWidget *source_desc;
        AdwPreferencesGroup *source_group;
        GtkWidget *status_label;

        source_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        gtk_widget_add_css_class(source_box, "card");
        source_title = gtk_label_new(welcome_text("自定义软件源 / Custom Source", "Custom Software Source"));
        gtk_widget_add_css_class(source_title, "heading");
        gtk_label_set_xalign(GTK_LABEL(source_title), 0.0);
        gtk_box_append(GTK_BOX(source_box), source_title);
        source_desc = gtk_label_new(welcome_text("默认源为官方扩展仓库。可追加自建源（多个用分号分隔，扩展市场按先到先得合并）。",
                                                 "Default is the official extension repo. Add self-hosted sources (semicolon-separated, marketplace merges by first-seen)."));
        gtk_label_set_wrap(GTK_LABEL(source_desc), TRUE);
        gtk_label_set_xalign(GTK_LABEL(source_desc), 0.0);
        gtk_widget_add_css_class(source_desc, "caption");
        gtk_widget_add_css_class(source_desc, "dim-label");
        gtk_box_append(GTK_BOX(source_box), source_desc);

        source_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
        data->custom_source_entry = ADW_ENTRY_ROW(adw_entry_row_new());
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(data->custom_source_entry),
                                     welcome_text("追加源 URL", "Add Source URL"));
        adw_entry_row_set_show_apply_button(data->custom_source_entry, FALSE);
        adw_preferences_group_add(source_group, GTK_WIDGET(data->custom_source_entry));

        {
            GtkWidget *row;
            GtkWidget *add_button;

            row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
            add_button = gtk_button_new_with_label(welcome_text("添加源", "Add Source"));
            gtk_widget_add_css_class(add_button, "suggested-action");
            gtk_widget_set_halign(add_button, GTK_ALIGN_END);
            gtk_widget_set_hexpand(add_button, FALSE);
            g_signal_connect(add_button, "clicked", G_CALLBACK(welcome_custom_source_add_clicked), data);
            gtk_box_append(GTK_BOX(row), add_button);
            /* 将按钮与 EntryRow 的 apply 同步，保留一个主按钮 */
            gtk_box_append(GTK_BOX(source_box), GTK_WIDGET(source_group));
            gtk_box_append(GTK_BOX(source_box), row);
        }

        status_label = gtk_label_new(NULL);
        {
            gchar *current;

            current = welcome_get_market_sources_display();
            gtk_label_set_text(GTK_LABEL(status_label), current);
            g_free(current);
        }
        data->custom_source_status = GTK_LABEL(status_label);
        gtk_label_set_wrap(GTK_LABEL(status_label), TRUE);
        gtk_label_set_xalign(GTK_LABEL(status_label), 0.0);
        gtk_widget_add_css_class(status_label, "caption");
        gtk_widget_add_css_class(status_label, "dim-label");
        gtk_box_append(GTK_BOX(source_box), status_label);

        gtk_box_append(GTK_BOX(box), source_box);
    }

    return outer_scroll;
}

static void
welcome_apply_extension_choices(MtWelcomeData *data)
{
    guint index;

    if (data->host->request_plugin_removal == NULL)
    {
        return;
    }
    for (index = 0; index < data->extension_buttons->len; index++)
    {
        GtkSwitch *sw;
        const gchar *id;

        sw = g_ptr_array_index(data->extension_buttons, index);
        id = g_object_get_data(G_OBJECT(sw), "vellum-plugin-id");
        if (!gtk_switch_get_active(sw))
        {
            data->host->request_plugin_removal(data->host, id);
        }
    }
    /* 保存安装方式偏好，供后续扩展市场与引导下载使用 */
    welcome_save_install_pref(data->prefer_source);
}

static void
welcome_ask_remove(MtWelcomeData *data)
{
    AdwMessageDialog *dialog;

    dialog = ADW_MESSAGE_DIALOG(adw_message_dialog_new(data->host->get_parent_window(data->host),
                                                        welcome_text("保留新手引导？", "Keep the welcome guide?"),
                                                        welcome_text("已完成所有陪玩关卡。保留后可以从主菜单再次打开；删除后需要重新安装才会出现。",
                                                                     "You completed every guided level. Keep it to reopen from the main menu, or remove it until Vellum is reinstalled.")));
    adw_message_dialog_add_response(dialog, "keep", welcome_text("保留", "Keep"));
    adw_message_dialog_add_response(dialog, "remove", welcome_text("删除", "Remove"));
    adw_message_dialog_set_response_appearance(dialog, "remove", ADW_RESPONSE_DESTRUCTIVE);
    adw_message_dialog_set_default_response(dialog, "keep");
    adw_message_dialog_set_close_response(dialog, "keep");
    g_signal_connect(dialog, "response", G_CALLBACK(welcome_remove_response), data);
    gtk_window_present(GTK_WINDOW(dialog));
}

static gboolean
welcome_remove_idle(gpointer user_data)
{
    MtWelcomeData *data;

    data = user_data;
    data->remove_source_id = 0;
    data->host->request_plugin_removal(data->host, data->plugin_id);
    return G_SOURCE_REMOVE;
}

static void
welcome_remove_response(AdwMessageDialog *dialog, const gchar *response, gpointer user_data)
{
    MtWelcomeData *data;

    data = user_data;
    gtk_window_destroy(GTK_WINDOW(dialog));
    if (g_strcmp0(response, "remove") == 0 && data->remove_source_id == 0)
    {
        data->host->show_toast(data->host, welcome_text("新手引导插件已删除", "Welcome guide plugin removed"));
        data->remove_source_id = g_idle_add(welcome_remove_idle, data);
    }
}

static void
welcome_close_guide(MtWelcomeData *data)
{
    if (data->guide_window != NULL)
    {
        gtk_window_destroy(GTK_WINDOW(data->guide_window));
        data->guide_window = NULL;
    }
}

static void
welcome_next_clicked(GtkButton *button, gpointer user_data)
{
    MtWelcomeData *data;

    (void)button;
    data = user_data;
    if (data->page < 1)
    {
        welcome_show_page(data, data->page + 1);
        return;
    }
    /* 保存偏好并移除未勾选的扩展 */
    welcome_apply_extension_choices(data);
    /* 热更新：为勾选但尚未安装的扩展触发下载/编译，无需重启 */
    if (data->host->has_plugin != NULL && data->host->install_extension_async != NULL)
    {
        /* 禁用导航，显示安装中 */
        gtk_widget_set_sensitive(GTK_WIDGET(data->next_button), FALSE);
        gtk_widget_set_sensitive(GTK_WIDGET(data->back_button), FALSE);
        welcome_start_installs(data);
    }
    else
    {
        /* 旧宿主不支持热更新安装，仅完成移除 */
        data->guide_complete = TRUE;
        welcome_close_guide(data);
        welcome_ask_remove(data);
    }
}

static void
welcome_back_clicked(GtkButton *button, gpointer user_data)
{
    MtWelcomeData *data;

    (void)button;
    data = user_data;
    if (data->page > 0)
    {
        welcome_show_page(data, data->page - 1);
    }
}

static gboolean
welcome_guide_close_request(GtkWidget *widget, gpointer user_data)
{
    MtWelcomeData *data;

    (void)widget;
    data = user_data;
    data->guide_window = NULL;
    return FALSE;
}

static void
welcome_show_guide(MtWelcomeData *data)
{
    AdwWindow *guide;
    GtkWidget *toolbar;
    GtkWidget *header;
    GtkWidget *content;
    GtkWidget *navigation;
    GtkWidget *progress;

    if (data->guide_window != NULL)
    {
        gtk_window_present(GTK_WINDOW(data->guide_window));
        return;
    }
    {
        gchar *flag;
        gchar *directory;

        flag = welcome_flag_path();
        directory = g_path_get_dirname(flag);
        g_mkdir_with_parents(directory, 0700);
        g_file_set_contents(flag, "1", 1, NULL);
        g_free(directory);
        g_free(flag);
    }
    guide = ADW_WINDOW(adw_window_new());
    gtk_window_set_title(GTK_WINDOW(guide), welcome_text("Vellum 新手引导", "Vellum Welcome Guide"));
    gtk_window_set_default_size(GTK_WINDOW(guide), 680, 720);
    if (data->host->get_parent_window(data->host) != NULL)
    {
        gtk_window_set_transient_for(GTK_WINDOW(guide), data->host->get_parent_window(data->host));
    }
    gtk_window_set_modal(GTK_WINDOW(guide), TRUE);
    g_signal_connect(guide, "close-request", G_CALLBACK(welcome_guide_close_request), data);
    toolbar = adw_toolbar_view_new();
    header = adw_header_bar_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
    content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_widget_set_margin_start(content, 28);
    gtk_widget_set_margin_end(content, 28);
    gtk_widget_set_margin_top(content, 20);
    gtk_widget_set_margin_bottom(content, 20);
    data->pages = GTK_STACK(gtk_stack_new());
    gtk_widget_set_vexpand(GTK_WIDGET(data->pages), TRUE);
    gtk_stack_add_named(data->pages, welcome_build_intro_page(), "page-0");
    gtk_stack_add_named(data->pages, welcome_build_extensions_page(data), "page-1");
    gtk_box_append(GTK_BOX(content), GTK_WIDGET(data->pages));
    navigation = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    data->back_button = GTK_BUTTON(gtk_button_new_with_label(welcome_text("上一关", "Previous")));
    g_signal_connect(data->back_button, "clicked", G_CALLBACK(welcome_back_clicked), data);
    gtk_box_append(GTK_BOX(navigation), GTK_WIDGET(data->back_button));
    progress = gtk_label_new(NULL);
    data->progress_label = GTK_LABEL(progress);
    gtk_widget_set_hexpand(progress, TRUE);
    gtk_widget_set_halign(progress, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(navigation), progress);
    data->next_button = GTK_BUTTON(gtk_button_new_with_label(welcome_text("下一关", "Next level")));
    gtk_widget_add_css_class(GTK_WIDGET(data->next_button), "suggested-action");
    g_signal_connect(data->next_button, "clicked", G_CALLBACK(welcome_next_clicked), data);
    gtk_box_append(GTK_BOX(navigation), GTK_WIDGET(data->next_button));
    gtk_box_append(GTK_BOX(content), navigation);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), content);
    adw_window_set_content(guide, toolbar);
    data->guide_window = GTK_WIDGET(guide);
    welcome_show_page(data, 0);
    gtk_window_present(GTK_WINDOW(guide));
}

static gboolean
welcome_auto_show(gpointer user_data)
{
    MtWelcomeData *data;

    data = user_data;
    data->show_source_id = 0;
    welcome_show_guide(data);
    return G_SOURCE_REMOVE;
}

static void
welcome_show_action(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action;
    (void)parameter;
    welcome_show_guide(user_data);
}

static void
welcome_data_free(gpointer user_data)
{
    MtWelcomeData *data;

    data = user_data;
    if (data->show_source_id != 0)
    {
        g_source_remove(data->show_source_id);
    }
    if (data->remove_source_id != 0)
    {
        g_source_remove(data->remove_source_id);
    }
    welcome_close_guide(data);
    g_clear_pointer(&data->extension_buttons, g_ptr_array_unref);
    g_clear_pointer(&data->pending_install_ids, g_ptr_array_unref);
    g_free(data->plugin_id);
    g_free(data);
    welcome_data = NULL;
}

G_MODULE_EXPORT const MtPluginInfo *
mt_plugin_query(void)
{
    static MtPluginInfo info;

    info.api_version = MT_PLUGIN_API_VERSION;
    info.id = "io.github.vellum.welcome";
    info.name = welcome_text("新手引导", "Welcome Guide");
    info.description = welcome_text("通过交互关卡完成首次输入和扩展选择", "Complete first input and extension selection through interactive levels");
    info.version = "0.3.0";
    return &info;
}

G_MODULE_EXPORT gboolean
mt_plugin_activate(MtPluginHost *host, GError **error)
{
    MtWelcomeData *data;
    gchar *flag;

    data = g_new0(MtWelcomeData, 1);
    data->host = host;
    data->plugin_id = g_strdup("io.github.vellum.welcome");
    data->extension_buttons = g_ptr_array_new();
    welcome_data = data;
    if (!host->add_action(host, "show-welcome", welcome_show_action, data,
                          (GDestroyNotify)welcome_data_free))
    {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                    "The show-welcome action is already registered");
        welcome_data = NULL;
        welcome_data_free(data);
        return FALSE;
    }
    flag = welcome_flag_path();
    if (!g_file_test(flag, G_FILE_TEST_EXISTS))
    {
        data->show_source_id = g_timeout_add(600, welcome_auto_show, data);
    }
    g_free(flag);
    return TRUE;
}

G_MODULE_EXPORT void
mt_plugin_deactivate(MtPluginHost *host)
{
    (void)host;
    if (welcome_data != NULL)
    {
        welcome_close_guide(welcome_data);
    }
}
