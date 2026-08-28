/*
 * link-check-plugin.c
 * 从当前文档中提取 HTTP/HTTPS 链接，以小范围 GET 请求检查可达性。
 * 该扩展只会在用户明确触发命令后访问文档中出现的链接。
 */

#include "mt-plugin.h"

#include <glib/gi18n.h>
#include <gmodule.h>
#include <libsoup/soup.h>

#define LINK_CHECK_LIMIT 32

typedef struct _LinkCheck LinkCheck;
typedef struct _LinkRequest LinkRequest;

struct _LinkCheck
{
    MtPluginHost *host;
    GPtrArray *urls;
    guint next_index;
    guint reachable;
    guint failed;
};

struct _LinkRequest
{
    LinkCheck *check;
    SoupMessage *message;
};

static const MtPluginInfo link_check_plugin_info = {
    MT_PLUGIN_API_VERSION,
    "io.github.vellum.link-check",
    "Test Links",
    "Test HTTP and HTTPS links found in the current document",
    "0.1.0"
};

static SoupSession *link_check_session;

static void link_check_next(LinkCheck *check);

static void
link_check_free(LinkCheck *check)
{
    if (check == NULL)
    {
        return;
    }

    g_ptr_array_unref(check->urls);
    g_free(check);
}

static gchar *
link_check_normalize_url(const gchar *match)
{
    gchar *url;
    gsize length;

    url = g_strdup(match);
    length = strlen(url);
    while (length > 0 && strchr(".,;:!?)]}", url[length - 1]) != NULL)
    {
        url[length - 1] = '\0';
        length--;
    }

    return url;
}

static GPtrArray *
link_check_collect_urls(const gchar *text)
{
    GRegex *regex;
    GMatchInfo *matches;
    GPtrArray *urls;
    GHashTable *seen;
    GError *error;

    error = NULL;
    regex = g_regex_new("https?://[^[:space:]<>\\\"']+",
                        G_REGEX_CASELESS | G_REGEX_OPTIMIZE,
                        0,
                        &error);
    if (regex == NULL)
    {
        g_clear_error(&error);
        return g_ptr_array_new_with_free_func(g_free);
    }

    urls = g_ptr_array_new_with_free_func(g_free);
    seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    g_regex_match(regex, text, 0, &matches);

    while (g_match_info_matches(matches) && urls->len < LINK_CHECK_LIMIT)
    {
        gchar *match;
        gchar *url;

        match = g_match_info_fetch(matches, 0);
        url = link_check_normalize_url(match);
        if (*url != '\0' && !g_hash_table_contains(seen, url))
        {
            g_hash_table_add(seen, g_strdup(url));
            g_ptr_array_add(urls, url);
        }
        else
        {
            g_free(url);
        }
        g_free(match);
        g_match_info_next(matches, NULL);
    }

    g_match_info_free(matches);
    g_hash_table_unref(seen);
    g_regex_unref(regex);

    return urls;
}

static void
link_check_complete(LinkCheck *check)
{
    gchar *message;

    message = g_strdup_printf(_("Link test complete: %u reachable, %u failed"),
                              check->reachable,
                              check->failed);
    check->host->show_toast(check->host, message);
    g_free(message);
    link_check_free(check);
}

static void
link_check_request_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    LinkRequest *request;
    GBytes *bytes;
    GError *error;
    guint status;

    request = user_data;
    error = NULL;
    bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &error);

    if (bytes != NULL)
    {
        status = soup_message_get_status(request->message);
        if (status >= 200 && status < 400)
        {
            request->check->reachable++;
        }
        else
        {
            request->check->failed++;
        }
        g_bytes_unref(bytes);
    }
    else
    {
        request->check->failed++;
        g_clear_error(&error);
    }

    g_object_unref(request->message);
    link_check_next(request->check);
    g_free(request);
}

static void
link_check_next(LinkCheck *check)
{
    const gchar *url;
    SoupMessage *message;
    LinkRequest *request;

    if (check->next_index >= check->urls->len)
    {
        link_check_complete(check);
        return;
    }

    url = g_ptr_array_index(check->urls, check->next_index);
    check->next_index++;
    message = soup_message_new("GET", url);
    if (message == NULL)
    {
        check->failed++;
        link_check_next(check);
        return;
    }

    soup_message_headers_append(soup_message_get_request_headers(message), "Range", "bytes=0-0");
    request = g_new0(LinkRequest, 1);
    request->check = check;
    request->message = g_object_ref(message);
    soup_session_send_and_read_async(link_check_session,
                                     message,
                                     G_PRIORITY_DEFAULT,
                                     NULL,
                                     link_check_request_finished,
                                     request);
    g_object_unref(message);
}

static void
link_check_activate_action(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    MtPluginHost *host;
    gchar *text;
    LinkCheck *check;

    (void)action;
    (void)parameter;

    host = user_data;
    text = host->get_current_text(host);
    check = g_new0(LinkCheck, 1);
    check->host = host;
    check->urls = link_check_collect_urls(text);
    g_free(text);

    if (check->urls->len == 0)
    {
        host->show_toast(host, _("No HTTP or HTTPS links found in the current document"));
        link_check_free(check);
        return;
    }

    if (check->urls->len == LINK_CHECK_LIMIT)
    {
        host->show_toast(host, _("Testing the first 32 unique links"));
    }
    else
    {
        host->show_toast(host, _("Testing links…"));
    }

    link_check_next(check);
}

G_MODULE_EXPORT const MtPluginInfo *
mt_plugin_query(void)
{
    return &link_check_plugin_info;
}

G_MODULE_EXPORT gboolean
mt_plugin_activate(MtPluginHost *host, GError **error)
{
    static const gchar *accelerators[] = { "<Primary><Shift>l", NULL };

    if (link_check_session == NULL)
    {
        link_check_session = soup_session_new();
        g_object_set(link_check_session, "timeout", 15, NULL);
    }

    if (!host->add_action(host, "test-links", link_check_activate_action, host, NULL))
    {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_EXISTS,
                    "The test-links action is already registered");
        return FALSE;
    }

    host->set_accelerators(host, "app.test-links", accelerators);

    return TRUE;
}

G_MODULE_EXPORT void
mt_plugin_deactivate(MtPluginHost *host)
{
    (void)host;

    if (link_check_session != NULL)
    {
        soup_session_abort(link_check_session);
        g_clear_object(&link_check_session);
    }
}
