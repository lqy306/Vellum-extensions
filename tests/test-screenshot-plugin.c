/*
 * test-screenshot-plugin.c
 * 验证截图插件：激活后能把当前主窗口渲染成 PNG 保存到图片目录。
 */

#include <adwaita.h>
#include <glib/gstdio.h>
#include <gmodule.h>

#include "mt-plugin.h"
#include "mt-window-private.h"

typedef struct _TestHost
{
    MtPluginHost host;
    GtkWindow *window;
    gchar *action_name;
    GSimpleAction *action;
    gpointer action_data;
    gchar *toast;
} TestHost;

static gboolean
host_add_action(MtPluginHost *host,
                const gchar *name,
                MtPluginActionCallback callback,
                gpointer user_data,
                GDestroyNotify destroy_notify)
{
    TestHost *test;

    (void)destroy_notify;
    test = host->private_data;
    test->action_name = g_strdup(name);
    test->action = g_simple_action_new(name, NULL);
    g_signal_connect(test->action, "activate", G_CALLBACK(callback), user_data);
    test->action_data = user_data;
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

static GtkWindow *
host_get_parent_window(MtPluginHost *host)
{
    return ((TestHost *)host->private_data)->window;
}

static void
host_show_toast(MtPluginHost *host, const gchar *message)
{
    TestHost *test;

    test = host->private_data;
    g_free(test->toast);
    test->toast = g_strdup(message);
}

static void
spin_main_context(void)
{
    gint index;

    for (index = 0; index < 30; index++)
    {
        while (g_main_context_iteration(NULL, FALSE))
        {
        }
        g_usleep(20000);
    }
}

static void
prepare_pictures_dir(gchar **config_home, gchar **pictures_home)
{
    GError *error;
    gchar *path;
    gchar *contents;

    error = NULL;
    *config_home = g_dir_make_tmp("vellum-shot-config-XXXXXX", &error);
    g_assert_no_error(error);
    *pictures_home = g_dir_make_tmp("vellum-shot-pictures-XXXXXX", &error);
    g_assert_no_error(error);
    path = g_build_filename(*config_home, "user-dirs.dirs", NULL);
    contents = g_strdup_printf("XDG_PICTURES_DIR=\"%s\"\n", *pictures_home);
    g_assert_true(g_file_set_contents(path, contents, -1, &error));
    g_assert_no_error(error);
    g_free(contents);
    g_free(path);
    g_setenv("XDG_CONFIG_HOME", *config_home, TRUE);
}

static void
test_screenshot_plugin(void)
{
    GError *error;
    AdwApplication *application;
    MtSettings *settings;
    MtWindow *window;
    TestHost test;
    GModule *module;
    MtPluginActivateFunc activate;
    MtPluginDeactivateFunc deactivate;
    gchar *config_home;
    gchar *pictures_home;
    gchar *contents;
    gsize length;
    const gchar *plugin_path;

    plugin_path = g_getenv("VELLUM_SCREENSHOT_TEST_PLUGIN");
    g_assert_nonnull(plugin_path);

    prepare_pictures_dir(&config_home, &pictures_home);

    error = NULL;
    application = ADW_APPLICATION(adw_application_new("io.github.vellum.ScreenshotPluginTest",
                                                       G_APPLICATION_NON_UNIQUE));
    g_assert_true(g_application_register(G_APPLICATION(application), NULL, &error));
    g_assert_no_error(error);
    settings = mt_settings_new();
    window = mt_window_new(application, settings);
    gtk_window_present(GTK_WINDOW(window->window));
    mt_window_new_document(window);
    spin_main_context();

    memset(&test, 0, sizeof(test));
    test.host.api_version = MT_PLUGIN_API_VERSION;
    test.host.private_data = &test;
    test.host.add_action = host_add_action;
    test.host.set_accelerators = host_set_accelerators;
    test.host.get_parent_window = host_get_parent_window;
    test.host.show_toast = host_show_toast;
    test.window = GTK_WINDOW(window->window);

    module = g_module_open(plugin_path, G_MODULE_BIND_LAZY);
    g_assert_nonnull(module);
    activate = NULL;
    deactivate = NULL;
    g_assert_true(g_module_symbol(module, "mt_plugin_activate", (gpointer *)&activate));
    g_module_symbol(module, "mt_plugin_deactivate", (gpointer *)&deactivate);
    error = NULL;
    g_assert_true(activate(&test.host, &error));
    g_assert_no_error(error);
    g_assert_nonnull(test.action);

    g_action_activate(G_ACTION(test.action), NULL);
    spin_main_context();

    g_assert_nonnull(test.toast);
    g_print("toast: %s\n", test.toast);
    g_assert_true(g_file_test(test.toast, G_FILE_TEST_IS_REGULAR));
    g_assert_true(g_str_has_suffix(test.toast, ".png"));
    contents = NULL;
    g_assert_true(g_file_get_contents(test.toast, &contents, &length, &error));
    g_assert_no_error(error);
    g_assert_cmpuint(length, >, 8);
    g_assert_true(memcmp(contents, "\x89PNG\r\n\x1a\n", 8) == 0);
    g_free(contents);

    if (deactivate != NULL)
    {
        deactivate(&test.host);
    }
    g_module_close(module);
    g_free(test.toast);
    g_free(test.action_name);
    g_clear_object(&test.action);
    gtk_window_destroy(GTK_WINDOW(window->window));
    spin_main_context();
    mt_window_free(window);
    mt_settings_free(settings);
    g_object_unref(application);

    g_free(pictures_home);
    g_free(config_home);
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/vellum/plugins/screenshot", test_screenshot_plugin);

    return g_test_run();
}
