/*
 * timestamp-plugin.c
 * 示例插件：按 Ctrl+Shift+T 在当前光标位置插入 ISO 8601 本地时间。
 */

#include "mt-plugin.h"

#include <gmodule.h>

static const MtPluginInfo timestamp_plugin_info = {
    MT_PLUGIN_API_VERSION,
    "io.github.vellum.timestamp",
    "Timestamp",
    "Insert the current local date and time",
    "0.1.0"
};

static const gchar *
timestamp_plugin_get_toast_message(void)
{
    const gchar * const *languages;

    languages = g_get_language_names();
    if (languages != NULL && languages[0] != NULL && g_str_has_prefix(languages[0], "zh"))
    {
        return "已插入时间戳";
    }

    return "Timestamp inserted";
}

static void
insert_timestamp(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    MtPluginHost *host;
    GDateTime *now;
    gchar *timestamp;

    (void)action;
    (void)parameter;

    host = user_data;
    now = g_date_time_new_now_local();
    timestamp = g_date_time_format(now, "%Y-%m-%dT%H:%M:%S%z");
    host->insert_text(host, timestamp);
    host->show_toast(host, timestamp_plugin_get_toast_message());

    g_free(timestamp);
    g_date_time_unref(now);
}

G_MODULE_EXPORT const MtPluginInfo *
mt_plugin_query(void)
{
    return &timestamp_plugin_info;
}

G_MODULE_EXPORT gboolean
mt_plugin_activate(MtPluginHost *host, GError **error)
{
    static const gchar *accelerators[] = { "<Primary><Shift>t", NULL };

    (void)error;

    if (!host->add_action(host, "insert-timestamp", insert_timestamp, host, NULL))
    {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_EXISTS,
                    "The insert-timestamp action is already registered");
        return FALSE;
    }

    host->set_accelerators(host, "app.insert-timestamp", accelerators);

    return TRUE;
}

G_MODULE_EXPORT void
mt_plugin_deactivate(MtPluginHost *host)
{
    (void)host;
}
