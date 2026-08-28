/*
 * build-run-plugin.c
 * 用户明确配置的构建与运行命令。命令经 GShell 解析后直接启动，不调用 shell。
 */

#include "mt-plugin.h"

#include <adwaita.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <gmodule.h>
#include <string.h>

#define BUILD_RUN_GROUP "Build Run"

/* 旧默认把可执行文件固定命名为 vellum-run；新默认与源文件同名（foo.c -> foo）。 */
#define BUILD_RUN_OLD_BUILD_COMMAND "cc -Wall -Wextra \"${file}\" -o \"${dir}/vellum-run\""
#define BUILD_RUN_OLD_RUN_COMMAND "\"${dir}/vellum-run\""
#define BUILD_RUN_DEFAULT_BUILD_COMMAND "cc -Wall -Wextra \"${file}\" -o \"${dir}/${name}\""
#define BUILD_RUN_DEFAULT_RUN_COMMAND "\"${dir}/${name}\""

typedef struct _BuildRunPlugin BuildRunPlugin;
typedef struct _BuildRunRequest BuildRunRequest;
typedef struct _BuildRunConfigWidgets BuildRunConfigWidgets;

typedef enum
{
    BUILD_RUN_MODE_BUILD,
    BUILD_RUN_MODE_RUN
} BuildRunMode;

struct _BuildRunPlugin
{
    MtPluginHost *host;
    GtkWidget *panel;
    GtkTextBuffer *output_buffer;
    GSubprocess *process;
    gchar *working_directory;
    gchar *build_command;
    gchar *run_command;
};

struct _BuildRunRequest
{
    BuildRunPlugin *plugin;
    BuildRunMode mode;
    gboolean run_after_success;
    GInputStream *stdout_stream;
    GInputStream *stderr_stream;
    gchar stdout_buffer[4096];
    gchar stderr_buffer[4096];
    gboolean stdout_done;
    gboolean stderr_done;
};

struct _BuildRunConfigWidgets
{
    BuildRunPlugin *plugin;
    AdwEntryRow *directory_row;
    AdwEntryRow *build_row;
    AdwEntryRow *run_row;
    GtkWindow *window;
};

static const MtPluginInfo build_run_plugin_info = {
    MT_PLUGIN_API_VERSION,
    "io.github.vellum.build-run",
    "Build & Run",
    "Run user-configured build and run commands for the current source file",
    "0.1.0"
};

static BuildRunPlugin *build_run_plugin;

static gchar *
build_run_config_path(void)
{
    gchar *directory;
    gchar *path;

    directory = g_build_filename(g_get_user_config_dir(), "vellum", NULL);
    g_mkdir_with_parents(directory, 0700);
    path = g_build_filename(directory, "build-run.ini", NULL);
    g_free(directory);

    return path;
}

static gchar *
build_run_get_value(GKeyFile *settings, const gchar *key, const gchar *fallback)
{
    gchar *value;

    value = g_key_file_get_string(settings, BUILD_RUN_GROUP, key, NULL);
    if (value == NULL)
    {
        value = g_strdup(fallback);
    }

    return value;
}

static void
build_run_load_settings(BuildRunPlugin *plugin)
{
    GKeyFile *settings;
    gchar *path;

    settings = g_key_file_new();
    path = build_run_config_path();
    g_key_file_load_from_file(settings, path, G_KEY_FILE_NONE, NULL);

    plugin->working_directory = build_run_get_value(settings, "working-directory", "");
    plugin->build_command = build_run_get_value(settings, "build-command",
                                                BUILD_RUN_DEFAULT_BUILD_COMMAND);
    if (g_strcmp0(plugin->build_command, BUILD_RUN_OLD_BUILD_COMMAND) == 0)
    {
        g_free(plugin->build_command);
        plugin->build_command = g_strdup(BUILD_RUN_DEFAULT_BUILD_COMMAND);
    }
    plugin->run_command = build_run_get_value(settings, "run-command",
                                              BUILD_RUN_DEFAULT_RUN_COMMAND);
    if (g_strcmp0(plugin->run_command, BUILD_RUN_OLD_RUN_COMMAND) == 0)
    {
        g_free(plugin->run_command);
        plugin->run_command = g_strdup(BUILD_RUN_DEFAULT_RUN_COMMAND);
    }
    g_free(path);
    g_key_file_unref(settings);
}

static gboolean
build_run_save_settings(BuildRunPlugin *plugin, GError **error)
{
    GKeyFile *settings;
    gchar *path;
    gchar *contents;
    gsize length;
    gboolean saved;

    settings = g_key_file_new();
    g_key_file_set_string(settings, BUILD_RUN_GROUP, "working-directory", plugin->working_directory);
    g_key_file_set_string(settings, BUILD_RUN_GROUP, "build-command", plugin->build_command);
    g_key_file_set_string(settings, BUILD_RUN_GROUP, "run-command", plugin->run_command);
    contents = g_key_file_to_data(settings, &length, error);
    if (contents == NULL)
    {
        g_key_file_unref(settings);
        return FALSE;
    }

    path = build_run_config_path();
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

static void
build_run_append_output(BuildRunPlugin *plugin, const gchar *text)
{
    GtkTextIter end;

    if (plugin->output_buffer == NULL || text == NULL)
    {
        return;
    }

    gtk_text_buffer_get_end_iter(plugin->output_buffer, &end);
    gtk_text_buffer_insert(plugin->output_buffer, &end, text, -1);
}

static void
build_run_clear_output(BuildRunPlugin *plugin)
{
    if (plugin->output_buffer != NULL)
    {
        gtk_text_buffer_set_text(plugin->output_buffer, "", -1);
    }
}

/* 源文件去掉最后一个扩展名的名字：/a/b/foo.c -> foo。 */
static gchar *
build_run_source_name(const gchar *file_path)
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
build_run_expand_command(const gchar *command,
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
        else
        {
            g_string_append_c(result, *cursor);
            cursor++;
        }
    }

    return g_string_free(result, FALSE);
}

static gchar *
build_run_get_working_directory(BuildRunPlugin *plugin, const gchar *file_path)
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

static void build_run_start(BuildRunPlugin *plugin,
                            BuildRunMode mode,
                            gboolean run_after_success);
static void build_run_process_wait(GObject *source,
                                   GAsyncResult *result,
                                   gpointer user_data);

static void
build_run_finish(BuildRunRequest *request)
{
    BuildRunPlugin *plugin;
    gboolean run_after_success;

    plugin = request->plugin;
    run_after_success = request->run_after_success;
    if (g_subprocess_get_successful(plugin->process))
    {
        plugin->host->show_toast(plugin->host,
                                 request->mode == BUILD_RUN_MODE_BUILD ?
                                 _("Build completed") : _("Program exited successfully"));
        g_clear_object(&plugin->process);
        g_free(request);
        if (run_after_success)
        {
            build_run_start(plugin, BUILD_RUN_MODE_RUN, FALSE);
        }
        return;
    }

    {
        gchar *message;

        message = g_strdup_printf(_("Command exited with status %d"),
                                  g_subprocess_get_exit_status(plugin->process));
        build_run_append_output(plugin, message);
        build_run_append_output(plugin, "\n");
        plugin->host->show_toast(plugin->host, message);
        g_free(message);
    }

    g_clear_object(&plugin->process);
    g_free(request);
}

/* stdout/stderr 管道 EOF 后统一在这里等待子进程退出并收尾。 */
static void
build_run_process_wait(GObject *source, GAsyncResult *result, gpointer user_data)
{
    BuildRunRequest *request;
    BuildRunPlugin *plugin;
    GError *error;

    request = user_data;
    plugin = request->plugin;
    error = NULL;
    g_subprocess_wait_finish(G_SUBPROCESS(source), result, &error);
    if (error != NULL)
    {
        gchar *message;

        message = g_strdup_printf(_("Command failed to start or finish: %s"), error->message);
        build_run_append_output(plugin, message);
        build_run_append_output(plugin, "\n");
        plugin->host->show_toast(plugin->host, message);
        g_free(message);
        g_clear_error(&error);
    }

    build_run_finish(request);
}

static void
build_run_stream_ready(GObject *source, GAsyncResult *result, gpointer user_data)
{
    BuildRunRequest *request;
    GInputStream *stream;
    gchar *buffer;
    gchar *chunk;
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
        chunk = g_strndup(buffer, (gsize)count);
        build_run_append_output(request->plugin, chunk);
        g_free(chunk);
        g_input_stream_read_async(stream,
                                  buffer,
                                  (stream == request->stdout_stream ?
                                   sizeof(request->stdout_buffer) :
                                   sizeof(request->stderr_buffer)),
                                  G_PRIORITY_DEFAULT,
                                  NULL,
                                  build_run_stream_ready,
                                  request);
        return;
    }

    if (error != NULL)
    {
        g_clear_error(&error);
    }
    /* EOF：标记该流完成；两个流都读完后再等待进程退出。 */
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
                                build_run_process_wait,
                                request);
    }
}

static void
build_run_start(BuildRunPlugin *plugin,
                BuildRunMode mode,
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
    BuildRunRequest *request;
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

    command = mode == BUILD_RUN_MODE_BUILD ? plugin->build_command : plugin->run_command;
    if (command == NULL || *command == '\0')
    {
        plugin->host->show_toast(plugin->host,
                                 mode == BUILD_RUN_MODE_BUILD ?
                                 _("Configure a build command first") :
                                 _("Configure a run command first"));
        g_free(file_path);
        return;
    }

    file_directory = g_path_get_dirname(file_path);
    working_directory = build_run_get_working_directory(plugin, file_path);
    source_name = build_run_source_name(file_path);
    expanded_command = build_run_expand_command(command,
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
        build_run_append_output(plugin, message);
        build_run_append_output(plugin, "\n");
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

    build_run_append_output(plugin, "\n$ ");
    build_run_append_output(plugin, expanded_command);
    build_run_append_output(plugin, "\n");
    heading = g_strdup_printf("%s…\n",
                              mode == BUILD_RUN_MODE_BUILD ? _("Building") : _("Running"));
    build_run_append_output(plugin, heading);
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
        build_run_append_output(plugin, message);
        build_run_append_output(plugin, "\n");
        plugin->host->show_toast(plugin->host, message);
        g_free(message);
        g_clear_error(&error);
        return;
    }

    request = g_new0(BuildRunRequest, 1);
    request->plugin = plugin;
    request->mode = mode;
    request->run_after_success = run_after_success;
    request->stdout_stream = g_subprocess_get_stdout_pipe(plugin->process);
    request->stderr_stream = g_subprocess_get_stderr_pipe(plugin->process);
    if (request->stdout_stream != NULL)
    {
        g_input_stream_read_async(request->stdout_stream,
                                  request->stdout_buffer,
                                  sizeof(request->stdout_buffer),
                                  G_PRIORITY_DEFAULT,
                                  NULL,
                                  build_run_stream_ready,
                                  request);
    }
    if (request->stderr_stream != NULL)
    {
        g_input_stream_read_async(request->stderr_stream,
                                  request->stderr_buffer,
                                  sizeof(request->stderr_buffer),
                                  G_PRIORITY_DEFAULT,
                                  NULL,
                                  build_run_stream_ready,
                                  request);
    }
    if (request->stdout_stream == NULL && request->stderr_stream == NULL)
    {
        g_subprocess_wait_async(plugin->process,
                                NULL,
                                build_run_process_wait,
                                request);
    }
}

static void
build_run_build_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    build_run_start(user_data, BUILD_RUN_MODE_BUILD, FALSE);
}

static void
build_run_run_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    build_run_start(user_data, BUILD_RUN_MODE_RUN, FALSE);
}

static void
build_run_build_and_run_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    build_run_start(user_data, BUILD_RUN_MODE_BUILD, TRUE);
}

static void
build_run_stop_clicked(GtkButton *button, gpointer user_data)
{
    BuildRunPlugin *plugin;

    (void)button;
    plugin = user_data;
    if (plugin->process != NULL)
    {
        g_subprocess_force_exit(plugin->process);
        build_run_append_output(plugin, _("Command stop requested\n"));
    }
}

static void
build_run_clear_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    build_run_clear_output(user_data);
}

static void
build_run_window_destroyed(GtkWidget *widget, gpointer user_data)
{
    BuildRunPlugin *plugin;

    (void)widget;
    plugin = user_data;
    plugin->panel = NULL;
    plugin->output_buffer = NULL;
}

static void
build_run_close_clicked(GtkButton *button, gpointer user_data)
{
    BuildRunPlugin *plugin;

    (void)button;
    plugin = user_data;
    if (plugin->panel != NULL)
    {
        gtk_window_destroy(GTK_WINDOW(plugin->panel));
    }
}

static void
build_run_show(BuildRunPlugin *plugin)
{
    GtkWidget *header;
    GtkWidget *title;
    GtkWidget *build_button;
    GtkWidget *run_button;
    GtkWidget *both_button;
    GtkWidget *stop_button;
    GtkWidget *clear_button;
    GtkWidget *close_button;
    GtkWidget *scroll;
    GtkWidget *output;
    GtkWidget *content;

    if (plugin->panel != NULL)
    {
        gtk_window_present(GTK_WINDOW(plugin->panel));
        return;
    }

    plugin->panel = adw_window_new();
    /*
     * 独立顶层 GTK 窗口：不关联主窗口（无 transient、无 modal），
     * 在任务栏作为独立窗口出现，可单独切换、最小化或关闭。
     */
    {
        gchar *file_path;
        gchar *base;
        gchar *window_title;

        file_path = plugin->host->get_current_file_path(plugin->host);
        if (file_path != NULL)
        {
            base = g_path_get_basename(file_path);
            window_title = g_strdup_printf(_("Build & Run — %s"), base);
            gtk_window_set_title(GTK_WINDOW(plugin->panel), window_title);
            g_free(window_title);
            g_free(base);
            g_free(file_path);
        }
        else
        {
            gtk_window_set_title(GTK_WINDOW(plugin->panel), _("Build & Run"));
        }
    }
    gtk_window_set_default_size(GTK_WINDOW(plugin->panel), 820, 500);
    gtk_widget_set_size_request(plugin->panel, 640, 360);
    content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    title = gtk_label_new(_("Build & Run"));
    build_button = gtk_button_new_with_label(_("Build"));
    run_button = gtk_button_new_with_label(_("Run"));
    both_button = gtk_button_new_with_label(_("Build + Run"));
    stop_button = gtk_button_new_from_icon_name("media-playback-stop-symbolic");
    clear_button = gtk_button_new_from_icon_name("edit-clear-symbolic");
    close_button = gtk_button_new_from_icon_name("sidebar-hide-symbolic");
    scroll = gtk_scrolled_window_new();
    output = gtk_text_view_new();
    plugin->output_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(output));

    gtk_widget_add_css_class(title, "heading");
    gtk_widget_set_hexpand(title, TRUE);
    gtk_widget_set_margin_start(header, 12);
    gtk_widget_set_margin_end(header, 6);
    gtk_widget_set_margin_top(header, 8);
    gtk_widget_set_margin_bottom(header, 6);
    gtk_widget_set_tooltip_text(stop_button, _("Stop Active Command"));
    gtk_widget_set_tooltip_text(clear_button, _("Clear Output"));
    gtk_widget_set_tooltip_text(close_button, _("Hide Build Output"));
    gtk_text_view_set_editable(GTK_TEXT_VIEW(output), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(output), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(output), GTK_WRAP_WORD_CHAR);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), output);

    gtk_box_append(GTK_BOX(header), title);
    gtk_box_append(GTK_BOX(header), build_button);
    gtk_box_append(GTK_BOX(header), run_button);
    gtk_box_append(GTK_BOX(header), both_button);
    gtk_box_append(GTK_BOX(header), stop_button);
    gtk_box_append(GTK_BOX(header), clear_button);
    gtk_box_append(GTK_BOX(header), close_button);
    gtk_box_append(GTK_BOX(content), header);
    gtk_box_append(GTK_BOX(content), scroll);
    g_signal_connect(build_button, "clicked", G_CALLBACK(build_run_build_clicked), plugin);
    g_signal_connect(run_button, "clicked", G_CALLBACK(build_run_run_clicked), plugin);
    g_signal_connect(both_button, "clicked", G_CALLBACK(build_run_build_and_run_clicked), plugin);
    g_signal_connect(stop_button, "clicked", G_CALLBACK(build_run_stop_clicked), plugin);
    g_signal_connect(clear_button, "clicked", G_CALLBACK(build_run_clear_clicked), plugin);
    g_signal_connect(close_button, "clicked", G_CALLBACK(build_run_close_clicked), plugin);
    g_signal_connect(plugin->panel,
                     "destroy",
                     G_CALLBACK(build_run_window_destroyed),
                     plugin);
    adw_window_set_content(ADW_WINDOW(plugin->panel), content);
    gtk_window_present(GTK_WINDOW(plugin->panel));
    build_run_append_output(plugin,
                            _("Commands use direct argument parsing, not a shell. Available placeholders: ${file}, ${dir}, ${name}, ${root} (${name} is the source file name without its extension).\n"));
}

static void
build_run_action_build(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action;
    (void)parameter;
    build_run_show(user_data);
    build_run_start(user_data, BUILD_RUN_MODE_BUILD, FALSE);
}

static void
build_run_action_run(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action;
    (void)parameter;
    build_run_show(user_data);
    build_run_start(user_data, BUILD_RUN_MODE_RUN, FALSE);
}

static void
build_run_action_build_and_run(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action;
    (void)parameter;
    build_run_show(user_data);
    build_run_start(user_data, BUILD_RUN_MODE_BUILD, TRUE);
}

static void
build_run_config_widgets_free(BuildRunConfigWidgets *widgets)
{
    g_free(widgets);
}

static void
build_run_save_clicked(GtkButton *button, gpointer user_data)
{
    BuildRunConfigWidgets *widgets;
    BuildRunPlugin *plugin;
    GError *error;

    (void)button;
    widgets = user_data;
    plugin = widgets->plugin;
    g_free(plugin->working_directory);
    g_free(plugin->build_command);
    g_free(plugin->run_command);
    plugin->working_directory = g_strdup(gtk_editable_get_text(GTK_EDITABLE(widgets->directory_row)));
    plugin->build_command = g_strdup(gtk_editable_get_text(GTK_EDITABLE(widgets->build_row)));
    plugin->run_command = g_strdup(gtk_editable_get_text(GTK_EDITABLE(widgets->run_row)));
    error = NULL;

    if (build_run_save_settings(plugin, &error))
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
    return &build_run_plugin_info;
}

G_MODULE_EXPORT gboolean
mt_plugin_activate(MtPluginHost *host, GError **error)
{
    static const gchar *build_accelerators[] = { "F9", NULL };
    static const gchar *run_accelerators[] = { "F10", NULL };
    static const gchar *both_accelerators[] = { "F11", NULL };

    if (host->get_current_file_path == NULL || host->get_parent_window == NULL)
    {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_NOT_SUPPORTED,
                    "Vellum host does not provide the build and run services");
        return FALSE;
    }

    build_run_plugin = g_new0(BuildRunPlugin, 1);
    build_run_plugin->host = host;
    build_run_load_settings(build_run_plugin);

    if (!host->add_action(host, "build-current", build_run_action_build, build_run_plugin, NULL) ||
        !host->add_action(host, "run-current", build_run_action_run, build_run_plugin, NULL) ||
        !host->add_action(host, "build-and-run-current", build_run_action_build_and_run, build_run_plugin, NULL))
    {
        g_free(build_run_plugin->working_directory);
        g_free(build_run_plugin->build_command);
        g_free(build_run_plugin->run_command);
        g_free(build_run_plugin);
        build_run_plugin = NULL;
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_EXISTS,
                    "A build and run action is already registered");
        return FALSE;
    }

    host->set_accelerators(host, "app.build-current", build_accelerators);
    host->set_accelerators(host, "app.run-current", run_accelerators);
    host->set_accelerators(host, "app.build-and-run-current", both_accelerators);

    return TRUE;
}

G_MODULE_EXPORT void
mt_plugin_deactivate(MtPluginHost *host)
{
    (void)host;
    if (build_run_plugin == NULL)
    {
        return;
    }

    if (build_run_plugin->process != NULL)
    {
        g_subprocess_force_exit(build_run_plugin->process);
    }
    if (build_run_plugin->panel != NULL)
    {
        gtk_window_destroy(GTK_WINDOW(build_run_plugin->panel));
    }

    g_clear_object(&build_run_plugin->process);
    g_free(build_run_plugin->working_directory);
    g_free(build_run_plugin->build_command);
    g_free(build_run_plugin->run_command);
    g_free(build_run_plugin);
    build_run_plugin = NULL;
}

G_MODULE_EXPORT void
mt_plugin_configure(MtPluginHost *host, gpointer parent_window)
{
    AdwPreferencesWindow *window;
    AdwPreferencesPage *page;
    AdwPreferencesGroup *group;
    AdwActionRow *save_row;
    GtkWidget *save_button;
    BuildRunConfigWidgets *widgets;
    gchar *save_title;

    (void)host;
    if (build_run_plugin == NULL)
    {
        return;
    }

    window = ADW_PREFERENCES_WINDOW(adw_preferences_window_new());
    gtk_window_set_title(GTK_WINDOW(window), _("Build & Run Settings"));
    gtk_window_set_transient_for(GTK_WINDOW(window), GTK_WINDOW(parent_window));
    gtk_window_set_modal(GTK_WINDOW(window), TRUE);
    page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(group, _("Explicit Commands"));
    adw_preferences_group_set_description(group,
                                          _("Commands run only after F9, F10 or F11. They are parsed into arguments without a shell. Use ${file}, ${dir}, ${name}, or ${root}; ${name} is the source file name without its extension. Quote placeholders when paths may contain spaces."));

    widgets = g_new0(BuildRunConfigWidgets, 1);
    widgets->plugin = build_run_plugin;
    widgets->window = GTK_WINDOW(window);
    widgets->directory_row = ADW_ENTRY_ROW(adw_entry_row_new());
    widgets->build_row = ADW_ENTRY_ROW(adw_entry_row_new());
    widgets->run_row = ADW_ENTRY_ROW(adw_entry_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(widgets->directory_row), _("Working Directory (optional)"));
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(widgets->build_row), _("Build Command"));
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(widgets->run_row), _("Run Command"));
    gtk_editable_set_text(GTK_EDITABLE(widgets->directory_row), build_run_plugin->working_directory);
    gtk_editable_set_text(GTK_EDITABLE(widgets->build_row), build_run_plugin->build_command);
    gtk_editable_set_text(GTK_EDITABLE(widgets->run_row), build_run_plugin->run_command);
    adw_preferences_group_add(group, GTK_WIDGET(widgets->directory_row));
    adw_preferences_group_add(group, GTK_WIDGET(widgets->build_row));
    adw_preferences_group_add(group, GTK_WIDGET(widgets->run_row));

    save_row = ADW_ACTION_ROW(adw_action_row_new());
    save_title = g_markup_escape_text(_("Save Build & Run Settings"), -1);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(save_row), save_title);
    g_free(save_title);
    save_button = gtk_button_new_with_label(_("Save"));
    gtk_widget_set_valign(save_button, GTK_ALIGN_CENTER);
    adw_action_row_add_suffix(save_row, save_button);
    adw_preferences_group_add(group, GTK_WIDGET(save_row));
    g_signal_connect(save_button, "clicked", G_CALLBACK(build_run_save_clicked), widgets);
    g_object_set_data_full(G_OBJECT(window),
                           "vellum-build-run-config-widgets",
                           widgets,
                           (GDestroyNotify)build_run_config_widgets_free);

    adw_preferences_page_add(page, group);
    adw_preferences_window_add(window, page);
    gtk_window_present(GTK_WINDOW(window));
}
