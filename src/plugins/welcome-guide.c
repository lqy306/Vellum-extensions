/*
 * welcome-guide.c
 * 新手引导的安装流程、UI 构建与引导管理。
 */

#include "welcome-private.h"

#include <sys/utsname.h>

#include <glib/gstdio.h>

/* —— 安装流程 —— */

void
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

void
welcome_install_next(MtWelcomeData *data)
{
    const gchar *plugin_id;

    if (data->pending_install_ids == NULL ||
        data->install_index >= data->pending_install_ids->len)
    {
        /* 全部完成：热更新已生效，无需重启；不再自动询问删除欢迎引导，避免误触 */
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
        else if (data->install_success == 0 && data->install_failed == 0)
        {
            if (data->host->show_toast != NULL)
                data->host->show_toast(data->host,
                                       welcome_text("所选扩展已就绪（热更新）",
                                                    "Selected extensions are ready (hot update)"));
        }
        g_clear_pointer(&data->pending_install_ids, g_ptr_array_unref);
        data->guide_complete = TRUE;
        welcome_close_guide(data);
        /* 完成后不自动弹出“删除欢迎引导”对话框，避免用户误以为刚安装的扩展被删除；
         * 欢迎引导仍可从主菜单再次打开，若需删除可在扩展列表中手动操作。 */
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

void
welcome_start_installs(MtWelcomeData *data)
{
    guint i;

    /* 收集勾选的扩展：无论是否已安装，均按当前偏好重装以保证源码/二进制一致 */
    data->pending_install_ids = g_ptr_array_new_with_free_func(g_free);
    for (i = 0; i < data->extension_buttons->len; i++)
    {
        GtkSwitch *sw;
        const gchar *id;

        sw = g_ptr_array_index(data->extension_buttons, i);
        if (!GTK_IS_SWITCH(sw))
            continue;
        if (!gtk_switch_get_active(sw))
            continue;
        id = g_object_get_data(G_OBJECT(sw), "vellum-plugin-id");
        if (id == NULL)
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

/* —— 扩展介绍 —— */

void
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

gchar *
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

WelcomeExtensionIntro *
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

/* —— 导航 —— */

void
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

void
welcome_show_page(MtWelcomeData *data, guint page)
{
    gchar *name;

    data->page = MIN(page, 1);
    name = g_strdup_printf("page-%u", data->page);
    gtk_stack_set_visible_child_name(data->pages, name);
    g_free(name);
    welcome_update_navigation(data);
}

/* —— UI 构建 —— */

GtkWidget *
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

void
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

void
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

void
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

void
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

GtkWidget *
welcome_build_extensions_page(MtWelcomeData *data)
{
    GtkWidget *outer_scroll;
    GtkWidget *box;
    GtkWidget *title;
    GtkWidget *description;
    GtkWidget *list_scroll;
    GtkWidget *list;
    guint index;

    g_ptr_array_set_size(data->extension_buttons, 0);
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
                arch_text = g_strdup_printf(welcome_text("当前架构检测失败，默认提供源码",
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
            GtkLabel *detail_label;
            GtkBox *check_row;

            note = gtk_label_new(NULL);
            gtk_label_set_wrap(GTK_LABEL(note), TRUE);
            gtk_label_set_xalign(GTK_LABEL(note), 0.0);
            gtk_widget_add_css_class(note, "caption");
            gtk_label_set_markup(GTK_LABEL(note),
                                 welcome_text("源码扩展需本机构建环境：<b>make、cc、pkg-config</b> 及开发包。按发行版执行：",
                                              "Source extensions need local build env: <b>make, cc, pkg-config</b> and dev packages. Run per distro:"));
            gtk_box_append(GTK_BOX(data->source_detail_box), note);

            detail_label = GTK_LABEL(gtk_label_new(NULL));
            gtk_label_set_selectable(detail_label, TRUE);
            gtk_label_set_wrap(detail_label, TRUE);
            gtk_label_set_xalign(detail_label, 0.0);
            gtk_widget_add_css_class(GTK_WIDGET(detail_label), "monospace");
            gtk_label_set_text(detail_label,
                               "Debian/Ubuntu:\n"
                               "  sudo apt install make gcc pkg-config libgtk-4-dev libadwaita-1-dev libgtksourceview-5-dev libsoup-3.0-dev libjson-glib-dev\n"
                               "Fedora/RHEL (Red Hat):\n"
                               "  sudo dnf install make gcc pkgconf-pkg-config gtk4-devel libadwaita-devel gtksourceview5-devel libsoup-devel json-glib-devel\n"
                               "Arch Linux:\n"
                               "  sudo pacman -S make gcc pkgconf gtk4 libadwaita gtksourceview5 libsoup json-glib\n"
                               "openSUSE:\n"
                               "  sudo zypper install make gcc pkgconf gtk4-devel libadwaita-devel gtksourceview5-devel libsoup-devel json-glib-devel");
            gtk_box_append(GTK_BOX(data->source_detail_box), GTK_WIDGET(detail_label));

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
        GtkLabel *source_desc;
        AdwPreferencesGroup *source_group;
        GtkLabel *status_label;

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

        status_label = GTK_LABEL(gtk_label_new(NULL));
        {
            gchar *current;

            current = welcome_get_market_sources_display();
            gtk_label_set_text(status_label, current);
            g_free(current);
        }
        data->custom_source_status = status_label;
        gtk_label_set_wrap(status_label, TRUE);
        gtk_label_set_xalign(status_label, 0.0);
        gtk_widget_add_css_class(status_label, "caption");
        gtk_widget_add_css_class(status_label, "dim-label");
        gtk_box_append(GTK_BOX(source_box), status_label);

        gtk_box_append(GTK_BOX(box), source_box);
    }

    return outer_scroll;
}

/* —— 引导管理 —— */

void
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
        if (!GTK_IS_SWITCH(sw))
            continue;
        id = g_object_get_data(G_OBJECT(sw), "vellum-plugin-id");
        if (id == NULL)
            continue;
        if (!gtk_switch_get_active(sw))
        {
            data->host->request_plugin_removal(data->host, id);
        }
    }
    /* 保存安装方式偏好，供后续扩展市场与引导下载使用 */
    welcome_save_install_pref(data->prefer_source);
}

void
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

void
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

void
welcome_close_guide(MtWelcomeData *data)
{
    if (data->guide_window != NULL)
    {
        gtk_window_destroy(GTK_WINDOW(data->guide_window));
        data->guide_window = NULL;
    }
}

void
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

void
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

gboolean
welcome_guide_close_request(GtkWidget *widget, gpointer user_data)
{
    MtWelcomeData *data;

    (void)widget;
    data = user_data;
    data->guide_window = NULL;
    return FALSE;
}

void
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

gboolean
welcome_auto_show(gpointer user_data)
{
    MtWelcomeData *data;

    data = user_data;
    data->show_source_id = 0;
    welcome_show_guide(data);
    return G_SOURCE_REMOVE;
}

void
welcome_show_action(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action;
    (void)parameter;
    welcome_show_guide(user_data);
}

void
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
}