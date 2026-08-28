/*
 * welcome-plugin.c
 * Vellum 的分关卡新手引导。引导把首次输入与扩展选择拆成明确、可完成的交互，
 * 每个扩展的介绍信息由独立 INI 配置提供，便于新增扩展时无需改写引导流程。
 */

#include "mt-plugin.h"

#include <adwaita.h>
#include <gmodule.h>
#include <glib/gstdio.h>

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
};

static MtWelcomeData *welcome_data;

static void welcome_remove_response(AdwMessageDialog *dialog,
                                    const gchar *response,
                                    gpointer user_data);

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

static GtkWidget *
welcome_build_extensions_page(MtWelcomeData *data)
{
    GtkWidget *box;
    GtkWidget *title;
    GtkWidget *description;
    GtkWidget *scroll;
    GtkWidget *list;
    guint index;

    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    title = gtk_label_new(welcome_text("第二关：选择你的扩展", "Level 2: choose your extensions"));
    gtk_widget_add_css_class(title, "title-2");
    gtk_label_set_xalign(GTK_LABEL(title), 0.0);
    gtk_box_append(GTK_BOX(box), title);
    description = gtk_label_new(welcome_text("勾选想要保留的工作流工具；关闭的扩展将在完成引导后被移除。",
                                              "Toggle the workflow tools you want to keep; turned-off extensions will be removed after completing the guide."));
    gtk_label_set_wrap(GTK_LABEL(description), TRUE);
    gtk_label_set_xalign(GTK_LABEL(description), 0.0);
    gtk_box_append(GTK_BOX(box), description);
    scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroll, TRUE);
    list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
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
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), list);
    gtk_box_append(GTK_BOX(box), scroll);

    {
        /* 源码分发所需环境的说明：源码包导入后由本机 make/cc 重建。 */
        GtkWidget *note;

        note = gtk_label_new(NULL);
        gtk_label_set_wrap(GTK_LABEL(note), TRUE);
        gtk_label_set_xalign(GTK_LABEL(note), 0.0);
        gtk_widget_add_css_class(note, "caption");
        gtk_label_set_markup(GTK_LABEL(note),
                             welcome_text("扩展可从扩展市场以二进制或源码两种形式安装。源码包需要本机构建环境：make、cc 与 pkg-config，以及清单列出的 GTK/GLib 等开发包（Debian/Ubuntu 大致为 sudo apt install make gcc pkg-config libgtk-4-dev libadwaita-1-dev libsoup-3.0-dev libjson-glib-dev）。",
                                          "Extensions can be installed from the marketplace as binary or source packages. Source packages need a local build environment: make, cc and pkg-config, plus the GTK/GLib development packages listed in the package (on Debian/Ubuntu roughly: sudo apt install make gcc pkg-config libgtk-4-dev libadwaita-1-dev libsoup-3.0-dev libjson-glib-dev)."));
        gtk_box_append(GTK_BOX(box), note);
    }

    return box;
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
    welcome_apply_extension_choices(data);
    data->guide_complete = TRUE;
    welcome_close_guide(data);
    welcome_ask_remove(data);
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
    gtk_window_set_default_size(GTK_WINDOW(guide), 620, 640);
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
    info.version = "0.2.0";
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
