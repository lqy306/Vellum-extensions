/*
 * test-dev-experience-markup.c
 * 验证 Dev Experience 配置页中的普通文本不会被 Adwaita 当作 Pango 标记解析。
 */

#include <adwaita.h>
#include <gmodule.h>

#include "mt-plugin.h"

static GtkWindow *test_parent_window;

static gchar *
host_get_current_file_path(MtPluginHost *host)
{
    (void)host;
    return g_strdup("/tmp/vellum-dev-xp-test.c");
}

static GtkWindow *
host_get_parent_window(MtPluginHost *host)
{
    (void)host;
    return test_parent_window;
}

static void
host_set_panel(MtPluginHost *host,
               const gchar *id,
               MtPluginPanelLocation location,
               GtkWidget *panel)
{
    (void)host;
    (void)id;
    (void)location;
    (void)panel;
}

static void
host_hide_panel(MtPluginHost *host,
                const gchar *id,
                MtPluginPanelLocation location)
{
    (void)host;
    (void)id;
    (void)location;
}

static gboolean
host_add_action(MtPluginHost *host,
                const gchar *name,
                MtPluginActionCallback callback,
                gpointer user_data,
                GDestroyNotify destroy_notify)
{
    (void)host;
    (void)name;
    (void)callback;
    (void)user_data;
    (void)destroy_notify;
    return TRUE;
}

static void
host_set_accelerators(MtPluginHost *host,
                      const gchar *detailed_action_name,
                      const gchar * const *accelerators)
{
    (void)host;
    (void)detailed_action_name;
    (void)accelerators;
}

static void
host_show_toast(MtPluginHost *host, const gchar *message)
{
    (void)host;
    (void)message;
}

static GtkWidget *
find_button(GtkWidget *widget, const gchar *label)
{
    GtkWidget *child;

    if (GTK_IS_BUTTON(widget) &&
        g_strcmp0(gtk_button_get_label(GTK_BUTTON(widget)), label) == 0)
    {
        return widget;
    }

    child = gtk_widget_get_first_child(widget);
    while (child != NULL)
    {
        GtkWidget *found;

        found = find_button(child, label);
        if (found != NULL)
        {
            return found;
        }
        child = gtk_widget_get_next_sibling(child);
    }

    return NULL;
}

static void
iterate_main_context(void)
{
    while (g_main_context_pending(NULL))
    {
        g_main_context_iteration(NULL, FALSE);
    }
}

int
main(void)
{
    MtPluginHost host;
    GModule *module;
    MtPluginActivateFunc activate;
    MtPluginDeactivateFunc deactivate;
    MtPluginConfigureFunc configure;
    GtkWidget *parent;
    GtkWidget *save_button;
    GList *windows;
    GList *item;
    GtkWidget *config_window;
    GError *error;

    if (g_getenv("VELLUM_DEV_EXPERIENCE_TEST_PLUGIN") == NULL)
    {
        return 2;
    }

    gtk_init();
    module = g_module_open(g_getenv("VELLUM_DEV_EXPERIENCE_TEST_PLUGIN"), G_MODULE_BIND_LAZY);
    if (module == NULL)
    {
        return 3;
    }

    activate = NULL;
    deactivate = NULL;
    configure = NULL;
    if (!g_module_symbol(module, "mt_plugin_activate", (gpointer *)&activate) ||
        !g_module_symbol(module, "mt_plugin_configure", (gpointer *)&configure))
    {
        g_module_close(module);
        return 4;
    }
    g_module_symbol(module, "mt_plugin_deactivate", (gpointer *)&deactivate);

    memset(&host, 0, sizeof(host));
    host.api_version = MT_PLUGIN_API_VERSION;
    host.add_action = host_add_action;
    host.set_accelerators = host_set_accelerators;
    host.get_current_file_path = host_get_current_file_path;
    host.get_parent_window = host_get_parent_window;
    host.set_panel = host_set_panel;
    host.hide_panel = host_hide_panel;
    host.show_toast = host_show_toast;
    error = NULL;
    if (!activate(&host, &error))
    {
        g_clear_error(&error);
        g_module_close(module);
        return 5;
    }

    parent = gtk_window_new();
    test_parent_window = GTK_WINDOW(parent);
    configure(&host, parent);
    iterate_main_context();

    config_window = NULL;
    windows = gtk_window_list_toplevels();
    for (item = windows; item != NULL; item = item->next)
    {
        if (item->data != parent)
        {
            config_window = item->data;
            break;
        }
    }
    g_list_free(windows);
    if (config_window == NULL)
    {
        if (deactivate != NULL)
        {
            deactivate(&host);
        }
        g_module_close(module);
        return 6;
    }

    save_button = find_button(config_window, "Save");
    if (save_button == NULL)
    {
        if (deactivate != NULL)
        {
            deactivate(&host);
        }
        g_module_close(module);
        return 7;
    }

    g_signal_emit_by_name(save_button, "clicked");
    iterate_main_context();
    gtk_window_destroy(GTK_WINDOW(parent));
    if (deactivate != NULL)
    {
        deactivate(&host);
    }
    g_module_close(module);
    return 0;
}
