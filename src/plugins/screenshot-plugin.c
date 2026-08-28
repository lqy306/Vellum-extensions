/*
 * screenshot-plugin.c
 * 截图插件：把 Vellum 主窗口当前界面渲染成 PNG 保存到图片目录，
 * 供界面排布分析（例如菜单被截断、面板错位）时直接查看。
 */

#include "mt-plugin.h"

#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <gmodule.h>
#include <gtk/gtk.h>

static const MtPluginInfo screenshot_plugin_info = {
    MT_PLUGIN_API_VERSION,
    "io.github.vellum.screenshot",
    "Screenshot",
    "Save the application window as a PNG image",
    "0.1.0"
};

static gchar *
screenshot_plugin_directory(void)
{
    const gchar *directory;

    directory = g_get_user_special_dir(G_USER_DIRECTORY_PICTURES);
    if (directory != NULL && g_file_test(directory, G_FILE_TEST_IS_DIR))
    {
        return g_strdup(directory);
    }

    directory = g_get_home_dir();
    if (directory != NULL && g_file_test(directory, G_FILE_TEST_IS_DIR))
    {
        return g_strdup(directory);
    }

    return g_strdup(g_get_user_data_dir());
}

static gboolean
screenshot_plugin_save_window(GtkWindow *window, gchar **out_path, GError **error)
{
    GtkWidget *window_widget;
    GdkPaintable *paintable;
    GtkSnapshot *snapshot;
    GskRenderNode *node;
    GskRenderer *renderer;
    GdkTexture *texture;
    GDateTime *now;
    gchar *directory;
    gchar *stamp;
    gchar *basename;
    gchar *path;
    gint width;
    gint height;
    gboolean success;

    window_widget = GTK_WIDGET(window);
    if (!gtk_widget_get_realized(window_widget) || !gtk_widget_get_mapped(window_widget))
    {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_FAILED,
                    "The window is not visible yet");
        return FALSE;
    }

    width = gtk_widget_get_width(window_widget);
    height = gtk_widget_get_height(window_widget);
    if (width <= 0 || height <= 0)
    {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_FAILED,
                    "The window has no size yet");
        return FALSE;
    }

    paintable = gtk_widget_paintable_new(window_widget);
    snapshot = gtk_snapshot_new();
    gdk_paintable_snapshot(paintable, GDK_SNAPSHOT(snapshot), width, height);
    g_object_unref(paintable);
    node = gtk_snapshot_free_to_node(snapshot);
    if (node == NULL)
    {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_FAILED,
                    "Unable to render the window to an image");
        return FALSE;
    }

    renderer = gsk_cairo_renderer_new();
    if (!gsk_renderer_realize_for_display(renderer,
                                          gdk_display_get_default(),
                                          error))
    {
        gsk_render_node_unref(node);
        g_object_unref(renderer);
        return FALSE;
    }
    texture = gsk_renderer_render_texture(renderer, node, NULL);
    gsk_render_node_unref(node);
    gsk_renderer_unrealize(renderer);
    g_object_unref(renderer);
    if (texture == NULL)
    {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_FAILED,
                    "Unable to render the window to an image");
        return FALSE;
    }

    directory = screenshot_plugin_directory();
    now = g_date_time_new_now_local();
    stamp = g_date_time_format(now, "%Y%m%d-%H%M%S");
    basename = g_strdup_printf("vellum-screenshot-%s.png", stamp);
    path = g_build_filename(directory, basename, NULL);

    success = gdk_texture_save_to_png(texture, path);
    if (!success)
    {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_FAILED,
                    "Unable to write %s", path);
        g_free(path);
        path = NULL;
    }

    if (out_path != NULL)
    {
        *out_path = g_steal_pointer(&path);
    }
    g_free(basename);
    g_free(stamp);
    g_date_time_unref(now);
    g_free(directory);
    g_object_unref(texture);

    return success;
}

static void
screenshot_plugin_take(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    MtPluginHost *host;
    GtkWindow *window;
    GError *error;
    gchar *path;

    (void)action;
    (void)parameter;

    host = user_data;
    window = host->get_parent_window(host);
    error = NULL;
    path = NULL;

    if (window == NULL || !screenshot_plugin_save_window(window, &path, &error))
    {
        host->show_toast(host, error != NULL ? error->message : _("Screenshot failed"));
        g_clear_error(&error);
        return;
    }

    host->show_toast(host, path);
    g_free(path);
}

G_MODULE_EXPORT const MtPluginInfo *
mt_plugin_query(void)
{
    return &screenshot_plugin_info;
}

G_MODULE_EXPORT gboolean
mt_plugin_activate(MtPluginHost *host, GError **error)
{
    static const gchar *accelerators[] = { "F12", NULL };

    (void)error;

    if (!host->add_action(host, "take-screenshot", screenshot_plugin_take, host, NULL))
    {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_EXISTS,
                    "The take-screenshot action is already registered");
        return FALSE;
    }

    host->set_accelerators(host, "app.take-screenshot", accelerators);

    return TRUE;
}

G_MODULE_EXPORT void
mt_plugin_deactivate(MtPluginHost *host)
{
    (void)host;
}
