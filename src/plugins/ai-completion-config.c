/*
 * ai-completion-config.c
 * 配置对话框：控件构建、保存逻辑、自动补全偏好开关。
 */

#include "ai-completion-private.h"
#include "ai-completion-features.h"
#include "ai-code-summary.h"

#include <adwaita.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <string.h>

static void
ai_completion_cost_warning_response(AdwMessageDialog *dialog,
                                    const gchar *response,
                                    gpointer user_data)
{
    AiConfigWidgets *widgets;

    widgets = user_data;
    gtk_window_destroy(GTK_WINDOW(dialog));
    if (g_strcmp0(response, "continue") == 0)
    {
        g_object_set_data(G_OBJECT(widgets->window),
                          "vellum-ai-summary-warning-accepted",
                          GINT_TO_POINTER(1));
        ai_completion_config_save_clicked(NULL, widgets);
    }
}

void
ai_completion_config_widgets_free(AiConfigWidgets *widgets)
{
    if (widgets != NULL)
    {
        if (widgets->language_items != NULL)
        {
            g_object_unref(widgets->language_items);
        }
        ai_code_summary_config_widgets_free(widgets->summary_widgets);
        g_free(widgets);
    }
}

void
ai_completion_config_save_clicked(GtkButton *button, gpointer user_data)
{
    AiConfigWidgets *widgets;
    const gchar *endpoint;
    const gchar *model;
    const gchar *api_key;
    gboolean auto_enabled;
    gint auto_delay;
    GError *error;

    (void)button;

    widgets = user_data;
    endpoint = gtk_editable_get_text(GTK_EDITABLE(widgets->endpoint_row));
    model = gtk_editable_get_text(GTK_EDITABLE(widgets->model_row));
    api_key = gtk_editable_get_text(GTK_EDITABLE(widgets->key_row));
    auto_enabled = adw_switch_row_get_active(widgets->auto_row);
    auto_delay = (gint)adw_spin_row_get_value(widgets->delay_row);
    error = NULL;

    if (!g_str_has_prefix(endpoint, "https://") && !g_str_has_prefix(endpoint, "http://"))
    {
        widgets->host->show_toast(widgets->host, _("AI endpoint must begin with https:// or http://"));
        return;
    }

    if (*model == '\0' || *api_key == '\0')
    {
        widgets->host->show_toast(widgets->host, _("AI model and API key are required"));
        return;
    }

    if (ai_code_summary_config_requires_cost_warning(widgets->summary_widgets) &&
        g_object_get_data(G_OBJECT(widgets->window), "vellum-ai-summary-warning-accepted") == NULL)
    {
        AdwMessageDialog *dialog;

        dialog = ADW_MESSAGE_DIALOG(adw_message_dialog_new(widgets->window,
                                                            _("Frequent AI summaries may use many tokens"),
                                                            _("The selected interval is below 30 modified lines. Automatic summaries can send code to your configured AI service very often and may significantly increase token consumption.")));
        adw_message_dialog_add_response(dialog, "cancel", _("Cancel"));
        adw_message_dialog_add_response(dialog, "continue", _("Enable Anyway"));
        adw_message_dialog_set_response_appearance(dialog, "continue", ADW_RESPONSE_DESTRUCTIVE);
        g_signal_connect(dialog, "response", G_CALLBACK(ai_completion_cost_warning_response), widgets);
        gtk_window_present(GTK_WINDOW(dialog));
        return;
    }

    if (ai_completion_save_settings(endpoint, model, api_key, auto_enabled,
                                    widgets->language_items,
                                    widgets->summary_widgets,
                                    &error))
    {
        widgets->host->show_toast(widgets->host, _("AI completion settings saved"));
        ai_auto_enabled = auto_enabled;
        ai_auto_fix_enabled = adw_switch_row_get_active(widgets->auto_fix_row);
        ai_features_set_base_delay((guint)auto_delay);
        {
            /* 持久化 "include-summary" 开关到同一 ini（保持 0600 权限）。 */
            GKeyFile *extra = ai_completion_load_settings();
            gchar *extra_path;
            gchar *extra_contents;

            g_key_file_set_boolean(extra, AI_COMPLETION_GROUP, "include-summary",
                                    adw_switch_row_get_active(widgets->include_row));
            g_key_file_set_integer(extra, AI_COMPLETION_GROUP, "context-mode",
                                   (gint)adw_combo_row_get_selected(widgets->context_row));
            g_key_file_set_boolean(extra, AI_COMPLETION_GROUP, "save-context-hidden",
                                   adw_switch_row_get_active(widgets->save_ctx_row));
            g_key_file_set_boolean(extra, AI_COMPLETION_GROUP, "auto-error-fix",
                                   adw_switch_row_get_active(widgets->auto_fix_row));
            g_key_file_set_integer(extra, AI_COMPLETION_GROUP, "auto-delay", auto_delay);
            extra_contents = g_key_file_to_data(extra, NULL, NULL);
            extra_path = ai_completion_config_path();
            if (extra_contents != NULL)
            {
                if (g_file_set_contents(extra_path, extra_contents, (gssize)strlen(extra_contents), NULL))
                {
                    g_chmod(extra_path, 0600);
                }
                g_free(extra_contents);
            }
            g_free(extra_path);
            g_key_file_unref(extra);
        }
        gtk_window_destroy(widgets->window);
    }
    else
    {
        gchar *message;

        message = g_strdup_printf(_("Unable to save AI settings: %s"), error->message);
        widgets->host->show_toast(widgets->host, message);
        g_free(message);
        g_clear_error(&error);
    }
}

gboolean
ai_completion_pref_auto_get(gpointer user_data)
{
    (void)user_data;

    return ai_auto_enabled;
}

void
ai_completion_pref_auto_set(gboolean value, gpointer user_data)
{
    (void)user_data;

    ai_auto_enabled = value;
    ai_completion_set_auto_enabled(value);
}