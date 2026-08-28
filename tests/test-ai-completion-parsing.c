/*
 * test-ai-completion-parsing.c
 * 直接包含 AI 补全插件源码，回归验证响应解析与端点归一化不再崩溃。
 * 不发起任何真实网络请求。
 */

#include <glib.h>
#include <gtk/gtk.h>
#include <json-glib/json-glib.h>

#include "../src/plugins/ai-completion-plugin.c"

static gboolean
quit_loop(gpointer user_data)
{
    g_main_loop_quit(user_data);
    return G_SOURCE_REMOVE;
}

static void
test_error_detail_is_null_safe(void)
{
    gchar *detail;

    detail = ai_completion_error_detail_data(NULL, 0);
    g_assert_cmpstr(detail, ==, "");
    g_free(detail);
    detail = ai_completion_error_detail_data("boom", 4);
    g_assert_cmpstr(detail, ==, "boom");
    g_free(detail);
}

static void
test_extract_content_handles_empty_and_malformed(void)
{
    GError *error;
    gchar *result;

    error = NULL;
    result = ai_completion_extract_content(NULL, 0, &error);
    g_assert_null(result);
    g_assert_nonnull(error);
    g_clear_error(&error);

    result = ai_completion_extract_content("", 0, &error);
    g_assert_null(result);
    g_assert_nonnull(error);
    g_clear_error(&error);

    result = ai_completion_extract_content("not json", 8, &error);
    g_assert_null(result);
    g_assert_nonnull(error);
    g_clear_error(&error);

    result = ai_completion_extract_content("{\"error\":{\"message\":\"boom\"}}", -1, &error);
    g_assert_null(result);
    g_assert_nonnull(error);
    g_clear_error(&error);
}

static void
test_extract_content_parses_supported_shapes(void)
{
    GError *error;
    gchar *result;

    error = NULL;
    result = ai_completion_extract_content(
        "{\"choices\":[{\"message\":{\"content\":\" completion\"}}]}", -1, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(result, ==, " completion");
    g_free(result);

    result = ai_completion_extract_content(
        "{\"choices\":[{\"delta\":{\"content\":\" delta\"}}]}", -1, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(result, ==, " delta");
    g_free(result);

    result = ai_completion_extract_content(
        "{\"choices\":[{\"text\":\" text\"}]}", -1, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(result, ==, " text");
    g_free(result);

    result = ai_completion_extract_content(
        "{\"choices\":[{\"message\":{\"content\":[{\"type\":\"text\",\"text\":\" array text\"}]}}]}",
        -1,
        &error);
    g_assert_no_error(error);
    g_assert_cmpstr(result, ==, " array text");
    g_free(result);

    result = ai_completion_extract_content(
        "{\"choices\":[{\"message\":{\"content\":null}}]}", -1, &error);
    g_assert_null(result);
    g_assert_nonnull(error);
    g_clear_error(&error);
}

static void
test_inline_candidate_normalization(void)
{
    gchar *candidate;

    candidate = ai_completion_prepare_inline_candidate("  suffix");
    g_assert_cmpstr(candidate, ==, "  suffix");
    g_free(candidate);

    candidate = ai_completion_prepare_inline_candidate("\n\rnext line\nignored line");
    g_assert_cmpstr(candidate, ==, "next line");
    g_free(candidate);

    candidate = ai_completion_prepare_inline_candidate("\r\n");
    g_assert_null(candidate);

    candidate = ai_completion_prepare_inline_candidate(NULL);
    g_assert_null(candidate);
}

static void
test_endpoint_normalization(void)
{
    gchar *normalized;

    normalized = ai_completion_normalize_endpoint("https://api.deepseek.com");
    g_assert_cmpstr(normalized, ==, "https://api.deepseek.com/chat/completions");
    g_free(normalized);

    normalized = ai_completion_normalize_endpoint("https://api.deepseek.com/");
    g_assert_cmpstr(normalized, ==, "https://api.deepseek.com/chat/completions");
    g_free(normalized);

    normalized = ai_completion_normalize_endpoint("https://api.deepseek.com/v1");
    g_assert_cmpstr(normalized, ==, "https://api.deepseek.com/v1/chat/completions");
    g_free(normalized);

    normalized = ai_completion_normalize_endpoint("https://api.deepseek.com/v1/");
    g_assert_cmpstr(normalized, ==, "https://api.deepseek.com/v1/chat/completions");
    g_free(normalized);

    normalized = ai_completion_normalize_endpoint("https://api.deepseek.com/chat/completions");
    g_assert_cmpstr(normalized, ==, "https://api.deepseek.com/chat/completions");
    g_free(normalized);

    normalized = ai_completion_normalize_endpoint("https://api.deepseek.com/chat/completions/");
    g_assert_cmpstr(normalized, ==, "https://api.deepseek.com/chat/completions");
    g_free(normalized);

    normalized = ai_completion_normalize_endpoint("https://custom.example.com/api/v2/models/chat");
    g_assert_cmpstr(normalized, ==, "https://custom.example.com/api/v2/models/chat");
    g_free(normalized);

    normalized = ai_completion_normalize_endpoint("");
    g_assert_cmpstr(normalized, ==, "");
    g_free(normalized);
}

static void
test_build_body_uses_prefix_and_suffix(void)
{
    gchar *body;
    JsonParser *parser;
    JsonObject *object;
    JsonObject *message;
    const gchar *content;

    body = ai_completion_build_body("test-model", "int main", "{", "Existing summary");
    parser = json_parser_new();
    g_assert_true(json_parser_load_from_data(parser, body, -1, NULL));
    object = json_node_get_object(json_parser_get_root(parser));
    g_assert_cmpstr(json_object_get_string_member(object, "model"), ==, "test-model");
    /* 对齐真实补全工具：请求体必须声明 SSE 流式输出。 */
    g_assert_true(json_object_get_boolean_member(object, "stream"));
    message = json_array_get_object_element(json_object_get_array_member(object, "messages"), 1);
    content = json_object_get_string_member(message, "content");
    g_assert_nonnull(strstr(content, "<document_summary>"));
    g_assert_nonnull(strstr(content, "Existing summary"));
    g_assert_nonnull(strstr(content, "<fim_prefix>"));
    g_assert_nonnull(strstr(content, "int main"));
    g_assert_nonnull(strstr(content, "<fim_suffix>"));
    g_assert_nonnull(strstr(content, "{"));
    g_object_unref(parser);
    g_free(body);
}

static void
test_outdent_adjustment(void)
{
    gchar *result;

    /* 当前行纯空白且已输入空白与补全开头空白重叠：裁掉重复部分。 */
    result = ai_completion_outdent("  return x", "  ");
    g_assert_cmpstr(result, ==, "return x");
    g_free(result);

    result = ai_completion_outdent("  return x", "    ");
    g_assert_cmpstr(result, ==, "return x");
    g_free(result);

    /* 空白种类不同：不裁。 */
    result = ai_completion_outdent("  return x", "\t");
    g_assert_cmpstr(result, ==, "  return x");
    g_free(result);

    /* 当前行不是纯空白：不裁。 */
    result = ai_completion_outdent("  return x", "x = 1");
    g_assert_cmpstr(result, ==, "  return x");
    g_free(result);

    /* 刚回车或空行：缩进来自补全本身，不裁。 */
    result = ai_completion_outdent("  return x", "");
    g_assert_cmpstr(result, ==, "  return x");
    g_free(result);

    result = ai_completion_outdent("  return x", "a = 1\n");
    g_assert_cmpstr(result, ==, "  return x");
    g_free(result);

    /* 补全没有开头空白：不裁。 */
    result = ai_completion_outdent("return x", "  ");
    g_assert_cmpstr(result, ==, "return x");
    g_free(result);

    result = ai_completion_outdent(NULL, "  ");
    g_assert_null(result);
}

static void
test_trim_suffix_overlap(void)
{
    gchar *result;

    /* 补全结尾与光标后已有文本重叠：裁掉重叠部分，避免重复。 */
    result = ai_completion_trim_suffix_overlap("return true;", ";");
    g_assert_cmpstr(result, ==, "return true");
    g_free(result);

    result = ai_completion_trim_suffix_overlap("return true;", ";\nmore");
    g_assert_cmpstr(result, ==, "return true");
    g_free(result);

    result = ai_completion_trim_suffix_overlap("abc", "bc");
    g_assert_cmpstr(result, ==, "a");
    g_free(result);

    /* 无重叠或后缀为空：原样保留。 */
    result = ai_completion_trim_suffix_overlap("abc", "xyz");
    g_assert_cmpstr(result, ==, "abc");
    g_free(result);

    result = ai_completion_trim_suffix_overlap("abc", "");
    g_assert_cmpstr(result, ==, "abc");
    g_free(result);

    result = ai_completion_trim_suffix_overlap("abc", NULL);
    g_assert_cmpstr(result, ==, "abc");
    g_free(result);

    result = ai_completion_trim_suffix_overlap("", "abc");
    g_assert_cmpstr(result, ==, "");
    g_free(result);
}

static void
test_adjust_candidate_combined(void)
{
    gchar *result;

    /* 出缩进 + 后缀去重组合生效。 */
    result = ai_completion_adjust_candidate("  return true;", "  ", ";");
    g_assert_cmpstr(result, ==, "return true");
    g_free(result);

    /* 尾部换行被裁掉。 */
    result = ai_completion_adjust_candidate("x\n", "x = 1", "");
    g_assert_cmpstr(result, ==, "x");
    g_free(result);

    result = ai_completion_adjust_candidate("x\r\n", "x = 1", "");
    g_assert_cmpstr(result, ==, "x");
    g_free(result);

    /* 刚回车时保留补全自带的缩进。 */
    result = ai_completion_adjust_candidate("\n  next();", "a = 1\n", "");
    g_assert_cmpstr(result, ==, "\n  next();");
    g_free(result);
}

static void
test_sse_payload_delta(void)
{
    gchar *delta;

    delta = ai_completion_sse_payload_delta("{\"choices\":[{\"delta\":{\"content\":\" hi\"}}]}");
    g_assert_cmpstr(delta, ==, " hi");
    g_free(delta);

    delta = ai_completion_sse_payload_delta(
        "{\"choices\":[{\"delta\":{\"content\":[{\"type\":\"text\",\"text\":\" arr\"}]}}]}");
    g_assert_cmpstr(delta, ==, " arr");
    g_free(delta);

    delta = ai_completion_sse_payload_delta("{\"choices\":[{\"delta\":{\"content\":null}}]}");
    g_assert_null(delta);

    delta = ai_completion_sse_payload_delta("{\"choices\":[]}");
    g_assert_null(delta);

    delta = ai_completion_sse_payload_delta("not json");
    g_assert_null(delta);

    delta = ai_completion_sse_payload_delta("[DONE]");
    g_assert_null(delta);
}

static void
test_disabled_languages_list(void)
{
    GHashTable *disabled;
    GHashTable *parsed;
    gchar *joined;

    /* 缺失或空值：全部启用。 */
    disabled = ai_completion_parse_disabled_languages(NULL);
    g_assert_cmpuint(g_hash_table_size(disabled), ==, 0);
    joined = ai_completion_join_disabled_languages(disabled);
    g_assert_cmpstr(joined, ==, "");
    g_free(joined);
    g_hash_table_unref(disabled);

    disabled = ai_completion_parse_disabled_languages("");
    g_assert_cmpuint(g_hash_table_size(disabled), ==, 0);
    g_hash_table_unref(disabled);

    /* 解析：去掉空白，忽略空段。 */
    disabled = ai_completion_parse_disabled_languages("c;cpp; js ;\tini;;");
    g_assert_cmpuint(g_hash_table_size(disabled), ==, 4);
    g_assert_true(g_hash_table_contains(disabled, "c"));
    g_assert_true(g_hash_table_contains(disabled, "cpp"));
    g_assert_true(g_hash_table_contains(disabled, "js"));
    g_assert_true(g_hash_table_contains(disabled, "ini"));
    g_assert_false(g_hash_table_contains(disabled, "python"));

    /* 序列化：按 id 排序，可往返。 */
    joined = ai_completion_join_disabled_languages(disabled);
    g_assert_cmpstr(joined, ==, "c;cpp;ini;js");
    parsed = ai_completion_parse_disabled_languages(joined);
    g_assert_cmpuint(g_hash_table_size(parsed), ==, 4);
    g_assert_true(g_hash_table_contains(parsed, "js"));
    g_hash_table_unref(parsed);
    g_free(joined);
    g_hash_table_unref(disabled);

    /* NULL 集合序列化为空串。 */
    joined = ai_completion_join_disabled_languages(NULL);
    g_assert_cmpstr(joined, ==, "");
    g_free(joined);
}

static const gchar *language_id;
static gboolean code_document;

static const gchar *
host_get_document_language_id(MtPluginHost *host)
{
    (void)host;
    return language_id;
}

static gboolean
host_get_is_code_document(MtPluginHost *host)
{
    (void)host;
    return code_document;
}

static void
test_document_allowed_gating(void)
{
    MtPluginHost host;
    gboolean result;

    memset(&host, 0, sizeof(host));

    /* 语言已知且启用：允许。 */
    language_id = "c";
    code_document = TRUE;
    host.get_document_language_id = host_get_document_language_id;
    host.get_is_code_document = host_get_is_code_document;
    ai_completion_languages_set(ai_completion_parse_disabled_languages(NULL));
    g_assert_true(ai_completion_document_allowed(&host, TRUE));
    g_assert_true(ai_completion_document_allowed(&host, FALSE));

    /* 语言被禁用：自动与手动都拒绝。 */
    ai_completion_languages_set(ai_completion_parse_disabled_languages("c"));
    g_assert_false(ai_completion_document_allowed(&host, TRUE));
    g_assert_false(ai_completion_document_allowed(&host, FALSE));

    /* 其他语言不受影响。 */
    language_id = "python";
    g_assert_true(ai_completion_document_allowed(&host, TRUE));

    /* 语言未知（纯文本）：自动补全拒绝，手动保持可用（历史行为）。 */
    language_id = NULL;
    g_assert_false(ai_completion_document_allowed(&host, TRUE));
    g_assert_true(ai_completion_document_allowed(&host, FALSE));

    /* 旧宿主（无语言接口）：按 is_code_document 判断自动，手动始终允许。 */
    memset(&host, 0, sizeof(host));
    host.get_is_code_document = host_get_is_code_document;
    code_document = TRUE;
    g_assert_true(ai_completion_document_allowed(&host, TRUE));
    code_document = FALSE;
    g_assert_false(ai_completion_document_allowed(&host, TRUE));
    g_assert_true(ai_completion_document_allowed(&host, FALSE));

    /* 无任何接口的宿主：全部允许（与历史一致）。 */
    memset(&host, 0, sizeof(host));
    g_assert_true(ai_completion_document_allowed(&host, TRUE));
    g_assert_true(ai_completion_document_allowed(&host, FALSE));

    ai_completion_languages_set(NULL);
}

static void
test_language_category_classification(void)
{
    g_assert_cmpint(ai_completion_language_category("c"), ==, AI_LANGUAGE_CATEGORY_CODE);
    g_assert_cmpint(ai_completion_language_category("python"), ==, AI_LANGUAGE_CATEGORY_CODE);
    g_assert_cmpint(ai_completion_language_category("sh"), ==, AI_LANGUAGE_CATEGORY_CODE);
    g_assert_cmpint(ai_completion_language_category("js"), ==, AI_LANGUAGE_CATEGORY_WEB);
    g_assert_cmpint(ai_completion_language_category("typescript"), ==, AI_LANGUAGE_CATEGORY_WEB);
    g_assert_cmpint(ai_completion_language_category("html"), ==, AI_LANGUAGE_CATEGORY_WEB);
    g_assert_cmpint(ai_completion_language_category("css"), ==, AI_LANGUAGE_CATEGORY_WEB);
    g_assert_cmpint(ai_completion_language_category("json"), ==, AI_LANGUAGE_CATEGORY_CONFIG);
    g_assert_cmpint(ai_completion_language_category("toml"), ==, AI_LANGUAGE_CATEGORY_CONFIG);
    g_assert_cmpint(ai_completion_language_category("yaml"), ==, AI_LANGUAGE_CATEGORY_CONFIG);
    g_assert_cmpint(ai_completion_language_category("xml"), ==, AI_LANGUAGE_CATEGORY_CONFIG);
    g_assert_cmpint(ai_completion_language_category("markdown"), ==, AI_LANGUAGE_CATEGORY_TEXT);
    g_assert_cmpint(ai_completion_language_category("latex"), ==, AI_LANGUAGE_CATEGORY_TEXT);
    g_assert_cmpint(ai_completion_language_category("diff"), ==, AI_LANGUAGE_CATEGORY_TEXT);
    g_assert_cmpint(ai_completion_language_category("no-such-language"), ==, AI_LANGUAGE_CATEGORY_OTHER);
    g_assert_cmpint(ai_completion_language_category(NULL), ==, AI_LANGUAGE_CATEGORY_OTHER);
}

static void
test_disabled_from_items(void)
{
    GListStore *store;
    AiLanguageItem *item;
    GHashTable *disabled;
    gchar *joined;

    store = g_list_store_new(AI_TYPE_LANGUAGE_ITEM);
    item = ai_language_item_new("c", "C", "*.c;*.h", TRUE);
    g_list_store_append(store, item);
    g_object_unref(item);
    item = ai_language_item_new("python", "Python", "*.py", FALSE);
    g_list_store_append(store, item);
    g_object_unref(item);
    item = ai_language_item_new("html", "HTML", "*.html;*.htm", FALSE);
    g_list_store_append(store, item);
    g_object_unref(item);
    /* 分类分隔行：不参与保存（即使未勾选也不计入禁用列表）。 */
    item = ai_language_item_new(NULL, "Code", "", FALSE);
    item->is_category_row = TRUE;
    item->category = AI_LANGUAGE_CATEGORY_CODE;
    g_list_store_append(store, item);
    g_object_unref(item);

    /* 保存路径：未勾选的文档类型进入禁用集合，分类行被跳过。 */
    disabled = ai_completion_disabled_from_items(store);
    g_assert_cmpuint(g_hash_table_size(disabled), ==, 2);
    g_assert_false(g_hash_table_contains(disabled, "c"));
    g_assert_true(g_hash_table_contains(disabled, "python"));
    g_assert_true(g_hash_table_contains(disabled, "html"));
    joined = ai_completion_join_disabled_languages(disabled);
    g_assert_cmpstr(joined, ==, "html;python");
    g_free(joined);
    g_hash_table_unref(disabled);

    /* 空表：空集合。 */
    g_list_store_remove_all(store);
    disabled = ai_completion_disabled_from_items(store);
    g_assert_cmpuint(g_hash_table_size(disabled), ==, 0);
    g_hash_table_unref(disabled);

    g_object_unref(store);
}

static void
test_set_category_enabled(void)
{
    GListStore *store;
    AiLanguageItem *item;

    store = g_list_store_new(AI_TYPE_LANGUAGE_ITEM);
    item = ai_language_item_new("c", "C", "*.c;*.h", FALSE);
    item->category = AI_LANGUAGE_CATEGORY_CODE;
    g_list_store_append(store, item);
    g_object_unref(item);
    item = ai_language_item_new("sh", "Shell", "*.sh", FALSE);
    item->category = AI_LANGUAGE_CATEGORY_CODE;
    g_list_store_append(store, item);
    g_object_unref(item);
    item = ai_language_item_new("html", "HTML", "*.html", TRUE);
    item->category = AI_LANGUAGE_CATEGORY_WEB;
    g_list_store_append(store, item);
    g_object_unref(item);

    /* 大类开关：只影响该大类的语言行。 */
    ai_completion_set_category_enabled(store, AI_LANGUAGE_CATEGORY_CODE, TRUE);
    g_assert_true(((AiLanguageItem *)g_list_model_get_item(G_LIST_MODEL(store), 0))->enabled);
    g_assert_true(((AiLanguageItem *)g_list_model_get_item(G_LIST_MODEL(store), 1))->enabled);
    item = g_list_model_get_item(G_LIST_MODEL(store), 2);
    g_assert_true(item->enabled);
    g_object_unref(item);
    g_object_unref(g_list_model_get_item(G_LIST_MODEL(store), 0));
    g_object_unref(g_list_model_get_item(G_LIST_MODEL(store), 1));

    ai_completion_set_category_enabled(store, AI_LANGUAGE_CATEGORY_CODE, FALSE);
    item = g_list_model_get_item(G_LIST_MODEL(store), 0);
    g_assert_false(item->enabled);
    g_object_unref(item);
    item = g_list_model_get_item(G_LIST_MODEL(store), 2);
    g_assert_true(item->enabled);
    g_object_unref(item);

    /* NULL 存储安全。 */
    ai_completion_set_category_enabled(NULL, AI_LANGUAGE_CATEGORY_CODE, TRUE);

    g_object_unref(store);
}

static GtkWidget *
find_descendant_checkbutton(GtkWidget *widget)
{
    GtkWidget *child;

    for (child = gtk_widget_get_first_child(widget);
         child != NULL;
         child = gtk_widget_get_next_sibling(child))
    {
        if (GTK_IS_CHECK_BUTTON(child))
        {
            return child;
        }
        {
            GtkWidget *found;

            found = find_descendant_checkbutton(child);
            if (found != NULL)
            {
                return found;
            }
        }
    }

    return NULL;
}

static void
test_category_toggle_syncs_rows(void)
{
    GListStore *store;
    AiLanguageItem *item;
    GtkColumnView *table;
    GtkSingleSelection *selection;
    GtkListItemFactory *factory;
    GtkColumnViewColumn *column;
    GtkCheckButton *category_check;
    GtkWindow *window;
    GtkWidget *row_check;
    GHashTable *checks;
    guint index;

    store = g_list_store_new(AI_TYPE_LANGUAGE_ITEM);
    item = ai_language_item_new("c", "C", "*.c;*.h", FALSE);
    item->category = AI_LANGUAGE_CATEGORY_CODE;
    g_list_store_append(store, item);
    g_object_unref(item);
    item = ai_language_item_new("sh", "Shell", "*.sh", FALSE);
    item->category = AI_LANGUAGE_CATEGORY_CODE;
    g_list_store_append(store, item);
    g_object_unref(item);
    item = ai_language_item_new("html", "HTML", "*.html", TRUE);
    item->category = AI_LANGUAGE_CATEGORY_WEB;
    g_list_store_append(store, item);
    g_object_unref(item);

    table = GTK_COLUMN_VIEW(gtk_column_view_new(NULL));
    checks = g_hash_table_new(NULL, NULL);
    g_object_set_data_full(G_OBJECT(table), "vellum-ai-category-checks",
                           checks, (GDestroyNotify)g_hash_table_unref);
    selection = gtk_single_selection_new(G_LIST_MODEL(store));
    gtk_single_selection_set_autoselect(selection, FALSE);
    gtk_column_view_set_model(table, GTK_SELECTION_MODEL(selection));
    factory = gtk_signal_list_item_factory_new();
    g_signal_connect(factory, "setup",
                     G_CALLBACK(ai_completion_check_factory_setup), NULL);
    g_signal_connect(factory, "bind",
                     G_CALLBACK(ai_completion_check_factory_bind), NULL);
    g_signal_connect(factory, "unbind",
                     G_CALLBACK(ai_completion_check_factory_unbind), NULL);
    column = gtk_column_view_column_new(NULL, factory);
    gtk_column_view_append_column(table, column);

    /* 大类复选框：与真实配置页相同的接线。 */
    category_check = GTK_CHECK_BUTTON(gtk_check_button_new_with_label("Code"));
    g_object_set_data(G_OBJECT(category_check), "vellum-ai-category",
                      GINT_TO_POINTER(AI_LANGUAGE_CATEGORY_CODE));
    g_object_set_data(G_OBJECT(category_check), "vellum-ai-store", store);
    g_signal_connect(category_check, "toggled",
                     G_CALLBACK(ai_completion_category_toggled), NULL);
    g_hash_table_replace(checks, GINT_TO_POINTER(AI_LANGUAGE_CATEGORY_CODE),
                         category_check);

    window = GTK_WINDOW(gtk_window_new());
    gtk_window_set_default_size(window, 300, 200);
    gtk_window_set_child(window, GTK_WIDGET(table));
    gtk_window_present(window);
    for (index = 0; index < 20; index++)
    {
        while (g_main_context_iteration(NULL, FALSE))
        {
        }
        g_usleep(5000);
    }

    row_check = find_descendant_checkbutton(GTK_WIDGET(table));
    g_assert_nonnull(row_check);
    g_assert_false(gtk_check_button_get_active(GTK_CHECK_BUTTON(row_check)));

    /* 上方大类开启 → 下方该大类所有行同步勾选。 */
    gtk_check_button_set_active(category_check, TRUE);
    g_print("DBG step: toggled TRUE, row_active=%d\n", gtk_check_button_get_active(GTK_CHECK_BUTTON(row_check)));
    g_assert_true(gtk_check_button_get_active(GTK_CHECK_BUTTON(row_check)));

    /* 下方单独取消一行 → 大类变为部分勾选（三态）。 */
    gtk_check_button_set_active(GTK_CHECK_BUTTON(row_check), FALSE);
    g_assert_true(gtk_check_button_get_inconsistent(category_check));
    g_assert_false(gtk_check_button_get_active(category_check));

    /* 点击部分勾选的大类 → 整类全部开启，三态消失。 */
    gtk_check_button_set_active(category_check, TRUE);
    g_assert_false(gtk_check_button_get_inconsistent(category_check));
    g_assert_true(gtk_check_button_get_active(category_check));
    g_assert_true(gtk_check_button_get_active(GTK_CHECK_BUTTON(row_check)));

    gtk_window_destroy(window);
    g_object_unref(store);
}

static void
test_summary_auto_enabled_gating(void)
{
    GKeyFile *settings;

    /* 缺失 key：默认开启。 */
    settings = g_key_file_new();
    g_assert_true(ai_code_summary_auto_enabled(settings));
    g_key_file_unref(settings);

    /* 显式关闭：自动总结被抑制（手动总结不受影响）。 */
    settings = g_key_file_new();
    g_key_file_set_boolean(settings, "AI Code Summary", "enabled", FALSE);
    g_assert_false(ai_code_summary_auto_enabled(settings));
    g_key_file_unref(settings);

    settings = g_key_file_new();
    g_key_file_set_boolean(settings, "AI Code Summary", "enabled", TRUE);
    g_assert_true(ai_code_summary_auto_enabled(settings));
    g_key_file_unref(settings);
}

static void
test_configure_builds_page(void)
{
    MtPluginHost host;
    gchar *old_config_home;
    gchar *config_home;
    gchar *config_dir;
    gchar *path;
    gchar *contents;

    /* 隔离配置：不让测试读写真实的用户配置。 */
    old_config_home = g_strdup(g_getenv("XDG_CONFIG_HOME") != NULL ?
                               g_getenv("XDG_CONFIG_HOME") : "");
    config_home = g_dir_make_tmp("vellum-ai-config-XXXXXX", NULL);
    g_assert_nonnull(config_home);
    g_setenv("XDG_CONFIG_HOME", config_home, TRUE);
    config_dir = g_build_filename(config_home, "vellum", NULL);
    g_mkdir_with_parents(config_dir, 0700);
    path = g_build_filename(config_dir, "ai-completion.ini", NULL);
    contents = g_strdup("[AI Completion]\nendpoint=https://example.com/v1\n"
                        "model=test-model\napi-key=test-key\n"
                        "disabled-languages=c;python\n");
    g_assert_true(g_file_set_contents(path, contents, -1, NULL));
    g_free(contents);
    g_free(path);
    g_free(config_dir);

    memset(&host, 0, sizeof(host));
    language_id = "c";
    host.get_document_language_id = host_get_document_language_id;

    /* 旧实现在这里因提前释放 settings 而段错误；构建整页即回归覆盖。 */
    mt_plugin_configure(&host, NULL);

    /* 让窗口正常展示一帧后销毁，验证构建与展示过程无崩溃。 */
    {
        GMainLoop *loop;

        loop = g_main_loop_new(NULL, FALSE);
        g_timeout_add(50, quit_loop, loop);
        g_main_loop_run(loop);
        g_main_loop_unref(loop);
    }

    if (old_config_home != NULL && *old_config_home != '\0')
    {
        g_setenv("XDG_CONFIG_HOME", old_config_home, TRUE);
    }
    else
    {
        g_unsetenv("XDG_CONFIG_HOME");
    }
    g_free(old_config_home);
    g_remove(g_build_filename(config_home, "vellum", "ai-completion.ini", NULL));
    g_rmdir(g_build_filename(config_home, "vellum", NULL));
    g_rmdir(config_home);
    g_free(config_home);
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    /* configure 测试需要 GTK 已初始化。 */
    gtk_init();
    g_test_add_func("/vellum/ai-completion/error-detail-null-safe", test_error_detail_is_null_safe);
    g_test_add_func("/vellum/ai-completion/extract-empty-malformed", test_extract_content_handles_empty_and_malformed);
    g_test_add_func("/vellum/ai-completion/extract-shapes", test_extract_content_parses_supported_shapes);
    g_test_add_func("/vellum/ai-completion/inline-candidate-normalization", test_inline_candidate_normalization);
    g_test_add_func("/vellum/ai-completion/endpoint-normalization", test_endpoint_normalization);
    g_test_add_func("/vellum/ai-completion/body-prefix-suffix", test_build_body_uses_prefix_and_suffix);
    g_test_add_func("/vellum/ai-completion/outdent-adjustment", test_outdent_adjustment);
    g_test_add_func("/vellum/ai-completion/trim-suffix-overlap", test_trim_suffix_overlap);
    g_test_add_func("/vellum/ai-completion/adjust-candidate", test_adjust_candidate_combined);
    g_test_add_func("/vellum/ai-completion/sse-payload-delta", test_sse_payload_delta);
    g_test_add_func("/vellum/ai-completion/disabled-languages-list", test_disabled_languages_list);
    g_test_add_func("/vellum/ai-completion/document-allowed-gating", test_document_allowed_gating);
    g_test_add_func("/vellum/ai-completion/disabled-from-items", test_disabled_from_items);
    g_test_add_func("/vellum/ai-completion/language-category", test_language_category_classification);
    g_test_add_func("/vellum/ai-completion/set-category-enabled", test_set_category_enabled);
    g_test_add_func("/vellum/ai-completion/category-toggle-syncs-rows", test_category_toggle_syncs_rows);
    g_test_add_func("/vellum/ai-completion/summary-auto-enabled", test_summary_auto_enabled_gating);
    g_test_add_func("/vellum/ai-completion/configure-builds-page", test_configure_builds_page);

    return g_test_run();
}
