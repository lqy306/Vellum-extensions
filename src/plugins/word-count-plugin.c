/*
 * word-count-plugin.c
 * 示例分析插件：按 Ctrl+Shift+W 显示当前文档的字符、词语和行数统计。
 */

#include "mt-plugin.h"

#include <gmodule.h>
#include <string.h>

static const MtPluginInfo word_count_plugin_info = {
    MT_PLUGIN_API_VERSION,
    "io.github.vellum.document-statistics",
    "Document Statistics",
    "Show character, word and line counts for the current document",
    "0.1.0"
};

static gboolean
word_count_plugin_is_chinese(void)
{
    const gchar * const *languages;

    languages = g_get_language_names();
    return languages != NULL && languages[0] != NULL && g_str_has_prefix(languages[0], "zh");
}

static guint
word_count_plugin_count_words(const gchar *text)
{
    const gchar *cursor;
    gboolean inside_word;
    guint words;

    cursor = text;
    inside_word = FALSE;
    words = 0;

    while (*cursor != '\0')
    {
        gunichar character;
        gboolean is_word_character;

        character = g_utf8_get_char(cursor);
        is_word_character = g_unichar_isalnum(character) || character == '_';

        if (is_word_character && !inside_word)
        {
            words++;
        }

        inside_word = is_word_character;
        cursor = g_utf8_next_char(cursor);
    }

    return words;
}

static void
word_count_plugin_show_statistics(GSimpleAction *action,
                                  GVariant *parameter,
                                  gpointer user_data)
{
    MtPluginHost *host;
    gchar *text;
    guint characters;
    guint words;
    guint lines;
    gchar *message;

    (void)action;
    (void)parameter;

    host = user_data;
    text = host->get_current_text(host);
    characters = (guint)g_utf8_strlen(text, -1);
    words = word_count_plugin_count_words(text);
    lines = *text == '\0' ? 0 : 1;

    {
        const gchar *cursor;

        cursor = text;
        while ((cursor = strchr(cursor, '\n')) != NULL)
        {
            lines++;
            cursor++;
        }
    }

    if (word_count_plugin_is_chinese())
    {
        message = g_strdup_printf("字符：%u，词语：%u，行数：%u", characters, words, lines);
    }
    else
    {
        message = g_strdup_printf("Characters: %u, Words: %u, Lines: %u", characters, words, lines);
    }

    host->show_toast(host, message);
    g_free(message);
    g_free(text);
}

G_MODULE_EXPORT const MtPluginInfo *
mt_plugin_query(void)
{
    return &word_count_plugin_info;
}

G_MODULE_EXPORT gboolean
mt_plugin_activate(MtPluginHost *host, GError **error)
{
    static const gchar *accelerators[] = { "<Primary><Shift>w", NULL };

    (void)error;

    if (!host->add_action(host,
                          "document-statistics",
                          word_count_plugin_show_statistics,
                          host,
                          NULL))
    {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_EXISTS,
                    "The document-statistics action is already registered");
        return FALSE;
    }

    host->set_accelerators(host, "app.document-statistics", accelerators);

    return TRUE;
}

G_MODULE_EXPORT void
mt_plugin_deactivate(MtPluginHost *host)
{
    (void)host;
}
