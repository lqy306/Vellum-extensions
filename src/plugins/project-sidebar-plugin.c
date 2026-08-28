/*
 * project-sidebar-plugin.c
 * 面向多文件工作的本地项目侧边栏；只在用户选择目录后读取该目录的名称列表。
 */

#include "mt-plugin.h"

#include <adwaita.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <gmodule.h>

#define PROJECT_SIDEBAR_GROUP "Project Sidebar"
#define PROJECT_SIDEBAR_MAX_DEPTH 5
#define PROJECT_SIDEBAR_MAX_ITEMS 600

typedef struct _ProjectSidebar ProjectSidebar;
typedef struct _ProjectFolderRequest ProjectFolderRequest;

struct _ProjectSidebar
{
    MtPluginHost *host;
    GtkWidget *panel;
    GtkListBox *list;
    GtkLabel *root_label;
    gchar *root_path;
    guint item_count;
};

struct _ProjectFolderRequest
{
    ProjectSidebar *sidebar;
};

static const MtPluginInfo project_sidebar_plugin_info = {
    MT_PLUGIN_API_VERSION,
    "io.github.vellum.project-sidebar",
    "Project Sidebar",
    "Choose a project folder and browse its files in a sidebar",
    "0.1.0"
};

static ProjectSidebar *project_sidebar;

static gchar *
project_sidebar_config_path(void)
{
    gchar *directory;
    gchar *path;

    directory = g_build_filename(g_get_user_config_dir(), "vellum", NULL);
    g_mkdir_with_parents(directory, 0700);
    path = g_build_filename(directory, "project-sidebar.ini", NULL);
    g_free(directory);

    return path;
}

static void
project_sidebar_load_root(ProjectSidebar *sidebar)
{
    GKeyFile *settings;
    gchar *path;
    gchar *root;

    settings = g_key_file_new();
    path = project_sidebar_config_path();
    g_key_file_load_from_file(settings, path, G_KEY_FILE_NONE, NULL);
    root = g_key_file_get_string(settings, PROJECT_SIDEBAR_GROUP, "root", NULL);

    if (root != NULL && g_file_test(root, G_FILE_TEST_IS_DIR))
    {
        sidebar->root_path = root;
    }
    else
    {
        g_free(root);
    }

    g_free(path);
    g_key_file_unref(settings);
}

static gboolean
project_sidebar_save_root(ProjectSidebar *sidebar, GError **error)
{
    GKeyFile *settings;
    gchar *path;
    gchar *contents;
    gsize length;
    gboolean saved;

    settings = g_key_file_new();
    g_key_file_set_string(settings,
                          PROJECT_SIDEBAR_GROUP,
                          "root",
                          sidebar->root_path != NULL ? sidebar->root_path : "");
    contents = g_key_file_to_data(settings, &length, error);
    if (contents == NULL)
    {
        g_key_file_unref(settings);
        return FALSE;
    }

    path = project_sidebar_config_path();
    saved = g_file_set_contents(path, contents, (gssize)length, error);
    if (saved)
    {
        g_chmod(path, 0600);
    }

    g_free(path);
    g_free(contents);
    g_key_file_unref(settings);

    return saved;
}

static gboolean
project_sidebar_should_skip(const gchar *name)
{
    return name[0] == '.' ||
           g_strcmp0(name, "node_modules") == 0 ||
           g_strcmp0(name, "__pycache__") == 0 ||
           g_strcmp0(name, "build") == 0 ||
           g_strcmp0(name, "dist") == 0;
}

static void
project_sidebar_add_row(ProjectSidebar *sidebar,
                        const gchar *path,
                        const gchar *name,
                        guint depth,
                        gboolean is_directory)
{
    GtkWidget *row;
    GtkWidget *box;
    GtkWidget *icon;
    GtkWidget *label;

    if (sidebar->item_count >= PROJECT_SIDEBAR_MAX_ITEMS)
    {
        return;
    }

    row = gtk_list_box_row_new();
    box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    icon = gtk_image_new_from_icon_name(is_directory ?
                                        "folder-symbolic" :
                                        "text-x-generic-symbolic");
    label = gtk_label_new(name);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_widget_set_margin_start(box, 8 + (gint)depth * 14);
    gtk_widget_set_margin_end(box, 8);
    gtk_widget_set_margin_top(box, 3);
    gtk_widget_set_margin_bottom(box, 3);
    gtk_box_append(GTK_BOX(box), icon);
    gtk_box_append(GTK_BOX(box), label);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
    gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), !is_directory);
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), !is_directory);
    g_object_set_data_full(G_OBJECT(row),
                           "vellum-project-path",
                           g_strdup(path),
                           g_free);
    gtk_list_box_append(sidebar->list, row);
    sidebar->item_count++;
}

static void
project_sidebar_collect_directory(ProjectSidebar *sidebar,
                                  const gchar *directory,
                                  guint depth)
{
    GDir *dir;
    const gchar *name;
    GPtrArray *names;
    guint index;

    if (depth > PROJECT_SIDEBAR_MAX_DEPTH || sidebar->item_count >= PROJECT_SIDEBAR_MAX_ITEMS)
    {
        return;
    }

    dir = g_dir_open(directory, 0, NULL);
    if (dir == NULL)
    {
        return;
    }

    names = g_ptr_array_new_with_free_func(g_free);
    while ((name = g_dir_read_name(dir)) != NULL)
    {
        if (!project_sidebar_should_skip(name))
        {
            g_ptr_array_add(names, g_strdup(name));
        }
    }
    g_dir_close(dir);
    g_ptr_array_sort(names, (GCompareFunc)g_strcmp0);

    for (index = 0; index < names->len && sidebar->item_count < PROJECT_SIDEBAR_MAX_ITEMS; index++)
    {
        const gchar *child_name;
        gchar *path;
        gboolean is_directory;

        child_name = g_ptr_array_index(names, index);
        path = g_build_filename(directory, child_name, NULL);
        is_directory = g_file_test(path, G_FILE_TEST_IS_DIR);
        project_sidebar_add_row(sidebar, path, child_name, depth, is_directory);
        if (is_directory)
        {
            project_sidebar_collect_directory(sidebar, path, depth + 1);
        }
        g_free(path);
    }

    g_ptr_array_unref(names);
}

static void
project_sidebar_clear_rows(ProjectSidebar *sidebar)
{
    GtkWidget *child;

    while ((child = gtk_widget_get_first_child(GTK_WIDGET(sidebar->list))) != NULL)
    {
        gtk_list_box_remove(sidebar->list, child);
    }
}

static void
project_sidebar_refresh(ProjectSidebar *sidebar)
{
    gchar *title;

    if (sidebar->list == NULL || sidebar->root_label == NULL)
    {
        return;
    }

    project_sidebar_clear_rows(sidebar);
    sidebar->item_count = 0;

    if (sidebar->root_path == NULL || !g_file_test(sidebar->root_path, G_FILE_TEST_IS_DIR))
    {
        gtk_label_set_text(sidebar->root_label, _("No project folder selected"));
        return;
    }

    title = g_path_get_basename(sidebar->root_path);
    gtk_label_set_text(sidebar->root_label, title);
    g_free(title);
    project_sidebar_collect_directory(sidebar, sidebar->root_path, 0);

    if (sidebar->item_count == PROJECT_SIDEBAR_MAX_ITEMS)
    {
        sidebar->host->show_toast(sidebar->host,
                                  _("Project sidebar is limited to the first 600 items"));
    }
}

static void
project_sidebar_row_activated(GtkListBox *list,
                              GtkListBoxRow *row,
                              gpointer user_data)
{
    ProjectSidebar *sidebar;
    const gchar *path;

    (void)list;
    sidebar = user_data;
    path = g_object_get_data(G_OBJECT(row), "vellum-project-path");
    if (path != NULL)
    {
        sidebar->host->open_file_path(sidebar->host, path);
    }
}

static void
project_sidebar_close_clicked(GtkButton *button, gpointer user_data)
{
    ProjectSidebar *sidebar;

    (void)button;
    sidebar = user_data;
    sidebar->host->hide_panel(sidebar->host,
                              project_sidebar_plugin_info.id,
                              MT_PLUGIN_PANEL_SIDEBAR);
    sidebar->panel = NULL;
    sidebar->list = NULL;
    sidebar->root_label = NULL;
}

static void
project_sidebar_refresh_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    project_sidebar_refresh(user_data);
}

static void
project_sidebar_folder_selected(GObject *source,
                                GAsyncResult *result,
                                gpointer user_data)
{
    ProjectFolderRequest *request;
    GFile *folder;
    GError *error;

    request = user_data;
    error = NULL;
    folder = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(source), result, &error);
    if (folder != NULL)
    {
        gchar *path;

        path = g_file_get_path(folder);
        if (path != NULL)
        {
            g_free(request->sidebar->root_path);
            request->sidebar->root_path = path;
            project_sidebar_refresh(request->sidebar);
            if (!project_sidebar_save_root(request->sidebar, &error))
            {
                gchar *message;

                message = g_strdup_printf(_("Unable to save project folder: %s"), error->message);
                request->sidebar->host->show_toast(request->sidebar->host, message);
                g_free(message);
                g_clear_error(&error);
            }
        }
        g_object_unref(folder);
    }
    else if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
        gchar *message;

        message = g_strdup_printf(_("Unable to choose project folder: %s"), error->message);
        request->sidebar->host->show_toast(request->sidebar->host, message);
        g_free(message);
    }

    g_clear_error(&error);
    g_free(request);
}

static void
project_sidebar_choose_folder(ProjectSidebar *sidebar)
{
    GtkFileDialog *dialog;
    ProjectFolderRequest *request;

    dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, _("Choose Project Folder"));
    request = g_new0(ProjectFolderRequest, 1);
    request->sidebar = sidebar;
    gtk_file_dialog_select_folder(dialog,
                                  sidebar->host->get_parent_window(sidebar->host),
                                  NULL,
                                  project_sidebar_folder_selected,
                                  request);
    g_object_unref(dialog);
}

static void
project_sidebar_choose_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    project_sidebar_choose_folder(user_data);
}

static void
project_sidebar_show(ProjectSidebar *sidebar)
{
    GtkWidget *header;
    GtkWidget *title;
    GtkWidget *choose_button;
    GtkWidget *refresh_button;
    GtkWidget *close_button;
    GtkWidget *scroll;

    if (sidebar->panel != NULL && gtk_widget_get_parent(sidebar->panel) != NULL)
    {
        sidebar->host->hide_panel(sidebar->host,
                                  project_sidebar_plugin_info.id,
                                  MT_PLUGIN_PANEL_SIDEBAR);
        sidebar->panel = NULL;
        sidebar->list = NULL;
        sidebar->root_label = NULL;
        return;
    }

    sidebar->panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    /* 侧边栏至少保留标签和三个操作按钮的可用宽度。 */
    gtk_widget_set_size_request(sidebar->panel, 280, -1);
    header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    title = gtk_label_new(_("Project Files"));
    choose_button = gtk_button_new_from_icon_name("folder-open-symbolic");
    refresh_button = gtk_button_new_from_icon_name("view-refresh-symbolic");
    close_button = gtk_button_new_from_icon_name("sidebar-hide-symbolic");
    sidebar->root_label = GTK_LABEL(gtk_label_new(""));
    sidebar->list = GTK_LIST_BOX(gtk_list_box_new());
    scroll = gtk_scrolled_window_new();

    gtk_widget_add_css_class(title, "heading");
    gtk_widget_set_hexpand(title, TRUE);
    gtk_widget_set_margin_start(header, 12);
    gtk_widget_set_margin_end(header, 6);
    gtk_widget_set_margin_top(header, 8);
    gtk_widget_set_margin_bottom(header, 4);
    gtk_widget_set_tooltip_text(choose_button, _("Choose Project Folder"));
    gtk_widget_set_tooltip_text(refresh_button, _("Refresh Project Files"));
    gtk_widget_set_tooltip_text(close_button, _("Hide Project Sidebar"));
    gtk_label_set_xalign(sidebar->root_label, 0.0f);
    gtk_label_set_ellipsize(sidebar->root_label, PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_set_margin_start(GTK_WIDGET(sidebar->root_label), 12);
    gtk_widget_set_margin_end(GTK_WIDGET(sidebar->root_label), 12);
    gtk_widget_set_margin_bottom(GTK_WIDGET(sidebar->root_label), 6);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(sidebar->list));

    gtk_box_append(GTK_BOX(header), title);
    gtk_box_append(GTK_BOX(header), choose_button);
    gtk_box_append(GTK_BOX(header), refresh_button);
    gtk_box_append(GTK_BOX(header), close_button);
    gtk_box_append(GTK_BOX(sidebar->panel), header);
    gtk_box_append(GTK_BOX(sidebar->panel), GTK_WIDGET(sidebar->root_label));
    gtk_box_append(GTK_BOX(sidebar->panel), scroll);
    g_signal_connect(sidebar->list,
                     "row-activated",
                     G_CALLBACK(project_sidebar_row_activated),
                     sidebar);
    g_signal_connect(choose_button,
                     "clicked",
                     G_CALLBACK(project_sidebar_choose_clicked),
                     sidebar);
    g_signal_connect(refresh_button,
                     "clicked",
                     G_CALLBACK(project_sidebar_refresh_clicked),
                     sidebar);
    g_signal_connect(close_button,
                     "clicked",
                     G_CALLBACK(project_sidebar_close_clicked),
                     sidebar);

    sidebar->host->set_panel(sidebar->host,
                             project_sidebar_plugin_info.id,
                             MT_PLUGIN_PANEL_SIDEBAR,
                             sidebar->panel);
    project_sidebar_refresh(sidebar);
}

static void
project_sidebar_activate_action(GSimpleAction *action,
                                GVariant *parameter,
                                gpointer user_data)
{
    (void)action;
    (void)parameter;
    project_sidebar_show(user_data);
}

G_MODULE_EXPORT const MtPluginInfo *
mt_plugin_query(void)
{
    return &project_sidebar_plugin_info;
}

G_MODULE_EXPORT gboolean
mt_plugin_activate(MtPluginHost *host, GError **error)
{
    static const gchar *accelerators[] = { "<Primary><Shift>p", NULL };

    if (host->set_panel == NULL || host->hide_panel == NULL ||
        host->open_file_path == NULL || host->get_parent_window == NULL)
    {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_NOT_SUPPORTED,
                    "Vellum host does not provide the project sidebar services");
        return FALSE;
    }

    project_sidebar = g_new0(ProjectSidebar, 1);
    project_sidebar->host = host;
    project_sidebar_load_root(project_sidebar);

    if (!host->add_action(host,
                          "project-sidebar",
                          project_sidebar_activate_action,
                          project_sidebar,
                          NULL))
    {
        g_free(project_sidebar->root_path);
        g_free(project_sidebar);
        project_sidebar = NULL;
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_EXISTS,
                    "The project-sidebar action is already registered");
        return FALSE;
    }
    host->set_accelerators(host, "app.project-sidebar", accelerators);

    return TRUE;
}

G_MODULE_EXPORT void
mt_plugin_deactivate(MtPluginHost *host)
{
    if (project_sidebar == NULL)
    {
        return;
    }

    if (project_sidebar->panel != NULL)
    {
        host->hide_panel(host,
                         project_sidebar_plugin_info.id,
                         MT_PLUGIN_PANEL_SIDEBAR);
    }

    g_free(project_sidebar->root_path);
    g_free(project_sidebar);
    project_sidebar = NULL;
}

G_MODULE_EXPORT void
mt_plugin_configure(MtPluginHost *host, gpointer parent_window)
{
    (void)host;
    (void)parent_window;

    if (project_sidebar != NULL)
    {
        project_sidebar_choose_folder(project_sidebar);
    }
}
