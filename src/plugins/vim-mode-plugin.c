/*
 * vim-mode-plugin.c
 * 独立的轻量 Vim 输入模式；默认由扩展管理器关闭即可恢复普通编辑行为。
 */

#include "mt-plugin.h"

#include <gtk/gtk.h>
#include <glib/gi18n.h>
#include <gmodule.h>

typedef struct _VimMode VimMode;

struct _VimMode
{
    MtPluginHost *host;
    gboolean insert_mode;
    gboolean pending_delete;
    gboolean pending_yank;
    gboolean pending_change;
    gboolean pending_goto;
    GtkWindow *command_window;
    GtkEntry *command_entry;
};

static const MtPluginInfo vim_mode_plugin_info = {
    MT_PLUGIN_API_VERSION,
    "io.github.vellum.vim-mode",
    "Vi Mode",
    "A lightweight Vi-compatible modal editor with normal, insert and command input states",
    "0.2.0"
};

static VimMode *vim_mode;

static void
vim_mode_set_normal(VimMode *mode)
{
    mode->insert_mode = FALSE;
    mode->pending_delete = FALSE;
    mode->pending_yank = FALSE;
    mode->pending_change = FALSE;
    mode->pending_goto = FALSE;
    mode->host->show_toast(mode->host, _("Vi: Normal mode"));
}

static void
vim_mode_set_insert(VimMode *mode)
{
    mode->insert_mode = TRUE;
    mode->pending_delete = FALSE;
    mode->pending_yank = FALSE;
    mode->pending_change = FALSE;
    mode->pending_goto = FALSE;
    mode->host->show_toast(mode->host, _("Vi: Insert mode"));
}

static gboolean
vim_mode_run(VimMode *mode, MtPluginEditorCommand command)
{
    return mode->host->run_editor_command(mode->host, command);
}

static void
vim_mode_command_window_destroyed(GtkWidget *widget, gpointer user_data)
{
    VimMode *mode;

    (void)widget;
    mode = user_data;
    mode->command_window = NULL;
    mode->command_entry = NULL;
}

static void
vim_mode_command_activate(GtkEntry *entry, gpointer user_data)
{
    VimMode *mode;
    const gchar *command;

    mode = user_data;
    command = gtk_editable_get_text(GTK_EDITABLE(entry));
    if (g_strcmp0(command, "w") == 0)
    {
        vim_mode_run(mode, MT_PLUGIN_EDITOR_SAVE);
    }
    else if (g_strcmp0(command, "wq") == 0 || g_strcmp0(command, "x") == 0)
    {
        vim_mode_run(mode, MT_PLUGIN_EDITOR_SAVE_AND_CLOSE);
    }
    else if (g_strcmp0(command, "q") == 0)
    {
        vim_mode_run(mode, MT_PLUGIN_EDITOR_CLOSE);
    }
    else if (g_strcmp0(command, "q!") == 0)
    {
        vim_mode_run(mode, MT_PLUGIN_EDITOR_FORCE_CLOSE);
    }
    else if (g_strcmp0(command, "help") == 0)
    {
        mode->host->show_toast(mode->host,
                               _("Vi commands: :w, :wq, :x, :q, :q!, :help; normal keys: h j k l w b 0 $ G gg i a o x d d d y y c c p u"));
    }
    else
    {
        gchar *message;

        message = g_strdup_printf(_("Unsupported Vi command: :%s"), command);
        mode->host->show_toast(mode->host, message);
        g_free(message);
    }

    if (mode->command_window != NULL)
    {
        gtk_window_destroy(mode->command_window);
    }
}

static gboolean
vim_mode_command_key_pressed(GtkEventControllerKey *controller,
                             guint keyval,
                             guint keycode,
                             GdkModifierType state,
                             gpointer user_data)
{
    VimMode *mode;

    (void)controller;
    (void)keycode;
    (void)state;
    mode = user_data;
    if (keyval == GDK_KEY_Escape && mode->command_window != NULL)
    {
        gtk_window_destroy(mode->command_window);
        return TRUE;
    }
    return FALSE;
}

static void
vim_mode_open_command(VimMode *mode)
{
    GtkWidget *box;
    GtkWidget *prefix;
    GtkEventController *controller;
    GtkWindow *parent;

    if (mode->command_window != NULL)
    {
        gtk_window_present(mode->command_window);
        return;
    }

    parent = mode->host->get_parent_window != NULL ?
             mode->host->get_parent_window(mode->host) : NULL;
    mode->command_window = GTK_WINDOW(gtk_window_new());
    gtk_window_set_title(mode->command_window, _("Vi Command"));
    gtk_window_set_transient_for(mode->command_window, parent);
    gtk_window_set_modal(mode->command_window, TRUE);
    gtk_window_set_resizable(mode->command_window, FALSE);
    gtk_window_set_default_size(mode->command_window, 520, -1);
    box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_top(box, 12);
    gtk_widget_set_margin_bottom(box, 12);
    gtk_widget_set_margin_start(box, 16);
    gtk_widget_set_margin_end(box, 16);
    prefix = gtk_label_new(":");
    mode->command_entry = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(mode->command_entry, _("w, wq, x, q, q!, help"));
    gtk_widget_set_hexpand(GTK_WIDGET(mode->command_entry), TRUE);
    gtk_box_append(GTK_BOX(box), prefix);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(mode->command_entry));
    gtk_window_set_child(mode->command_window, box);
    g_signal_connect(mode->command_window,
                     "destroy",
                     G_CALLBACK(vim_mode_command_window_destroyed),
                     mode);
    g_signal_connect(mode->command_entry,
                     "activate",
                     G_CALLBACK(vim_mode_command_activate),
                     mode);
    controller = gtk_event_controller_key_new();
    g_signal_connect(controller,
                     "key-pressed",
                     G_CALLBACK(vim_mode_command_key_pressed),
                     mode);
    gtk_widget_add_controller(GTK_WIDGET(mode->command_entry), controller);
    gtk_window_present(mode->command_window);
    gtk_widget_grab_focus(GTK_WIDGET(mode->command_entry));
}

static gboolean
vim_mode_handle_key(MtPluginHost *host,
                    guint keyval,
                    guint keycode,
                    guint state,
                    gpointer user_data)
{
    VimMode *mode;
    gunichar character;

    (void)host;
    (void)keycode;
    mode = user_data;

    if (mode->insert_mode)
    {
        if (keyval == GDK_KEY_Escape)
        {
            vim_mode_set_normal(mode);
            return TRUE;
        }
        return FALSE;
    }

    if ((state & GDK_CONTROL_MASK) != 0 && keyval == GDK_KEY_r)
    {
        vim_mode_run(mode, MT_PLUGIN_EDITOR_REDO);
        return TRUE;
    }
    if ((state & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK | GDK_META_MASK)) != 0)
    {
        return FALSE;
    }
    if (keyval == GDK_KEY_Escape)
    {
        mode->pending_delete = FALSE;
        mode->pending_yank = FALSE;
        mode->pending_change = FALSE;
        mode->pending_goto = FALSE;
        return TRUE;
    }

    character = gdk_keyval_to_unicode(keyval);
    if (character == 0)
    {
        return FALSE;
    }

    if (mode->pending_delete)
    {
        mode->pending_delete = FALSE;
        if (character == 'd')
        {
            vim_mode_run(mode, MT_PLUGIN_EDITOR_DELETE_CURRENT_LINE);
        }
        return TRUE;
    }
    if (mode->pending_yank)
    {
        mode->pending_yank = FALSE;
        if (character == 'y')
        {
            vim_mode_run(mode, MT_PLUGIN_EDITOR_YANK_LINE);
        }
        return TRUE;
    }
    if (mode->pending_change)
    {
        mode->pending_change = FALSE;
        if (character == 'c')
        {
            vim_mode_run(mode, MT_PLUGIN_EDITOR_CHANGE_LINE);
            vim_mode_set_insert(mode);
        }
        return TRUE;
    }
    if (mode->pending_goto)
    {
        mode->pending_goto = FALSE;
        if (character == 'g')
        {
            vim_mode_run(mode, MT_PLUGIN_EDITOR_MOVE_DOCUMENT_START);
        }
        return TRUE;
    }

    switch (character)
    {
        case ':':
            vim_mode_open_command(mode);
            return TRUE;
        case 'i':
            vim_mode_set_insert(mode);
            return TRUE;
        case 'a':
            vim_mode_run(mode, MT_PLUGIN_EDITOR_MOVE_RIGHT);
            vim_mode_set_insert(mode);
            return TRUE;
        case 'o':
            vim_mode_run(mode, MT_PLUGIN_EDITOR_MOVE_LINE_END);
            mode->host->insert_text(mode->host, "\n");
            vim_mode_set_insert(mode);
            return TRUE;
        case 'h':
            vim_mode_run(mode, MT_PLUGIN_EDITOR_MOVE_LEFT);
            return TRUE;
        case 'j':
            vim_mode_run(mode, MT_PLUGIN_EDITOR_MOVE_DOWN);
            return TRUE;
        case 'k':
            vim_mode_run(mode, MT_PLUGIN_EDITOR_MOVE_UP);
            return TRUE;
        case 'l':
            vim_mode_run(mode, MT_PLUGIN_EDITOR_MOVE_RIGHT);
            return TRUE;
        case 'w':
            vim_mode_run(mode, MT_PLUGIN_EDITOR_MOVE_WORD_FORWARD);
            return TRUE;
        case 'b':
            vim_mode_run(mode, MT_PLUGIN_EDITOR_MOVE_WORD_BACKWARD);
            return TRUE;
        case '0':
            vim_mode_run(mode, MT_PLUGIN_EDITOR_MOVE_LINE_START);
            return TRUE;
        case '$':
            vim_mode_run(mode, MT_PLUGIN_EDITOR_MOVE_LINE_END);
            return TRUE;
        case 'G':
            vim_mode_run(mode, MT_PLUGIN_EDITOR_MOVE_DOCUMENT_END);
            return TRUE;
        case 'g':
            mode->pending_goto = TRUE;
            return TRUE;
        case 'x':
            vim_mode_run(mode, MT_PLUGIN_EDITOR_DELETE_FORWARD_CHAR);
            return TRUE;
        case 'd':
            mode->pending_delete = TRUE;
            return TRUE;
        case 'y':
            mode->pending_yank = TRUE;
            return TRUE;
        case 'c':
            mode->pending_change = TRUE;
            return TRUE;
        case 'p':
            vim_mode_run(mode, MT_PLUGIN_EDITOR_PASTE);
            return TRUE;
        case 'u':
            vim_mode_run(mode, MT_PLUGIN_EDITOR_UNDO);
            return TRUE;
        default:
            return TRUE;
    }
}

G_MODULE_EXPORT const MtPluginInfo *
mt_plugin_query(void)
{
    return &vim_mode_plugin_info;
}

G_MODULE_EXPORT gboolean
mt_plugin_activate(MtPluginHost *host, GError **error)
{
    if (host->add_key_handler == NULL || host->run_editor_command == NULL ||
        host->get_parent_window == NULL)
    {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_NOT_SUPPORTED,
                    "Vellum host does not provide the Vi mode input services");
        return FALSE;
    }

    vim_mode = g_new0(VimMode, 1);
    vim_mode->host = host;
    if (!host->add_key_handler(host, vim_mode_handle_key, vim_mode, NULL))
    {
        g_free(vim_mode);
        vim_mode = NULL;
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_FAILED,
                    "Unable to register the Vim mode input handler");
        return FALSE;
    }
    host->show_toast(host, _("Vi mode enabled: press i to insert, Escape for normal mode, and : for commands"));

    return TRUE;
}

G_MODULE_EXPORT void
mt_plugin_deactivate(MtPluginHost *host)
{
    (void)host;

    if (vim_mode != NULL)
    {
        if (vim_mode->command_window != NULL)
        {
            gtk_window_destroy(vim_mode->command_window);
        }
        g_free(vim_mode);
        vim_mode = NULL;
    }
}
