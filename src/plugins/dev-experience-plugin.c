/*
 * dev-experience-plugin.c
 * 开发体验优化插件：构建/运行 + 编译器报错红行描述 + 断点调试。
 *
 * 报错红行：构建输出按 GCC / Clang / MSVC 三种格式解析，把出错行用红色
 * 波浪下划线标在编辑器里，悬停显示报错描述；点击输出里的错误行可跳转。
 * 断点调试：gutter 点击设置断点，面板管理断点；内置 gdb 机器接口调试，
 * 支持继续/单步/步入/步出/停止，并显示当前停驻行的局部变量。
 */

#include "mt-plugin.h"

#include <adwaita.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <gmodule.h>
#include <string.h>
#include <stdlib.h>

#define DEV_XP_GROUP "Dev Experience"

#define DEV_XP_OLD_BUILD_COMMAND "cc -Wall -Wextra \"${file}\" -o \"${dir}/vellum-run\""
#define DEV_XP_OLD_RUN_COMMAND "\"${dir}/vellum-run\""
#define DEV_XP_DEFAULT_BUILD_COMMAND "cc -Wall -Wextra \"${file}\" -o \"${dir}/${name}\""
#define DEV_XP_DEFAULT_RUN_COMMAND "\"${dir}/${name}\""
#define DEV_XP_DEFAULT_DEBUGGER "gdb"
#define DEV_XP_DEFAULT_PROFILE "debug"

#define TAG_COMMAND "br-command"
#define TAG_ERROR "br-error"
#define TAG_WARNING "br-warning"
#define TAG_NOTE "br-note"
#define TAG_INFO "br-info"

typedef enum
{
    DEV_XP_MODE_BUILD,
    DEV_XP_MODE_RUN,
    DEV_XP_MODE_DEBUG
} DevXpMode;

typedef enum
{
    COMPILER_SEV_ERROR,
    COMPILER_SEV_WARNING,
    COMPILER_SEV_NOTE
} CompilerSeverity;

typedef struct
{
    gchar *file;
    gint line;
    gint column;
    CompilerSeverity severity;
    gchar *message;
    gchar *raw;
} CompilerError;

typedef struct
{
    gint line;
    gchar *condition;
} Breakpoint;

typedef enum
{
    DEBUG_STATE_STOPPED,
    DEBUG_STATE_RUNNING
} DebugState;
typedef struct _DevXpPlugin DevXpPlugin;
typedef struct _DevXpRequest DevXpRequest;
typedef struct _DevXpConfigWidgets DevXpConfigWidgets;
struct _DevXpPlugin
{
    MtPluginHost *host;
    GtkWidget *panel;
    GtkTextBuffer *output_buffer;
    GSubprocess *process;
    gchar *working_directory;
    gchar *build_command;
    gchar *run_command;
    gchar *build_profile;
    GPtrArray *errors;
    GPtrArray *breakpoints;
    GSubprocess *debug_process;
    GInputStream *debug_stdout;
    GOutputStream *debug_stdin;
    DebugState debug_state;
    gchar *debugger_command;
    gchar *current_program;
    GString *debug_output;
    GtkWidget *breakpoint_list_box;
    GtkWidget *variables_view;
    GtkLabel *debug_status_label;
    GtkButton *debug_run_button;
    GtkButton *debug_step_button;
    GtkButton *debug_step_over_button;
    GtkButton *debug_step_out_button;
    GtkButton *debug_stop_button;
    GtkButton *breakpoint_button;
    GtkEntry *breakpoint_entry;
};
struct _DevXpRequest
{
    DevXpPlugin *plugin;
    DevXpMode mode;
    gboolean run_after_success;
    GInputStream *stdout_stream;
    GInputStream *stderr_stream;
    gchar stdout_buffer[4096];
    gchar stderr_buffer[4096];
    gboolean stdout_done;
    gboolean stderr_done;
    GString *output;
};
struct _DevXpConfigWidgets
{
    DevXpPlugin *plugin;
    AdwEntryRow *directory_row;
    AdwEntryRow *build_row;
    AdwEntryRow *run_row;
    AdwEntryRow *profile_row;
    AdwEntryRow *debugger_row;
    GtkWindow *window;
};










static DevXpPlugin *dev_xp_plugin;

static const MtPluginInfo dev_xp_plugin_info = {
    MT_PLUGIN_API_VERSION,
    "io.github.vellum.dev-experience",
    "Dev Experience",
    "Build, run and debug with compiler error underlines and breakpoints",
    "1.1.0"
};

/* —— 配置 —— */

static gchar *
dev_xp_config_path(void)
{
    gchar *directory;
    gchar *path;

    directory = g_build_filename(g_get_user_config_dir(), "vellum", NULL);
    g_mkdir_with_parents(directory, 0700);
    path = g_build_filename(directory, "dev-experience.ini", NULL);
    g_free(directory);

    return path;
}

static gchar *
dev_xp_get_value(GKeyFile *settings, const gchar *key, const gchar *fallback)
{
    gchar *value;

    value = g_key_file_get_string(settings, DEV_XP_GROUP, key, NULL);
    if (value == NULL)
    {
        value = g_strdup(fallback);
    }

    return value;
}

static void
dev_xp_migrate_old_config(const gchar *new_path)
{
    gchar *directory;
    gchar *old_path;
    GKeyFile *old_settings;

    directory = g_build_filename(g_get_user_config_dir(), "vellum", NULL);
    old_path = g_build_filename(directory, "build-run.ini", NULL);

    if (!g_file_test(old_path, G_FILE_TEST_IS_REGULAR))
    {
        g_free(old_path);
        g_free(directory);
        return;
    }

    old_settings = g_key_file_new();
    if (g_key_file_load_from_file(old_settings, old_path, G_KEY_FILE_NONE, NULL))
    {
        gchar *working_dir = g_key_file_get_string(old_settings, "Build Run",
                                                    "working-directory", NULL);
        gchar *build_cmd = g_key_file_get_string(old_settings, "Build Run",
                                                   "build-command", NULL);
        gchar *run_cmd = g_key_file_get_string(old_settings, "Build Run",
                                                "run-command", NULL);
        gchar *profile = g_key_file_get_string(old_settings, "Build Run",
                                                 "build-profile", NULL);
        gchar *debugger = g_key_file_get_string(old_settings, "Build Run",
                                                  "debugger", NULL);
        GKeyFile *new_settings = g_key_file_new();

        if (working_dir) g_key_file_set_string(new_settings, DEV_XP_GROUP,
                                                "working-directory", working_dir);
        if (build_cmd) g_key_file_set_string(new_settings, DEV_XP_GROUP,
                                               "build-command", build_cmd);
        if (run_cmd) g_key_file_set_string(new_settings, DEV_XP_GROUP,
                                            "run-command", run_cmd);
        if (profile) g_key_file_set_string(new_settings, DEV_XP_GROUP,
                                            "build-profile", profile);
        if (debugger) g_key_file_set_string(new_settings, DEV_XP_GROUP,
                                             "debugger", debugger);

        g_key_file_save_to_file(new_settings, new_path, NULL);
        g_key_file_unref(new_settings);
        g_remove(old_path);
    }

    g_key_file_unref(old_settings);
    g_free(old_path);
    g_free(directory);
}

static void
dev_xp_load_settings(DevXpPlugin *plugin)
{
    GKeyFile *settings;
    gchar *path;

    path = dev_xp_config_path();
    dev_xp_migrate_old_config(path);

    settings = g_key_file_new();
    g_key_file_load_from_file(settings, path, G_KEY_FILE_NONE, NULL);

    plugin->working_directory = dev_xp_get_value(settings, "working-directory", "");
    plugin->build_command = dev_xp_get_value(settings, "build-command",
                                                DEV_XP_DEFAULT_BUILD_COMMAND);
    if (g_strcmp0(plugin->build_command, DEV_XP_OLD_BUILD_COMMAND) == 0)
    {
        g_free(plugin->build_command);
        plugin->build_command = g_strdup(DEV_XP_DEFAULT_BUILD_COMMAND);
    }
    plugin->run_command = dev_xp_get_value(settings, "run-command",
                                              DEV_XP_DEFAULT_RUN_COMMAND);
    if (g_strcmp0(plugin->run_command, DEV_XP_OLD_RUN_COMMAND) == 0)
    {
        g_free(plugin->run_command);
        plugin->run_command = g_strdup(DEV_XP_DEFAULT_RUN_COMMAND);
    }
    plugin->build_profile = dev_xp_get_value(settings, "build-profile",
                                                DEV_XP_DEFAULT_PROFILE);
    plugin->debugger_command = dev_xp_get_value(settings, "debugger",
                                                   DEV_XP_DEFAULT_DEBUGGER);
    g_free(path);
    g_key_file_unref(settings);
}

static gboolean
dev_xp_save_settings(DevXpPlugin *plugin, GError **error)
{
    GKeyFile *settings;
    gchar *path;
    gchar *contents;
    gsize length;
    gboolean saved;

    settings = g_key_file_new();
    g_key_file_set_string(settings, DEV_XP_GROUP, "working-directory", plugin->working_directory);
    g_key_file_set_string(settings, DEV_XP_GROUP, "build-command", plugin->build_command);
    g_key_file_set_string(settings, DEV_XP_GROUP, "run-command", plugin->run_command);
    g_key_file_set_string(settings, DEV_XP_GROUP, "build-profile", plugin->build_profile);
    g_key_file_set_string(settings, DEV_XP_GROUP, "debugger", plugin->debugger_command);
    contents = g_key_file_to_data(settings, &length, error);
    if (contents == NULL)
    {
        g_key_file_unref(settings);
        return FALSE;
    }

    path = dev_xp_config_path();
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

/* —— 输出面板 —— */

static void
dev_xp_append_output(DevXpPlugin *plugin, const gchar *text, const gchar *tag_name)
{
    GtkTextIter end;

    if (plugin->output_buffer == NULL || text == NULL)
    {
        return;
    }

    gtk_text_buffer_get_end_iter(plugin->output_buffer, &end);
    if (tag_name != NULL)
    {
        gtk_text_buffer_insert_with_tags_by_name(plugin->output_buffer, &end,
                                                 text, -1, tag_name, NULL);
    }
    else
    {
        gtk_text_buffer_insert(plugin->output_buffer, &end, text, -1);
    }
}

static void
dev_xp_append_command(DevXpPlugin *plugin, const gchar *text)
{
    dev_xp_append_output(plugin, text, TAG_COMMAND);
}

static void
dev_xp_append_error(DevXpPlugin *plugin, const gchar *text)
{
    dev_xp_append_output(plugin, text, TAG_ERROR);
}

static void
dev_xp_append_warning(DevXpPlugin *plugin, const gchar *text)
{
    dev_xp_append_output(plugin, text, TAG_WARNING);
}

static void
dev_xp_append_info(DevXpPlugin *plugin, const gchar *text)
{
    dev_xp_append_output(plugin, text, TAG_INFO);
}

static void
dev_xp_clear_output(DevXpPlugin *plugin)
{
    if (plugin->output_buffer != NULL)
    {
        gtk_text_buffer_set_text(plugin->output_buffer, "", -1);
    }
}

static void
dev_xp_setup_output_tags(DevXpPlugin *plugin)
{
    GtkTextTag *tag;

    if (plugin->output_buffer == NULL)
    {
        return;
    }

    tag = gtk_text_buffer_create_tag(plugin->output_buffer, TAG_COMMAND, NULL);
    g_object_set(tag, "weight", PANGO_WEIGHT_BOLD, NULL);
    g_object_set(tag, "foreground", "#2e7e7e", NULL);

    tag = gtk_text_buffer_create_tag(plugin->output_buffer, TAG_ERROR, NULL);
    g_object_set(tag, "weight", PANGO_WEIGHT_BOLD, NULL);
    g_object_set(tag, "foreground", "#f22b2b", NULL);

    tag = gtk_text_buffer_create_tag(plugin->output_buffer, TAG_WARNING, NULL);
    g_object_set(tag, "foreground", "#e07a24", NULL);

    tag = gtk_text_buffer_create_tag(plugin->output_buffer, TAG_NOTE, NULL);
    g_object_set(tag, "foreground", "#7e3e7e", NULL);

    tag = gtk_text_buffer_create_tag(plugin->output_buffer, TAG_INFO, NULL);
    g_object_set(tag, "foreground", "#888888", NULL);
}

/* —— 编译器报错解析 —— */

static void
dev_xp_clear_errors(DevXpPlugin *plugin)
{
    guint i;

    if (plugin->errors == NULL)
    {
        return;
    }
    for (i = 0; i < plugin->errors->len; i++)
    {
        CompilerError *error;

        error = g_ptr_array_index(plugin->errors, i);
        g_free(error->file);
        g_free(error->message);
        g_free(error->raw);
        g_free(error);
    }
    g_ptr_array_set_size(plugin->errors, 0);
}

static void
dev_xp_apply_error_underline(DevXpPlugin *plugin, CompilerError *error)
{
    gchar *text;
    gint offset;
    gint length;

    if (plugin->host == NULL || error->file == NULL || error->line <= 0)
    {
        return;
    }

    {
        gchar *current_path;
        gchar *current_base;
        gchar *error_base;
        gboolean match;

        current_path = plugin->host->get_current_file_path(plugin->host);
        if (current_path == NULL)
        {
            g_free(current_path);
            return;
        }
        current_base = g_path_get_basename(current_path);
        error_base = g_path_get_basename(error->file);
        match = (g_strcmp0(current_base, error_base) == 0);
        g_free(current_base);
        g_free(error_base);
        g_free(current_path);
        if (!match)
        {
            return;
        }
    }

    text = plugin->host->get_current_text(plugin->host);
    if (text == NULL)
    {
        return;
    }

    {
        const gchar *p;
        gint line;
        gint byte_offset;

        line = 1;
        byte_offset = 0;
        p = text;
        while (*p != '\0' && line < error->line)
        {
            if (*p == '\n')
            {
                line++;
            }
            byte_offset++;
            p++;
        }
        offset = byte_offset;
    }

    {
        const gchar *p;
        gint length_bytes;

        p = text + offset;
        length_bytes = 0;
        while (*p != '\0' && *p != '\n')
        {
            length_bytes++;
            p++;
        }
        length = length_bytes;
    }

    g_free(text);

    if (length <= 0)
    {
        return;
    }

    {
        gchar *message;

        message = g_strdup_printf(_("%s:%d:%d: %s"),
                                  error->file, error->line, error->column,
                                  error->message != NULL ? error->message : "");
        plugin->host->show_error_underline(plugin->host, offset, length, message);
        g_free(message);
    }
}

/*
 * 解析一行编译器输出。支持 GCC / Clang / MSVC 三种格式。
 */
static gboolean
dev_xp_parse_compiler_line(const gchar *line, CompilerError **out_error)
{
    const gchar *p;
    gchar *file;
    gint file_len;
    const gchar *q;
    gint line_no;
    gint col_no;
    CompilerSeverity severity;
    const gchar *msg_start;
    const gchar *msg_end;
    gchar *message;

    if (line == NULL || *line == '\0')
    {
        return FALSE;
    }

    p = line;
    while (*p == ' ' || *p == '\t')
    {
        p++;
    }

    /* MSVC: path(line,col): severity Cxxxx: message */
    {
        const gchar *lp;
        const gchar *comma;
        const gchar *rparen;

        lp = strchr(p, '(');
        comma = lp != NULL ? strchr(lp + 1, ',') : NULL;
        rparen = comma != NULL ? strchr(comma + 1, ')') : NULL;
        if (lp != NULL && comma != NULL && rparen != NULL && rparen > comma)
        {
            file_len = (gint)(lp - p);
            if (file_len <= 0)
            {
                return FALSE;
            }
            file = g_strndup(p, (gsize)file_len);
            line_no = (gint)strtol(lp + 1, NULL, 10);
            col_no = (gint)strtol(comma + 1, NULL, 10);
            q = rparen + 1;
            while (*q == ' ' || *q == '\t')
            {
                q++;
            }
            if (*q == ':')
            {
                q++;
                while (*q == ' ' || *q == '\t')
                {
                    q++;
                }
            }
            if (g_ascii_strncasecmp(q, "error", 5) == 0)
            {
                severity = COMPILER_SEV_ERROR;
                msg_start = q + 5;
            }
            else if (g_ascii_strncasecmp(q, "warning", 7) == 0)
            {
                severity = COMPILER_SEV_WARNING;
                msg_start = q + 7;
            }
            else if (g_ascii_strncasecmp(q, "note", 4) == 0)
            {
                severity = COMPILER_SEV_NOTE;
                msg_start = q + 4;
            }
            else
            {
                g_free(file);
                return FALSE;
            }
            while (*msg_start == ' ' || *msg_start == '\t')
            {
                msg_start++;
            }
            msg_end = msg_start + strlen(msg_start);
            while (msg_end > msg_start && (*(msg_end - 1) == '\r' || *(msg_end - 1) == '\n'))
            {
                msg_end--;
            }
            message = g_strndup(msg_start, (gsize)(msg_end - msg_start));

            CompilerError *error;
            error = g_new0(CompilerError, 1);
            error->file = file;
            error->line = line_no;
            error->column = col_no;
            error->severity = severity;
            error->message = message;
            error->raw = g_strdup(line);
            *out_error = error;
            return TRUE;
        }
    }

    /* GCC/Clang: path:line:col: severity: message  或  path:line: severity: message */
    {
        const gchar *colon1;

        colon1 = strchr(p, ':');
        if (colon1 == NULL)
        {
            return FALSE;
        }
        file_len = (gint)(colon1 - p);
        if (file_len <= 0)
        {
            return FALSE;
        }
        file = g_strndup(p, (gsize)file_len);

        q = colon1 + 1;
        line_no = (gint)strtol(q, (gchar **)&q, 10);
        if (line_no <= 0)
        {
            g_free(file);
            return FALSE;
        }
        while (*q == ':' || *q == ' ' || *q == '\t')
        {
            q++;
        }
        if (g_ascii_isdigit(*q))
        {
            col_no = (gint)strtol(q, (gchar **)&q, 10);
            while (*q == ':' || *q == ' ' || *q == '\t')
            {
                q++;
            }
        }
        else
        {
            col_no = 1;
        }

        if (g_ascii_strncasecmp(q, "error", 5) == 0)
        {
            severity = COMPILER_SEV_ERROR;
            msg_start = q + 5;
        }
        else if (g_ascii_strncasecmp(q, "warning", 7) == 0)
        {
            severity = COMPILER_SEV_WARNING;
            msg_start = q + 7;
        }
        else if (g_ascii_strncasecmp(q, "note", 4) == 0)
        {
            severity = COMPILER_SEV_NOTE;
            msg_start = q + 4;
        }
        else
        {
            g_free(file);
            return FALSE;
        }
        while (*msg_start == ' ' || *msg_start == '\t')
        {
            msg_start++;
        }
        msg_end = msg_start + strlen(msg_start);
        while (msg_end > msg_start && (*(msg_end - 1) == '\r' || *(msg_end - 1) == '\n'))
        {
            msg_end--;
        }
        message = g_strndup(msg_start, (gsize)(msg_end - msg_start));

        CompilerError *error;
        error = g_new0(CompilerError, 1);
        error->file = file;
        error->line = line_no;
        error->column = col_no;
        error->severity = severity;
        error->message = message;
        error->raw = g_strdup(line);
        *out_error = error;
        return TRUE;
    }
}

static void
dev_xp_parse_errors(DevXpPlugin *plugin, const gchar *output)
{
    gchar **lines;
    guint i;

    if (output == NULL || *output == '\0')
    {
        return;
    }

    dev_xp_clear_errors(plugin);
    if (plugin->host != NULL)
    {
        plugin->host->clear_error_underlines(plugin->host);
    }

    lines = g_strsplit(output, "\n", -1);
    for (i = 0; lines[i] != NULL; i++)
    {
        CompilerError *error;

        if (dev_xp_parse_compiler_line(lines[i], &error))
        {
            g_ptr_array_add(plugin->errors, error);
            dev_xp_apply_error_underline(plugin, error);
        }
    }
    g_strfreev(lines);
}

/* —— 源文件名 / 命令展开 —— */

static gchar *
dev_xp_source_name(const gchar *file_path)
{
    gchar *base;
    gchar *dot;
    gchar *name;

    base = g_path_get_basename(file_path);
    dot = strrchr(base, '.');
    if (dot != NULL && dot != base)
    {
        name = g_strndup(base, (gsize)(dot - base));
    }
    else
    {
        name = g_strdup(base);
    }
    g_free(base);

    return name;
}

static gchar *
dev_xp_expand_command(DevXpPlugin *plugin,
                         const gchar *command,
                         const gchar *file_path,
                         const gchar *directory,
                         const gchar *root,
                         const gchar *name)
{
    GString *result;
    const gchar *cursor;

    result = g_string_new("");
    cursor = command;
    while (*cursor != '\0')
    {
        if (g_str_has_prefix(cursor, "${file}"))
        {
            g_string_append(result, file_path);
            cursor += 7;
        }
        else if (g_str_has_prefix(cursor, "${dir}"))
        {
            g_string_append(result, directory);
            cursor += 6;
        }
        else if (g_str_has_prefix(cursor, "${name}"))
        {
            g_string_append(result, name != NULL ? name : "");
            cursor += 7;
        }
        else if (g_str_has_prefix(cursor, "${root}"))
        {
            g_string_append(result, root);
            cursor += 7;
        }
        else if (g_str_has_prefix(cursor, "${profile}"))
        {
            g_string_append(result, plugin->build_profile != NULL ? plugin->build_profile : "");
            cursor += 10;
        }
        else
        {
            g_string_append_c(result, *cursor);
            cursor++;
        }
    }

    return g_string_free(result, FALSE);
}

static gchar *
dev_xp_get_working_directory(DevXpPlugin *plugin, const gchar *file_path)
{
    gchar *directory;

    if (plugin->working_directory != NULL && *plugin->working_directory != '\0' &&
        g_file_test(plugin->working_directory, G_FILE_TEST_IS_DIR))
    {
        return g_strdup(plugin->working_directory);
    }

    directory = g_path_get_dirname(file_path);
    return directory;
}

/* —— 进程启动 —— */

static void dev_xp_start(DevXpPlugin *plugin,
                            DevXpMode mode,
                            gboolean run_after_success);
static void dev_xp_process_wait(GObject *source,
                                   GAsyncResult *result,
                                   gpointer user_data);

static void
dev_xp_finish(DevXpRequest *request)
{
    DevXpPlugin *plugin;
    gboolean run_after_success;

    plugin = request->plugin;
    run_after_success = request->run_after_success;

    if (request->output != NULL && request->output->len > 0)
    {
        dev_xp_parse_errors(plugin, request->output->str);
    }

    if (g_subprocess_get_successful(plugin->process))
    {
        plugin->host->show_toast(plugin->host,
                                 request->mode == DEV_XP_MODE_BUILD ?
                                 _("Build completed") : _("Program exited successfully"));
        g_clear_object(&plugin->process);
        g_string_free(request->output, TRUE);
        g_free(request);
        if (run_after_success)
        {
            dev_xp_start(plugin, DEV_XP_MODE_RUN, FALSE);
        }
        return;
    }

    {
        gchar *message;

        message = g_strdup_printf(_("Command exited with status %d"),
                                  g_subprocess_get_exit_status(plugin->process));
        dev_xp_append_error(plugin, message);
        dev_xp_append_output(plugin, "\n", NULL);
        plugin->host->show_toast(plugin->host, message);
        g_free(message);
    }

    g_clear_object(&plugin->process);
    g_string_free(request->output, TRUE);
    g_free(request);
}

static void
dev_xp_process_wait(GObject *source, GAsyncResult *result, gpointer user_data)
{
    DevXpRequest *request;
    DevXpPlugin *plugin;
    GError *error;

    request = user_data;
    plugin = request->plugin;
    error = NULL;
    g_subprocess_wait_finish(G_SUBPROCESS(source), result, &error);
    if (error != NULL)
    {
        gchar *message;

        message = g_strdup_printf(_("Command failed to start or finish: %s"), error->message);
        dev_xp_append_error(plugin, message);
        dev_xp_append_output(plugin, "\n", NULL);
        plugin->host->show_toast(plugin->host, message);
        g_free(message);
        g_clear_error(&error);
    }

    dev_xp_finish(request);
}

static void
dev_xp_stream_ready(GObject *source, GAsyncResult *result, gpointer user_data)
{
    DevXpRequest *request;
    GInputStream *stream;
    gchar *buffer;
    gssize count;
    GError *error;

    request = user_data;
    stream = G_INPUT_STREAM(source);
    buffer = stream == request->stdout_stream ?
             request->stdout_buffer : request->stderr_buffer;
    error = NULL;
    count = g_input_stream_read_finish(stream, result, &error);

    if (count > 0)
    {
        gchar *chunk;

        chunk = g_strndup(buffer, (gsize)count);
        if (request->output != NULL)
        {
            g_string_append_len(request->output, chunk, (gsize)count);
        }
        dev_xp_append_output(request->plugin, chunk, NULL);
        g_free(chunk);
        g_input_stream_read_async(stream,
                                  buffer,
                                  (stream == request->stdout_stream ?
                                   sizeof(request->stdout_buffer) :
                                   sizeof(request->stderr_buffer)),
                                  G_PRIORITY_DEFAULT,
                                  NULL,
                                  dev_xp_stream_ready,
                                  request);
        return;
    }

    if (error != NULL)
    {
        g_clear_error(&error);
    }
    if (stream == request->stdout_stream)
    {
        request->stdout_done = TRUE;
    }
    else
    {
        request->stderr_done = TRUE;
    }
    if (request->stdout_done && request->stderr_done)
    {
        g_subprocess_wait_async(request->plugin->process,
                                NULL,
                                dev_xp_process_wait,
                                request);
    }
}

static void
dev_xp_start(DevXpPlugin *plugin,
                DevXpMode mode,
                gboolean run_after_success)
{
    gchar *file_path;
    gchar *file_directory;
    gchar *working_directory;
    gchar *source_name;
    gchar *expanded_command;
    gchar **argv;
    gint argc;
    GError *error;
    GSubprocessLauncher *launcher;
    DevXpRequest *request;
    const gchar *command;
    gchar *heading;

    if (plugin->process != NULL)
    {
        plugin->host->show_toast(plugin->host, _("A build or run command is already active"));
        return;
    }

    file_path = plugin->host->get_current_file_path(plugin->host);
    if (file_path == NULL)
    {
        plugin->host->show_toast(plugin->host, _("Save the current source file before building or running"));
        return;
    }

    command = mode == DEV_XP_MODE_BUILD ? plugin->build_command : plugin->run_command;
    if (command == NULL || *command == '\0')
    {
        plugin->host->show_toast(plugin->host,
                                 mode == DEV_XP_MODE_BUILD ?
                                 _("Configure a build command first") :
                                 _("Configure a run command first"));
        g_free(file_path);
        return;
    }

    file_directory = g_path_get_dirname(file_path);
    working_directory = dev_xp_get_working_directory(plugin, file_path);
    source_name = dev_xp_source_name(file_path);
    expanded_command = dev_xp_expand_command(plugin, command,
                                                file_path,
                                                file_directory,
                                                working_directory,
                                                source_name);
    g_free(source_name);
    argv = NULL;
    argc = 0;
    error = NULL;
    if (!g_shell_parse_argv(expanded_command, &argc, &argv, &error) || argc == 0)
    {
        gchar *message;

        message = g_strdup_printf(_("Unable to parse command: %s"), error->message);
        dev_xp_append_error(plugin, message);
        dev_xp_append_output(plugin, "\n", NULL);
        plugin->host->show_toast(plugin->host, message);
        g_free(message);
        g_clear_error(&error);
        g_strfreev(argv);
        g_free(expanded_command);
        g_free(working_directory);
        g_free(file_directory);
        g_free(file_path);
        return;
    }

    dev_xp_append_output(plugin, "\n$ ", NULL);
    dev_xp_append_command(plugin, expanded_command);
    dev_xp_append_output(plugin, "\n", NULL);
    heading = g_strdup_printf("%s…\n",
                              mode == DEV_XP_MODE_BUILD ? _("Building") : _("Running"));
    dev_xp_append_info(plugin, heading);
    g_free(heading);

    launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                         G_SUBPROCESS_FLAGS_STDERR_PIPE);
    g_subprocess_launcher_set_cwd(launcher, working_directory);
    plugin->process = g_subprocess_launcher_spawnv(launcher,
                                                    (const gchar * const *)argv,
                                                    &error);
    g_object_unref(launcher);
    g_strfreev(argv);
    g_free(expanded_command);
    g_free(working_directory);
    g_free(file_directory);
    g_free(file_path);

    if (plugin->process == NULL)
    {
        gchar *message;

        message = g_strdup_printf(_("Unable to start command: %s"), error->message);
        dev_xp_append_error(plugin, message);
        dev_xp_append_output(plugin, "\n", NULL);
        plugin->host->show_toast(plugin->host, message);
        g_free(message);
        g_clear_error(&error);
        return;
    }

    request = g_new0(DevXpRequest, 1);
    request->plugin = plugin;
    request->mode = mode;
    request->run_after_success = run_after_success;
    request->output = g_string_new("");
    request->stdout_stream = g_subprocess_get_stdout_pipe(plugin->process);
    request->stderr_stream = g_subprocess_get_stderr_pipe(plugin->process);
    if (request->stdout_stream != NULL)
    {
        g_input_stream_read_async(request->stdout_stream,
                                  request->stdout_buffer,
                                  sizeof(request->stdout_buffer),
                                  G_PRIORITY_DEFAULT,
                                  NULL,
                                  dev_xp_stream_ready,
                                  request);
    }
    if (request->stderr_stream != NULL)
    {
        g_input_stream_read_async(request->stderr_stream,
                                  request->stderr_buffer,
                                  sizeof(request->stderr_buffer),
                                  G_PRIORITY_DEFAULT,
                                  NULL,
                                  dev_xp_stream_ready,
                                  request);
    }
    if (request->stdout_stream == NULL && request->stderr_stream == NULL)
    {
        g_subprocess_wait_async(plugin->process,
                                NULL,
                                dev_xp_process_wait,
                                request);
    }
}

/* —— 断点管理 —— */

static void
dev_xp_refresh_breakpoint_ui(DevXpPlugin *plugin);

static void
dev_xp_sync_breakpoints_to_host(DevXpPlugin *plugin)
{
    guint i;

    if (plugin->host == NULL || plugin->breakpoints == NULL)
    {
        return;
    }
    plugin->host->clear_all_breakpoints(plugin->host);
    for (i = 0; i < plugin->breakpoints->len; i++)
    {
        Breakpoint *bp;

        bp = g_ptr_array_index(plugin->breakpoints, i);
        plugin->host->set_breakpoint(plugin->host, bp->line);
    }
}

static gboolean
dev_xp_has_breakpoint(DevXpPlugin *plugin, gint line)
{
    guint i;

    if (plugin->breakpoints == NULL)
    {
        return FALSE;
    }
    for (i = 0; i < plugin->breakpoints->len; i++)
    {
        Breakpoint *bp;

        bp = g_ptr_array_index(plugin->breakpoints, i);
        if (bp->line == line)
        {
            return TRUE;
        }
    }
    return FALSE;
}

static void
dev_xp_toggle_breakpoint_action(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    DevXpPlugin *plugin;
    gint line;

    (void)action;
    plugin = user_data;
    line = g_variant_get_int32(parameter);

    if (dev_xp_has_breakpoint(plugin, line))
    {
        Breakpoint *bp;
        guint i;

        for (i = 0; i < plugin->breakpoints->len; i++)
        {
            bp = g_ptr_array_index(plugin->breakpoints, i);
            if (bp->line == line)
            {
                g_ptr_array_remove_index_fast(plugin->breakpoints, i);
                g_free(bp->condition);
                g_free(bp);
                break;
            }
        }
        plugin->host->clear_breakpoint(plugin->host, line);
    }
    else
    {
        Breakpoint *bp;

        bp = g_new0(Breakpoint, 1);
        bp->line = line;
        bp->condition = NULL;
        g_ptr_array_add(plugin->breakpoints, bp);
        plugin->host->set_breakpoint(plugin->host, line);
    }

    dev_xp_refresh_breakpoint_ui(plugin);
}

static void
dev_xp_breakpoint_entry_activated(GtkEntry *entry, DevXpPlugin *plugin)
{
    const gchar *text;
    gchar *end;
    glong line;

    (void)entry;
    text = gtk_editable_get_text(GTK_EDITABLE(entry));
    if (text == NULL || *text == '\0')
    {
        return;
    }
    line = strtol(text, &end, 10);
    if (end == text || line <= 0)
    {
        return;
    }
    if (dev_xp_has_breakpoint(plugin, (gint)line))
    {
        return;
    }
    {
        Breakpoint *bp;

        bp = g_new0(Breakpoint, 1);
        bp->line = (gint)line;
        bp->condition = NULL;
        g_ptr_array_add(plugin->breakpoints, bp);
        plugin->host->set_breakpoint(plugin->host, bp->line);
        dev_xp_refresh_breakpoint_ui(plugin);
    }
    gtk_editable_set_text(GTK_EDITABLE(entry), "");
}

static void
dev_xp_breakpoint_add_clicked(GtkButton *button, DevXpPlugin *plugin)
{
    dev_xp_breakpoint_entry_activated(plugin->breakpoint_entry, plugin);
}

static void
dev_xp_breakpoint_remove_clicked(GtkButton *button, DevXpPlugin *plugin)
{
    gint line;

    line = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "br-bp-line"));
    if (line <= 0)
    {
        return;
    }
    {
        Breakpoint *bp;
        guint i;

        for (i = 0; i < plugin->breakpoints->len; i++)
        {
            bp = g_ptr_array_index(plugin->breakpoints, i);
            if (bp->line == line)
            {
                g_ptr_array_remove_index_fast(plugin->breakpoints, i);
                g_free(bp->condition);
                g_free(bp);
                break;
            }
        }
    }
    plugin->host->clear_breakpoint(plugin->host, line);
    dev_xp_refresh_breakpoint_ui(plugin);
}

static void
dev_xp_refresh_breakpoint_ui(DevXpPlugin *plugin)
{
    GtkWidget *box;
    guint i;

    if (plugin->breakpoint_list_box == NULL)
    {
        return;
    }
    box = plugin->breakpoint_list_box;

    while (gtk_widget_get_first_child(box) != NULL)
    {
        gtk_widget_unparent(gtk_widget_get_first_child(box));
    }

    if (plugin->breakpoints == NULL || plugin->breakpoints->len == 0)
    {
        GtkWidget *label;

        label = gtk_label_new(_("No breakpoints"));
        gtk_widget_set_halign(label, GTK_ALIGN_START);
        gtk_box_append(GTK_BOX(box), label);
        return;
    }

    for (i = 0; i < plugin->breakpoints->len; i++)
    {
        Breakpoint *bp;
        GtkWidget *row;
        GtkLabel *line_label;
        GtkButton *remove_button;

        bp = g_ptr_array_index(plugin->breakpoints, i);
        row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        line_label = GTK_LABEL(gtk_label_new(NULL));
        gtk_label_set_xalign(line_label, 0.0f);
        gtk_label_set_text(line_label, g_strdup_printf(":%d", bp->line));
        remove_button = GTK_BUTTON(gtk_button_new_from_icon_name("list-remove-symbolic"));
        g_object_set_data_full(G_OBJECT(remove_button), "br-bp-line",
                               GINT_TO_POINTER(bp->line), NULL);
        g_signal_connect(remove_button, "clicked",
                         G_CALLBACK(dev_xp_breakpoint_remove_clicked), plugin);
        gtk_box_append(GTK_BOX(row), GTK_WIDGET(line_label));
        gtk_box_append(GTK_BOX(row), GTK_WIDGET(remove_button));
        gtk_box_append(GTK_BOX(box), GTK_WIDGET(row));
    }
}

/* —— 调试：gdb 机器接口 —— */

typedef struct
{
    DevXpPlugin *plugin;
    gchar *buffer;
} DebugReadData;

static void
dev_xp_debug_send(DevXpPlugin *plugin, const gchar *command)
{
    GBytes *bytes;
    GError *error;

    if (plugin->debug_stdin == NULL || command == NULL)
    {
        return;
    }
    error = NULL;
    bytes = g_bytes_new(command, strlen(command));
    if (!g_output_stream_write_bytes(plugin->debug_stdin, bytes, NULL, &error))
    {
        g_warning("Failed to send gdb command: %s", error != NULL ? error->message : "");
        g_clear_error(&error);
    }
    g_bytes_unref(bytes);
}

static void
dev_xp_debug_continue(DevXpPlugin *plugin)
{
    dev_xp_debug_send(plugin, "-exec-continue\n");
    plugin->debug_state = DEBUG_STATE_RUNNING;
    if (plugin->debug_status_label != NULL)
    {
        gtk_label_set_text(plugin->debug_status_label, _("Running…"));
    }
}

static void
dev_xp_debug_step_over(DevXpPlugin *plugin)
{
    dev_xp_debug_send(plugin, "-exec-next\n");
    plugin->debug_state = DEBUG_STATE_RUNNING;
    if (plugin->debug_status_label != NULL)
    {
        gtk_label_set_text(plugin->debug_status_label, _("Stepping…"));
    }
}

static void
dev_xp_debug_step_into(DevXpPlugin *plugin)
{
    dev_xp_debug_send(plugin, "-exec-step\n");
    plugin->debug_state = DEBUG_STATE_RUNNING;
    if (plugin->debug_status_label != NULL)
    {
        gtk_label_set_text(plugin->debug_status_label, _("Stepping…"));
    }
}

static void
dev_xp_debug_step_out(DevXpPlugin *plugin)
{
    dev_xp_debug_send(plugin, "-exec-finish\n");
    plugin->debug_state = DEBUG_STATE_RUNNING;
    if (plugin->debug_status_label != NULL)
    {
        gtk_label_set_text(plugin->debug_status_label, _("Stepping…"));
    }
}

static void
dev_xp_debug_stop(DevXpPlugin *plugin);

static void
dev_xp_debug_query_locals(DevXpPlugin *plugin)
{
    dev_xp_debug_send(plugin, "-stack-list-locals --simple-values\n");
}

static void
dev_xp_debug_parse_stopped(DevXpPlugin *plugin, const gchar *output)
{
    const gchar *p;
    gchar *file;
    gint line;

    file = NULL;
    line = 0;
    p = strstr(output, "frame={");
    if (p != NULL)
    {
        const gchar *fp;

        fp = strstr(p, "file=");
        if (fp != NULL)
        {
            const gchar *start;
            const gchar *end;

            start = strchr(fp + 5, '"');
            if (start != NULL)
            {
                end = strchr(start + 1, '"');
                if (end != NULL)
                {
                    file = g_strndup(start + 1, (gsize)(end - start - 1));
                }
            }
        }
        {
            const gchar *lp;

            lp = strstr(p, "line=");
            if (lp != NULL)
            {
                const gchar *start;

                start = strchr(lp + 5, '"');
                if (start != NULL)
                {
                    const gchar *end;

                    end = strchr(start + 1, '"');
                    if (end != NULL)
                    {
                        line = (gint)strtol(start + 1, NULL, 10);
                    }
                }
            }
        }
    }

    if (file != NULL && line > 0)
    {
        plugin->host->scroll_to_line(plugin->host, line);
        g_free(file);
    }
    else
    {
        g_free(file);
    }

    dev_xp_debug_query_locals(plugin);
}

static void
dev_xp_debug_handle_line(DevXpPlugin *plugin, const gchar *line)
{
    if (line == NULL || *line == '\0')
    {
        return;
    }

    if (strcmp(line, "(gdb)") == 0)
    {
        if (plugin->debug_state == DEBUG_STATE_RUNNING)
        {
            plugin->debug_state = DEBUG_STATE_STOPPED;
            if (plugin->debug_status_label != NULL)
            {
                gtk_label_set_text(plugin->debug_status_label, _("Stopped"));
            }
            dev_xp_debug_query_locals(plugin);
        }
        return;
    }

    if (strncmp(line, "*stopped", 8) == 0)
    {
        dev_xp_debug_parse_stopped(plugin, line);
        return;
    }

    if (strncmp(line, "=stack-list-locals", 18) == 0 ||
        strncmp(line, "=locals", 7) == 0)
    {
        if (plugin->variables_view != NULL)
        {
            GtkTextBuffer *buffer;
            GtkTextIter end;

            buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(plugin->variables_view));
            gtk_text_buffer_get_end_iter(buffer, &end);
            gtk_text_buffer_insert(buffer, &end, line, -1);
            gtk_text_buffer_insert(buffer, &end, "\n", -1);
        }
        return;
    }
}

static void
dev_xp_debug_stream_ready(GObject *source, GAsyncResult *result, gpointer user_data)
{
    DebugReadData *data;
    GInputStream *stream;
    gssize count;
    GError *error;
    GString *line_buf;

    data = user_data;
    stream = G_INPUT_STREAM(source);
    error = NULL;
    count = g_input_stream_read_finish(stream, result, &error);
    if (count <= 0)
    {
        g_free(data->buffer);
        g_free(data);
        if (error != NULL)
        {
            g_clear_error(&error);
        }
        return;
    }

    line_buf = g_string_new("");
    {
        gssize i;

        for (i = 0; i < count; i++)
        {
            if (data->buffer[i] == '\n')
            {
                dev_xp_debug_handle_line(data->plugin, line_buf->str);
                g_string_set_size(line_buf, 0);
            }
            else
            {
                g_string_append_c(line_buf, data->buffer[i]);
            }
        }
    }
    if (line_buf->len > 0)
    {
        dev_xp_debug_handle_line(data->plugin, line_buf->str);
    }
    g_string_free(line_buf, TRUE);

    g_input_stream_read_async(stream, data->buffer, 4096,
                              G_PRIORITY_DEFAULT, NULL,
                              dev_xp_debug_stream_ready, data);
}

static void
dev_xp_debug_stop(DevXpPlugin *plugin)
{
    if (plugin->debug_process == NULL)
    {
        return;
    }
    dev_xp_debug_send(plugin, "-gdb-exit\n");
    g_subprocess_force_exit(plugin->debug_process);
    g_clear_object(&plugin->debug_process);
    plugin->debug_state = DEBUG_STATE_STOPPED;
    if (plugin->debug_status_label != NULL)
    {
        gtk_label_set_text(plugin->debug_status_label, _("Stopped"));
    }
}

static void
dev_xp_debug_process_wait(GObject *source, GAsyncResult *result, gpointer user_data)
{
    DevXpPlugin *plugin;
    GError *error;

    plugin = user_data;
    error = NULL;
    g_subprocess_wait_finish(G_SUBPROCESS(source), result, &error);
    if (error != NULL)
    {
        g_clear_error(&error);
    }
    plugin->debug_state = DEBUG_STATE_STOPPED;
    plugin->debug_process = NULL;
    if (plugin->debug_status_label != NULL)
    {
        gtk_label_set_text(plugin->debug_status_label, _("Debug session ended"));
    }
}

static void
dev_xp_debug_start(DevXpPlugin *plugin, const gchar *program_path)
{
    const gchar *debugger;
    gchar **argv;
    GError *error;
    GSubprocessLauncher *launcher;
    DebugReadData *data;

    if (plugin->debug_process != NULL)
    {
        plugin->host->show_toast(plugin->host, _("A debug session is already active"));
        return;
    }

    debugger = plugin->debugger_command != NULL ? plugin->debugger_command : "gdb";
    g_free(plugin->current_program);
    plugin->current_program = g_strdup(program_path);

    argv = g_new0(gchar *, 4);
    argv[0] = g_strdup(debugger);
    argv[1] = g_strdup("--interpreter=mi");
    argv[2] = g_strdup("--nx");
    argv[3] = NULL;
    error = NULL;

    launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDIN_PIPE |
                                         G_SUBPROCESS_FLAGS_STDOUT_PIPE);
    plugin->debug_process = g_subprocess_launcher_spawnv(launcher,
                                                          (const gchar * const *)argv,
                                                          &error);
    g_object_unref(launcher);
    g_strfreev(argv);

    if (plugin->debug_process == NULL)
    {
        gchar *message;

        message = g_strdup_printf(_("Unable to start debugger: %s"), error->message);
        dev_xp_append_error(plugin, message);
        plugin->host->show_toast(plugin->host, message);
        g_free(message);
        g_clear_error(&error);
        return;
    }

    g_clear_error(&error);
    plugin->debug_state = DEBUG_STATE_STOPPED;
    plugin->debug_output = g_string_new("");

    plugin->debug_stdin = g_subprocess_get_stdin_pipe(plugin->debug_process);
    plugin->debug_stdout = g_subprocess_get_stdout_pipe(plugin->debug_process);

    {
        gchar *cmd;

        cmd = g_strdup_printf("-file-exec-file \"%s\"\n", program_path);
        dev_xp_debug_send(plugin, cmd);
        g_free(cmd);
    }

    if (plugin->debug_stdout != NULL)
    {
        data = g_new0(DebugReadData, 1);
        data->plugin = plugin;
        data->buffer = g_malloc0(4096);
        g_input_stream_read_async(plugin->debug_stdout, data->buffer, 4096,
                                  G_PRIORITY_DEFAULT, NULL,
                                  dev_xp_debug_stream_ready, data);
    }

    if (plugin->debug_status_label != NULL)
    {
        gtk_label_set_text(plugin->debug_status_label, _("Ready"));
    }
    dev_xp_append_info(plugin, _("Debugger started"));
}

/* —— UI —— */

static void
dev_xp_debug_run_clicked(GtkButton *button, DevXpPlugin *plugin)
{
    (void)button;
    if (plugin->debug_process == NULL)
    {
        plugin->host->show_toast(plugin->host, _("Start a debug session first (Build + Debug)"));
        return;
    }
    dev_xp_debug_continue(plugin);
}

static void
dev_xp_debug_step_over_clicked(GtkButton *button, DevXpPlugin *plugin)
{
    (void)button;
    dev_xp_debug_step_over(plugin);
}

static void
dev_xp_debug_step_into_clicked(GtkButton *button, DevXpPlugin *plugin)
{
    (void)button;
    dev_xp_debug_step_into(plugin);
}

static void
dev_xp_debug_step_out_clicked(GtkButton *button, DevXpPlugin *plugin)
{
    (void)button;
    dev_xp_debug_step_out(plugin);
}

static void
dev_xp_debug_stop_clicked(GtkButton *button, DevXpPlugin *plugin)
{
    (void)button;
    dev_xp_debug_stop(plugin);
}

static void
dev_xp_build_clicked(GtkButton *button, DevXpPlugin *plugin)
{
    (void)button;
    dev_xp_start(plugin, DEV_XP_MODE_BUILD, FALSE);
}

static void
dev_xp_run_clicked(GtkButton *button, DevXpPlugin *plugin)
{
    (void)button;
    dev_xp_start(plugin, DEV_XP_MODE_RUN, FALSE);
}

static void
dev_xp_build_and_run_clicked(GtkButton *button, DevXpPlugin *plugin)
{
    (void)button;
    dev_xp_start(plugin, DEV_XP_MODE_BUILD, TRUE);
}

static void
dev_xp_build_and_debug_clicked(GtkButton *button, DevXpPlugin *plugin)
{
    DevXpPlugin *p;
    gchar *file_path;
    gchar *file_directory;
    gchar *working_directory;
    gchar *source_name;
    gchar *expanded_command;
    gchar **argv;
    gint argc;
    GError *error;
    GSubprocessLauncher *launcher;
    GSubprocess *build_process;
    gchar *program_path;

    (void)button;
    p = plugin;

    file_path = p->host->get_current_file_path(p->host);
    if (file_path == NULL)
    {
        p->host->show_toast(p->host, _("Save the current source file before debugging"));
        return;
    }

    file_directory = g_path_get_dirname(file_path);
    working_directory = dev_xp_get_working_directory(p, file_path);
    source_name = dev_xp_source_name(file_path);
    expanded_command = dev_xp_expand_command(p, p->build_command,
                                                file_path,
                                                file_directory,
                                                working_directory,
                                                source_name);
    g_free(source_name);
    argv = NULL;
    argc = 0;
    error = NULL;
    if (!g_shell_parse_argv(expanded_command, &argc, &argv, &error) || argc == 0)
    {
        gchar *message;

        message = g_strdup_printf(_("Unable to parse command: %s"), error->message);
        dev_xp_append_error(p, message);
        dev_xp_append_output(p, "\n", NULL);
        p->host->show_toast(p->host, message);
        g_free(message);
        g_clear_error(&error);
        g_strfreev(argv);
        g_free(expanded_command);
        g_free(working_directory);
        g_free(file_directory);
        g_free(file_path);
        return;
    }

    dev_xp_append_output(p, "\n$ ", NULL);
    dev_xp_append_command(p, expanded_command);
    dev_xp_append_output(p, "\n", NULL);
    dev_xp_append_info(p, _("Building for debug…\n"));

    launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                         G_SUBPROCESS_FLAGS_STDERR_PIPE);
    g_subprocess_launcher_set_cwd(launcher, working_directory);
    build_process = g_subprocess_launcher_spawnv(launcher,
                                                  (const gchar * const *)argv,
                                                  &error);
    g_object_unref(launcher);
    g_strfreev(argv);
    g_free(expanded_command);

    if (build_process == NULL)
    {
        gchar *message;

        message = g_strdup_printf(_("Unable to start command: %s"), error->message);
        dev_xp_append_error(p, message);
        dev_xp_append_output(p, "\n", NULL);
        p->host->show_toast(p->host, message);
        g_free(message);
        g_clear_error(&error);
        g_free(working_directory);
        g_free(file_directory);
        g_free(file_path);
        return;
    }

    {
        GInputStream *out;
        GInputStream *err;
        GString *out_str;
        GError *wait_error;
        gint exit_status;

        out_str = g_string_new("");
        out = g_subprocess_get_stdout_pipe(build_process);
        err = g_subprocess_get_stderr_pipe(build_process);
        wait_error = NULL;
        exit_status = 0;

        if (out != NULL)
        {
            gssize n;
            gchar buf[4096];

            while ((n = g_input_stream_read(out, buf, sizeof(buf), NULL, &wait_error)) > 0)
            {
                buf[n] = '\0';
                dev_xp_append_output(p, buf, NULL);
                g_string_append_len(out_str, buf, (gsize)n);
            }
        }
        if (err != NULL)
        {
            gssize n;
            gchar buf[4096];

            while ((n = g_input_stream_read(err, buf, sizeof(buf), NULL, &wait_error)) > 0)
            {
                buf[n] = '\0';
                dev_xp_append_output(p, buf, NULL);
                g_string_append_len(out_str, buf, (gsize)n);
            }
        }

        if (wait_error != NULL)
        {
            g_clear_error(&wait_error);
        }

        if (!g_subprocess_wait(build_process, NULL, &wait_error))
        {
            g_clear_error(&wait_error);
        }

        exit_status = g_subprocess_get_exit_status(build_process);

        if (out_str->len > 0)
        {
            dev_xp_parse_errors(p, out_str->str);
        }

        g_object_unref(build_process);
        g_string_free(out_str, TRUE);

        if (exit_status != 0)
        {
            gchar *message;

            message = g_strdup_printf(_("Build failed with status %d"), exit_status);
            dev_xp_append_error(p, message);
            dev_xp_append_output(p, "\n", NULL);
            p->host->show_toast(p->host, message);
            g_free(message);
            g_free(working_directory);
            g_free(file_directory);
            g_free(file_path);
            return;
        }

        program_path = dev_xp_expand_command(p, p->run_command,
                                                file_path,
                                                file_directory,
                                                working_directory,
                                                source_name);
        if (program_path != NULL && program_path[0] == '"' &&
            strlen(program_path) > 1 && program_path[strlen(program_path) - 1] == '"')
        {
            memmove(program_path, program_path + 1, strlen(program_path));
            program_path[strlen(program_path) - 1] = '\0';
        }

        dev_xp_debug_start(p, program_path);
        g_free(program_path);
        g_free(working_directory);
        g_free(file_directory);
        g_free(file_path);
    }
}

static void
dev_xp_stop_clicked(GtkButton *button, DevXpPlugin *plugin)
{
    (void)button;
    if (plugin->process != NULL)
    {
        g_subprocess_force_exit(plugin->process);
        dev_xp_append_output(plugin, _("Command stop requested\n"), NULL);
    }
    if (plugin->debug_process != NULL)
    {
        dev_xp_debug_stop(plugin);
    }
}

static void
dev_xp_clear_clicked(GtkButton *button, DevXpPlugin *plugin)
{
    (void)button;
    dev_xp_clear_output(plugin);
}

static void
dev_xp_window_destroyed(GtkWidget *widget, DevXpPlugin *plugin)
{
    (void)widget;
    plugin->panel = NULL;
    plugin->output_buffer = NULL;
    plugin->breakpoint_list_box = NULL;
    plugin->variables_view = NULL;
    plugin->debug_status_label = NULL;
    plugin->debug_run_button = NULL;
    plugin->debug_step_button = NULL;
    plugin->debug_step_over_button = NULL;
    plugin->debug_step_out_button = NULL;
    plugin->debug_stop_button = NULL;
    plugin->breakpoint_button = NULL;
    plugin->breakpoint_entry = NULL;
}

static void
dev_xp_close_clicked(GtkButton *button, DevXpPlugin *plugin)
{
    (void)button;
    if (plugin->panel != NULL)
    {
        gtk_window_destroy(GTK_WINDOW(plugin->panel));
    }
}

static void
dev_xp_show(DevXpPlugin *plugin)
{
    GtkBox *content;
    GtkBox *toolbar_box;
    GtkBox *main_box;
    GtkPaned *paned;
    GtkBox *left_panel;
    GtkBox *right_panel;
    GtkBox *breakpoint_box;
    GtkBox *debug_box;
    GtkBox *variables_box;
    GtkBox *bp_header;
    GtkBox *bp_list_box;
    GtkScrolledWindow *scroll;
    GtkScrolledWindow *bp_scroll;
    GtkScrolledWindow *vars_scroll;
    GtkTextView *output;
    GtkTextView *variables_view;
    GtkLabel *title;
    GtkLabel *bp_title;
    GtkLabel *bp_hint;
    GtkLabel *dbg_title;
    GtkLabel *debug_status_label;
    GtkButton *build_button;
    GtkButton *run_button;
    GtkButton *both_button;
    GtkButton *debug_button;
    GtkButton *stop_button;
    GtkButton *clear_button;
    GtkButton *close_button;
    GtkButton *bp_add_button;
    GtkButton *debug_run_button;
    GtkButton *debug_step_button;
    GtkButton *debug_step_over_button;
    GtkButton *debug_step_out_button;
    GtkButton *debug_stop_button;
    GtkEntry *bp_entry;

    if (plugin->panel != NULL)
    {
        gtk_window_present(GTK_WINDOW(plugin->panel));
        return;
    }

    plugin->panel = adw_window_new();
    {
        gchar *file_path;
        gchar *base;
        gchar *window_title;

        file_path = plugin->host->get_current_file_path(plugin->host);
        if (file_path != NULL)
        {
            base = g_path_get_basename(file_path);
            window_title = g_strdup_printf(_("Dev Experience — %s"), base);
            gtk_window_set_title(GTK_WINDOW(plugin->panel), window_title);
            g_free(window_title);
            g_free(base);
            g_free(file_path);
        }
        else
        {
            gtk_window_set_title(GTK_WINDOW(plugin->panel), _("Dev Experience"));
        }
    }
    gtk_window_set_default_size(GTK_WINDOW(plugin->panel), 900, 560);
    gtk_widget_set_size_request(plugin->panel, 700, 400);

    content = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));

    toolbar_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6));
    title = GTK_LABEL(gtk_label_new(_("Dev Experience")));
    gtk_widget_add_css_class(GTK_WIDGET(title), "heading");
    gtk_widget_set_hexpand(GTK_WIDGET(title), TRUE);

    build_button = GTK_BUTTON(gtk_button_new_with_label(_("Build")));
    run_button = GTK_BUTTON(gtk_button_new_with_label(_("Run")));
    both_button = GTK_BUTTON(gtk_button_new_with_label(_("Build + Run")));
    debug_button = GTK_BUTTON(gtk_button_new_with_label(_("Build + Debug")));
    stop_button = GTK_BUTTON(gtk_button_new_from_icon_name("media-playback-stop-symbolic"));
    clear_button = GTK_BUTTON(gtk_button_new_from_icon_name("edit-clear-symbolic"));
    close_button = GTK_BUTTON(gtk_button_new_from_icon_name("sidebar-hide-symbolic"));

    gtk_widget_set_tooltip_text(GTK_WIDGET(stop_button), _("Stop Active Command"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(clear_button), _("Clear Output"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(close_button), _("Hide Dev Experience Output"));

    gtk_box_append(toolbar_box, GTK_WIDGET(title));
    gtk_box_append(toolbar_box, GTK_WIDGET(build_button));
    gtk_box_append(toolbar_box, GTK_WIDGET(run_button));
    gtk_box_append(toolbar_box, GTK_WIDGET(both_button));
    gtk_box_append(toolbar_box, GTK_WIDGET(debug_button));
    gtk_box_append(toolbar_box, GTK_WIDGET(stop_button));
    gtk_box_append(toolbar_box, GTK_WIDGET(clear_button));
    gtk_box_append(toolbar_box, GTK_WIDGET(close_button));

    main_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
    paned = GTK_PANED(gtk_paned_new(GTK_ORIENTATION_HORIZONTAL));
    gtk_paned_set_position(paned, 560);

    left_panel = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
    scroll = GTK_SCROLLED_WINDOW(gtk_scrolled_window_new());
    output = GTK_TEXT_VIEW(gtk_text_view_new());
    plugin->output_buffer = gtk_text_view_get_buffer(output);
    gtk_text_view_set_editable(output, FALSE);
    gtk_text_view_set_monospace(output, TRUE);
    gtk_text_view_set_wrap_mode(output, GTK_WRAP_WORD_CHAR);
    gtk_widget_set_vexpand(GTK_WIDGET(output), TRUE);
    gtk_scrolled_window_set_policy(scroll, GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(scroll, GTK_WIDGET(output));
    gtk_box_append(left_panel, GTK_WIDGET(scroll));

    right_panel = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 6));
    gtk_widget_set_vexpand(GTK_WIDGET(right_panel), TRUE);

    breakpoint_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 6));
    bp_header = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4));
    bp_title = GTK_LABEL(gtk_label_new(_("Breakpoints")));
    gtk_widget_add_css_class(GTK_WIDGET(bp_title), "heading");
    bp_hint = GTK_LABEL(gtk_label_new(_("Enter a line number and press Enter, or click in the gutter.")));
    bp_entry = GTK_ENTRY(gtk_entry_new());
    bp_add_button = GTK_BUTTON(gtk_button_new_from_icon_name("list-add-symbolic"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(bp_add_button), _("Add breakpoint at line"));
    gtk_box_append(bp_header, GTK_WIDGET(bp_title));
    gtk_box_append(bp_header, GTK_WIDGET(bp_entry));
    gtk_box_append(bp_header, GTK_WIDGET(bp_add_button));

    bp_list_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 2));
    bp_scroll = GTK_SCROLLED_WINDOW(gtk_scrolled_window_new());
    gtk_scrolled_window_set_policy(bp_scroll, GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(bp_scroll, GTK_WIDGET(bp_list_box));
    gtk_widget_set_vexpand(GTK_WIDGET(bp_scroll), TRUE);

    gtk_box_append(breakpoint_box, GTK_WIDGET(bp_header));
    gtk_box_append(breakpoint_box, GTK_WIDGET(bp_scroll));
    gtk_box_append(breakpoint_box, GTK_WIDGET(bp_hint));

    debug_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 6));
    dbg_title = GTK_LABEL(gtk_label_new(_("Debug")));
    gtk_widget_add_css_class(GTK_WIDGET(dbg_title), "heading");
    debug_status_label = GTK_LABEL(gtk_label_new(_("Stopped")));
    gtk_widget_set_halign(GTK_WIDGET(debug_status_label), GTK_ALIGN_START);

    {
        GtkBox *debug_controls;

        debug_controls = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4));
        debug_run_button = GTK_BUTTON(gtk_button_new_with_label(_("Continue")));
        debug_step_button = GTK_BUTTON(gtk_button_new_with_label(_("Step Into")));
        debug_step_over_button = GTK_BUTTON(gtk_button_new_with_label(_("Step Over")));
        debug_step_out_button = GTK_BUTTON(gtk_button_new_with_label(_("Step Out")));
        debug_stop_button = GTK_BUTTON(gtk_button_new_from_icon_name("media-playback-stop-symbolic"));
        gtk_box_append(debug_controls, GTK_WIDGET(debug_run_button));
        gtk_box_append(debug_controls, GTK_WIDGET(debug_step_button));
        gtk_box_append(debug_controls, GTK_WIDGET(debug_step_over_button));
        gtk_box_append(debug_controls, GTK_WIDGET(debug_step_out_button));
        gtk_box_append(debug_controls, GTK_WIDGET(debug_stop_button));
        gtk_box_append(debug_box, GTK_WIDGET(dbg_title));
        gtk_box_append(debug_box, GTK_WIDGET(debug_status_label));
        gtk_box_append(debug_box, GTK_WIDGET(debug_controls));
    }

    variables_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 4));
    {
        GtkLabel *vars_title;

        vars_title = GTK_LABEL(gtk_label_new(_("Locals")));
        gtk_widget_add_css_class(GTK_WIDGET(vars_title), "heading");
        vars_scroll = GTK_SCROLLED_WINDOW(gtk_scrolled_window_new());
        variables_view = GTK_TEXT_VIEW(gtk_text_view_new());
        gtk_text_view_set_editable(variables_view, FALSE);
        gtk_text_view_set_monospace(variables_view, TRUE);
        gtk_text_view_set_wrap_mode(variables_view, GTK_WRAP_WORD_CHAR);
        gtk_scrolled_window_set_child(vars_scroll, GTK_WIDGET(variables_view));
        gtk_widget_set_vexpand(GTK_WIDGET(vars_scroll), TRUE);
        gtk_box_append(variables_box, GTK_WIDGET(vars_title));
        gtk_box_append(variables_box, GTK_WIDGET(vars_scroll));
    }

    gtk_box_append(right_panel, GTK_WIDGET(breakpoint_box));
    gtk_box_append(right_panel, GTK_WIDGET(debug_box));
    gtk_box_append(right_panel, GTK_WIDGET(variables_box));

    gtk_paned_set_start_child(paned, GTK_WIDGET(left_panel));
    gtk_paned_set_end_child(paned, GTK_WIDGET(right_panel));
    gtk_box_append(main_box, GTK_WIDGET(paned));

    gtk_box_append(content, GTK_WIDGET(toolbar_box));
    gtk_box_append(content, GTK_WIDGET(main_box));

    g_signal_connect(build_button, "clicked", G_CALLBACK(dev_xp_build_clicked), plugin);
    g_signal_connect(run_button, "clicked", G_CALLBACK(dev_xp_run_clicked), plugin);
    g_signal_connect(both_button, "clicked", G_CALLBACK(dev_xp_build_and_run_clicked), plugin);
    g_signal_connect(debug_button, "clicked", G_CALLBACK(dev_xp_build_and_debug_clicked), plugin);
    g_signal_connect(stop_button, "clicked", G_CALLBACK(dev_xp_stop_clicked), plugin);
    g_signal_connect(clear_button, "clicked", G_CALLBACK(dev_xp_clear_clicked), plugin);
    g_signal_connect(close_button, "clicked", G_CALLBACK(dev_xp_close_clicked), plugin);
    g_signal_connect(debug_run_button, "clicked", G_CALLBACK(dev_xp_debug_run_clicked), plugin);
    g_signal_connect(debug_step_button, "clicked", G_CALLBACK(dev_xp_debug_step_into_clicked), plugin);
    g_signal_connect(debug_step_over_button, "clicked", G_CALLBACK(dev_xp_debug_step_over_clicked), plugin);
    g_signal_connect(debug_step_out_button, "clicked", G_CALLBACK(dev_xp_debug_step_out_clicked), plugin);
    g_signal_connect(debug_stop_button, "clicked", G_CALLBACK(dev_xp_debug_stop_clicked), plugin);
    g_signal_connect(bp_entry, "activate", G_CALLBACK(dev_xp_breakpoint_entry_activated), plugin);
    g_signal_connect(bp_add_button, "clicked", G_CALLBACK(dev_xp_breakpoint_add_clicked), plugin);
    g_signal_connect(plugin->panel, "destroy", G_CALLBACK(dev_xp_window_destroyed), plugin);

    plugin->breakpoint_list_box = GTK_WIDGET(bp_list_box);
    plugin->variables_view = GTK_WIDGET(variables_view);
    plugin->debug_status_label = debug_status_label;
    plugin->debug_run_button = debug_run_button;
    plugin->debug_step_button = debug_step_button;
    plugin->debug_step_over_button = debug_step_over_button;
    plugin->debug_step_out_button = debug_step_out_button;
    plugin->debug_stop_button = debug_stop_button;
    plugin->breakpoint_entry = bp_entry;

    dev_xp_setup_output_tags(plugin);
    adw_window_set_content(ADW_WINDOW(plugin->panel), GTK_WIDGET(content));
    gtk_window_present(GTK_WINDOW(plugin->panel));

    dev_xp_append_output(plugin,
                            _("Commands use direct argument parsing, not a shell. Placeholders: ${file}, ${dir}, ${name}, ${root}, ${profile}.\n"), NULL);
    dev_xp_append_output(plugin,
                            _("Breakpoints: click in the gutter or enter a line number. Debug with Build + Debug.\n"), NULL);

    dev_xp_refresh_breakpoint_ui(plugin);
}

/* —— 动作 —— */

static void
dev_xp_action_build(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action;
    (void)parameter;
    dev_xp_show(user_data);
    dev_xp_start(user_data, DEV_XP_MODE_BUILD, FALSE);
}

static void
dev_xp_action_run(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action;
    (void)parameter;
    dev_xp_show(user_data);
    dev_xp_start(user_data, DEV_XP_MODE_RUN, FALSE);
}

static void
dev_xp_action_build_and_run(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action;
    (void)parameter;
    dev_xp_show(user_data);
    dev_xp_start(user_data, DEV_XP_MODE_BUILD, TRUE);
}

static void
dev_xp_action_build_and_debug(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action;
    (void)parameter;
    dev_xp_show(user_data);
    dev_xp_build_and_debug_clicked(NULL, user_data);
}

static void
dev_xp_save_clicked(GtkButton *button, DevXpConfigWidgets *widgets)
{
    DevXpPlugin *plugin;
    GError *error;

    (void)button;
    plugin = widgets->plugin;
    g_free(plugin->working_directory);
    g_free(plugin->build_command);
    g_free(plugin->run_command);
    g_free(plugin->build_profile);
    g_free(plugin->debugger_command);
    plugin->working_directory = g_strdup(gtk_editable_get_text(GTK_EDITABLE(widgets->directory_row)));
    plugin->build_command = g_strdup(gtk_editable_get_text(GTK_EDITABLE(widgets->build_row)));
    plugin->run_command = g_strdup(gtk_editable_get_text(GTK_EDITABLE(widgets->run_row)));
    plugin->build_profile = g_strdup(gtk_editable_get_text(GTK_EDITABLE(widgets->profile_row)));
    plugin->debugger_command = g_strdup(gtk_editable_get_text(GTK_EDITABLE(widgets->debugger_row)));
    error = NULL;

    if (dev_xp_save_settings(plugin, &error))
    {
        plugin->host->show_toast(plugin->host, _("Build and run settings saved"));
        gtk_window_destroy(widgets->window);
    }
    else
    {
        gchar *message;

        message = g_strdup_printf(_("Unable to save build settings: %s"), error->message);
        plugin->host->show_toast(plugin->host, message);
        g_free(message);
        g_clear_error(&error);
    }
}

G_MODULE_EXPORT const MtPluginInfo *
mt_plugin_query(void)
{
    return &dev_xp_plugin_info;
}

G_MODULE_EXPORT gboolean
mt_plugin_activate(MtPluginHost *host, GError **error)
{
    static const gchar *build_accelerators[] = { "F9", NULL };
    static const gchar *run_accelerators[] = { "F10", NULL };
    static const gchar *both_accelerators[] = { "F11", NULL };
    static const gchar *debug_accelerators[] = { "<Primary><Shift>D", NULL };

    if (host->get_current_file_path == NULL || host->get_parent_window == NULL)
    {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_NOT_SUPPORTED,
                    "Vellum host does not provide the build and run services");
        return FALSE;
    }

    dev_xp_plugin = g_new0(DevXpPlugin, 1);
    dev_xp_plugin->host = host;
    dev_xp_plugin->errors = g_ptr_array_new_with_free_func((GDestroyNotify)g_free);
    dev_xp_plugin->breakpoints = g_ptr_array_new_with_free_func((GDestroyNotify)g_free);
    dev_xp_load_settings(dev_xp_plugin);

    if (!host->add_action(host, "build-current", dev_xp_action_build, dev_xp_plugin, NULL) ||
        !host->add_action(host, "run-current", dev_xp_action_run, dev_xp_plugin, NULL) ||
        !host->add_action(host, "build-and-run-current", dev_xp_action_build_and_run, dev_xp_plugin, NULL) ||
        !host->add_action(host, "build-and-debug-current", dev_xp_action_build_and_debug, dev_xp_plugin, NULL))
    {
        g_free(dev_xp_plugin->working_directory);
        g_free(dev_xp_plugin->build_command);
        g_free(dev_xp_plugin->run_command);
        g_free(dev_xp_plugin->build_profile);
        g_free(dev_xp_plugin->debugger_command);
        g_ptr_array_unref(dev_xp_plugin->errors);
        g_ptr_array_unref(dev_xp_plugin->breakpoints);
        g_free(dev_xp_plugin);
        dev_xp_plugin = NULL;
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_EXISTS,
                    "A build and run action is already registered");
        return FALSE;
    }

    {
        GtkWindow *window;
        GAction *action;

        window = host->get_parent_window(host);
        action = g_action_map_lookup_action(G_ACTION_MAP(window),
                                           "win.toggle-breakpoint");
        if (action != NULL)
        {
            g_signal_connect_data(action,
                                  "activate",
                                  G_CALLBACK(dev_xp_toggle_breakpoint_action),
                                  dev_xp_plugin,
                                  NULL,
                                  G_CONNECT_DEFAULT);
        }
    }

    host->set_accelerators(host, "app.build-current", build_accelerators);
    host->set_accelerators(host, "app.run-current", run_accelerators);
    host->set_accelerators(host, "app.build-and-run-current", both_accelerators);
    host->set_accelerators(host, "app.build-and-debug-current", debug_accelerators);

    return TRUE;
}

G_MODULE_EXPORT void
mt_plugin_deactivate(MtPluginHost *host)
{
    (void)host;
    if (dev_xp_plugin == NULL)
    {
        return;
    }

    if (dev_xp_plugin->process != NULL)
    {
        g_subprocess_force_exit(dev_xp_plugin->process);
    }
    if (dev_xp_plugin->debug_process != NULL)
    {
        dev_xp_debug_stop(dev_xp_plugin);
    }
    if (dev_xp_plugin->panel != NULL)
    {
        gtk_window_destroy(GTK_WINDOW(dev_xp_plugin->panel));
    }

    g_clear_object(&dev_xp_plugin->process);
    g_clear_object(&dev_xp_plugin->debug_process);
    g_clear_pointer(&dev_xp_plugin->debug_output, (GDestroyNotify)g_string_free);
    g_free(dev_xp_plugin->working_directory);
    g_free(dev_xp_plugin->build_command);
    g_free(dev_xp_plugin->run_command);
    g_free(dev_xp_plugin->build_profile);
    g_free(dev_xp_plugin->debugger_command);
    g_free(dev_xp_plugin->current_program);
    g_clear_pointer(&dev_xp_plugin->errors, g_ptr_array_unref);
    g_clear_pointer(&dev_xp_plugin->breakpoints, g_ptr_array_unref);
    g_free(dev_xp_plugin);
    dev_xp_plugin = NULL;
}

G_MODULE_EXPORT void
mt_plugin_configure(MtPluginHost *host, gpointer parent_window)
{
    AdwPreferencesWindow *window;
    AdwPreferencesPage *page;
    AdwPreferencesGroup *group;
    AdwActionRow *save_row;
    GtkButton *save_button;
    DevXpConfigWidgets *widgets;
    gchar *save_title;

    (void)host;
    if (dev_xp_plugin == NULL)
    {
        return;
    }

    window = ADW_PREFERENCES_WINDOW(adw_preferences_window_new());
    gtk_window_set_title(GTK_WINDOW(window), _("Dev Experience Settings"));
    gtk_window_set_transient_for(GTK_WINDOW(window), GTK_WINDOW(parent_window));
    gtk_window_set_modal(GTK_WINDOW(window), TRUE);
    page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(group, _("Explicit Commands"));
    adw_preferences_group_set_description(group,
                                          _("Commands run only after F9, F10 or F11. They are parsed into arguments without a shell. Use ${file}, ${dir}, ${name}, or ${root}; ${name} is the source file name without its extension. Quote placeholders when paths may contain spaces."));

    widgets = g_new0(DevXpConfigWidgets, 1);
    widgets->plugin = dev_xp_plugin;
    widgets->window = GTK_WINDOW(window);
    widgets->directory_row = ADW_ENTRY_ROW(adw_entry_row_new());
    widgets->build_row = ADW_ENTRY_ROW(adw_entry_row_new());
    widgets->run_row = ADW_ENTRY_ROW(adw_entry_row_new());
    widgets->profile_row = ADW_ENTRY_ROW(adw_entry_row_new());
    widgets->debugger_row = ADW_ENTRY_ROW(adw_entry_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(widgets->directory_row), _("Working Directory (optional)"));
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(widgets->build_row), _("Build Command"));
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(widgets->run_row), _("Run Command"));
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(widgets->profile_row), _("Build Profile (debug/release)"));
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(widgets->debugger_row), _("Debugger (e.g. gdb)"));
    gtk_editable_set_text(GTK_EDITABLE(widgets->directory_row), dev_xp_plugin->working_directory);
    gtk_editable_set_text(GTK_EDITABLE(widgets->build_row), dev_xp_plugin->build_command);
    gtk_editable_set_text(GTK_EDITABLE(widgets->run_row), dev_xp_plugin->run_command);
    gtk_editable_set_text(GTK_EDITABLE(widgets->profile_row), dev_xp_plugin->build_profile);
    gtk_editable_set_text(GTK_EDITABLE(widgets->debugger_row), dev_xp_plugin->debugger_command);
    adw_preferences_group_add(group, GTK_WIDGET(widgets->directory_row));
    adw_preferences_group_add(group, GTK_WIDGET(widgets->build_row));
    adw_preferences_group_add(group, GTK_WIDGET(widgets->run_row));
    adw_preferences_group_add(group, GTK_WIDGET(widgets->profile_row));
    adw_preferences_group_add(group, GTK_WIDGET(widgets->debugger_row));

    save_row = ADW_ACTION_ROW(adw_action_row_new());
    save_title = g_markup_escape_text(_("Save Dev Experience Settings"), -1);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(save_row), save_title);
    g_free(save_title);
    save_button = GTK_BUTTON(gtk_button_new_with_label(_("Save")));
    gtk_widget_set_valign(GTK_WIDGET(save_button), GTK_ALIGN_CENTER);
    adw_action_row_add_suffix(save_row, GTK_WIDGET(save_button));
    adw_preferences_group_add(group, GTK_WIDGET(save_row));
    g_signal_connect(save_button, "clicked", G_CALLBACK(dev_xp_save_clicked), widgets);
    g_object_set_data_full(G_OBJECT(window),
                           "vellum-dev-xp-config-widgets",
                           widgets,
                           (GDestroyNotify)g_free);

    adw_preferences_page_add(page, group);
    adw_preferences_window_add(window, page);
    gtk_window_present(GTK_WINDOW(window));
}