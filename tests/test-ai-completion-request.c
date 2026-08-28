/*
 * test-ai-completion-request.c
 * 验证 AI 异步响应不会在正常完成或插件停用后访问失效宿主。
 */

#include <adwaita.h>
#include <gmodule.h>
#include <glib/gstdio.h>
#include <libsoup/soup.h>

#include "mt-plugin.h"

typedef struct _TestHost TestHost;

struct _TestHost
{
    MtPluginHost host;
    MtPluginActionCallback completion_action;
    gpointer completion_action_data;
    MtPluginActionCallback summary_action;
    gpointer summary_action_data;
    MtPluginKeyCallback key_handler;
    gchar *context;
    gchar *suffix;
    const gchar *language_id;
    gchar *candidate;
    gchar *accepted_text;
    gboolean accepted;
    guint toast_count;
};

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
    if (g_str_equal(name, "ai-complete"))
    {
        test->completion_action = callback;
        test->completion_action_data = user_data;
    }
    else if (g_str_equal(name, "ai-summarize"))
    {
        test->summary_action = callback;
        test->summary_action_data = user_data;
    }
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

static gchar *
host_get_text_before_cursor(MtPluginHost *host)
{
    TestHost *test;

    test = host->private_data;
    return g_strdup(test->context);
}

static gchar *
host_get_text_after_cursor(MtPluginHost *host)
{
    TestHost *test;

    test = host->private_data;
    return g_strdup(test->suffix);
}

static void
host_insert_text(MtPluginHost *host, const gchar *text)
{
    TestHost *test;

    test = host->private_data;
    test->accepted = text != NULL && *text != '\0';
    g_free(test->accepted_text);
    test->accepted_text = g_strdup(text != NULL ? text : "");
}

static void
host_show_toast(MtPluginHost *host, const gchar *message)
{
    TestHost *test;

    (void)message;
    test = host->private_data;
    test->toast_count++;
}

static void
host_show_inline_completion(MtPluginHost *host, const gchar *text)
{
    TestHost *test;

    test = host->private_data;
    g_free(test->candidate);
    test->candidate = g_strdup(text);
}

static void
host_clear_inline_completion(MtPluginHost *host)
{
    TestHost *test;

    test = host->private_data;
    g_clear_pointer(&test->candidate, g_free);
}

static const gchar *
host_get_document_language_id(MtPluginHost *host)
{
    TestHost *test;

    test = host->private_data;
    return test->language_id;
}

static gboolean
host_add_key_handler(MtPluginHost *host,
                     MtPluginKeyCallback handler,
                     gpointer user_data,
                     GDestroyNotify destroy_notify)
{
    TestHost *test;

    (void)user_data;
    (void)destroy_notify;
    test = host->private_data;
    test->key_handler = handler;
    return TRUE;
}

static void
server_handler(SoupServer *server,
               SoupServerMessage *message,
               const char *path,
               GHashTable *query,
               gpointer user_data)
{
    /* 模拟 OpenAI 兼容服务的 SSE 流式响应：补全分多个 data 事件到达。 */
    static const gchar *body =
        "data: {\"choices\":[{\"delta\":{\"content\":\" \"}}]}\n"
        "\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"completion\"}}]}\n"
        "\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"\\n// next line\"}}]}\n"
        "\n"
        "data: [DONE]\n"
        "\n";

    (void)server;
    (void)path;
    (void)query;
    (void)user_data;
    soup_server_message_set_status(message, SOUP_STATUS_OK, NULL);
    soup_server_message_set_response(message,
                                     "text/event-stream",
                                     SOUP_MEMORY_STATIC,
                                     body,
                                     strlen(body));
}

static gboolean
quit_loop(gpointer user_data)
{
    g_main_loop_quit(user_data);
    return G_SOURCE_REMOVE;
}

static void
run_for(guint milliseconds)
{
    GMainLoop *loop;

    loop = g_main_loop_new(NULL, FALSE);
    g_timeout_add(milliseconds, quit_loop, loop);
    g_main_loop_run(loop);
    g_main_loop_unref(loop);
}

static void
wait_for_candidate(TestHost *test, guint timeout_milliseconds)
{
    gint64 deadline;

    deadline = g_get_monotonic_time() + ((gint64)timeout_milliseconds * 1000);
    while (test->candidate == NULL && g_get_monotonic_time() < deadline)
    {
        while (g_main_context_iteration(NULL, FALSE))
        {
        }
        g_usleep(1000);
    }
}

static gchar *
start_server(SoupServer **server_out)
{
    SoupServer *server;
    GSList *uris;
    gchar *uri;
    GError *error;

    server = soup_server_new(NULL, NULL);
    soup_server_add_handler(server, NULL, server_handler, NULL, NULL);
    error = NULL;
    g_assert_true(soup_server_listen_local(server, 0, 0, &error));
    g_assert_no_error(error);
    uris = soup_server_get_uris(server);
    g_assert_nonnull(uris);
    uri = g_uri_to_string(uris->data);
    g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
    *server_out = server;
    return uri;
}

static void write_ai_config_disabled(const gchar *endpoint,
                                     const gchar *model,
                                     const gchar *api_key,
                                     const gchar *disabled_languages);

static void
write_ai_config(const gchar *endpoint,
                const gchar *model,
                const gchar *api_key)
{
    write_ai_config_disabled(endpoint, model, api_key, NULL);
}

static void
write_ai_config_disabled(const gchar *endpoint,
                         const gchar *model,
                         const gchar *api_key,
                         const gchar *disabled_languages)
{
    gchar *directory;
    gchar *path;
    gchar *contents;

    directory = g_build_filename(g_get_user_config_dir(), "vellum", NULL);
    g_mkdir_with_parents(directory, 0700);
    path = g_build_filename(directory, "ai-completion.ini", NULL);
    contents = g_strdup_printf("[AI Completion]\nendpoint=%s\nmodel=%s\napi-key=%s\n"
                               "disabled-languages=%s\n",
                               endpoint,
                               model,
                               api_key,
                               disabled_languages != NULL ? disabled_languages : "");
    g_assert_true(g_file_set_contents(path, contents, -1, NULL));
    g_chmod(path, 0600);
    g_free(contents);
    g_free(path);
    g_free(directory);
}

int
main(void)
{
    TestHost test;
    SoupServer *server;
    gchar *endpoint;
    GModule *module;
    MtPluginActivateFunc activate;
    MtPluginDeactivateFunc deactivate;
    GError *error;
    gchar *config_home;
    const gchar *live_endpoint;
    const gchar *live_model;
    const gchar *live_api_key;
    gboolean live_service;

    if (g_getenv("VELLUM_AI_TEST_PLUGIN") == NULL)
    {
        return 2;
    }

    config_home = g_dir_make_tmp("vellum-ai-test-XXXXXX", NULL);
    if (config_home == NULL)
    {
        return 3;
    }
    g_setenv("XDG_CONFIG_HOME", config_home, TRUE);
    live_endpoint = g_getenv("VELLUM_AI_LIVE_ENDPOINT");
    live_model = g_getenv("VELLUM_AI_LIVE_MODEL");
    live_api_key = g_getenv("VELLUM_AI_LIVE_API_KEY");
    live_service = live_endpoint != NULL && *live_endpoint != '\0';
    server = NULL;
    if (live_service)
    {
        g_assert_nonnull(live_model);
        g_assert_nonnull(live_api_key);
        g_assert_true(*live_model != '\0' && *live_api_key != '\0');
        endpoint = g_strdup(live_endpoint);
        write_ai_config(endpoint, live_model, live_api_key);
    }
    else
    {
        gchar *base_endpoint;

        base_endpoint = start_server(&server);
        endpoint = g_strconcat(base_endpoint, "/v1", NULL);
        g_free(base_endpoint);
        write_ai_config(endpoint, "test-model", "test-key");
    }

    memset(&test, 0, sizeof(test));
    test.context = g_strdup("int main");
    test.suffix = g_strdup("{");
    test.language_id = "c";
    test.host.api_version = MT_PLUGIN_API_VERSION;
    test.host.private_data = &test;
    test.host.add_action = host_add_action;
    test.host.set_accelerators = host_set_accelerators;
    test.host.get_text_before_cursor = host_get_text_before_cursor;
    test.host.get_text_after_cursor = host_get_text_after_cursor;
    test.host.insert_text = host_insert_text;
    test.host.show_toast = host_show_toast;
    test.host.show_inline_completion = host_show_inline_completion;
    test.host.clear_inline_completion = host_clear_inline_completion;
    test.host.add_key_handler = host_add_key_handler;
    test.host.get_document_language_id = host_get_document_language_id;

    module = g_module_open(g_getenv("VELLUM_AI_TEST_PLUGIN"), G_MODULE_BIND_LAZY);
    if (module == NULL)
    {
        return 4;
    }
    activate = NULL;
    deactivate = NULL;
    g_assert_true(g_module_symbol(module, "mt_plugin_activate", (gpointer *)&activate));
    g_module_symbol(module, "mt_plugin_deactivate", (gpointer *)&deactivate);
    error = NULL;
    g_assert_true(activate(&test.host, &error));
    g_assert_no_error(error);
    g_assert_nonnull(test.completion_action);
    g_assert_nonnull(test.summary_action);

    test.completion_action(NULL, NULL, test.completion_action_data);
    if (live_service)
    {
        wait_for_candidate(&test, 28000);
    }
    else
    {
        run_for(900);
    }
    if (live_service)
    {
        g_assert_nonnull(test.candidate);
        g_assert_true(*test.candidate != '\0');
    }
    else
    {
        /* 预览只显示首行，接受必须插入完整的多行补全（对齐真实插件）。 */
        g_assert_cmpstr(test.candidate, ==, " completion");
        g_assert_cmpstr(test.accepted_text, ==, NULL);
    }
    g_assert_nonnull(test.key_handler);
    g_assert_true(test.key_handler(&test.host, GDK_KEY_Tab, 0, 0, NULL));
    g_assert_true(test.accepted);
    if (!live_service)
    {
        g_assert_cmpstr(test.accepted_text, ==, " completion\n// next line");
    }

    if (!live_service)
    {
        /* 阶段二：在设置里禁用当前文档类型（c）后，手动请求被拦截。 */
        guint toasts_before;

        g_clear_pointer(&test.candidate, g_free);
        g_clear_pointer(&test.accepted_text, g_free);
        test.accepted = FALSE;
        deactivate(&test.host);
        write_ai_config_disabled(endpoint, "test-model", "test-key", "c");
        error = NULL;
        g_assert_true(activate(&test.host, &error));
        g_assert_no_error(error);
        toasts_before = test.toast_count;
        test.completion_action(NULL, NULL, test.completion_action_data);
        run_for(400);
        g_assert_null(test.candidate);
        g_assert_false(test.accepted);
        g_assert_cmpuint(test.toast_count, >, toasts_before);
        deactivate(&test.host);
    }
    else if (deactivate != NULL)
    {
        deactivate(&test.host);
    }
    run_for(live_service ? 100 : 300);

    g_module_close(module);
    if (server != NULL)
    {
        soup_server_disconnect(server);
        g_object_unref(server);
    }
    g_free(endpoint);
    g_free(test.context);
    g_free(test.suffix);
    g_free(test.candidate);
    g_free(test.accepted_text);
    g_remove(g_build_filename(config_home, "vellum", "ai-completion.ini", NULL));
    g_rmdir(g_build_filename(config_home, "vellum", NULL));
    g_rmdir(config_home);
    g_free(config_home);
    return 0;
}
