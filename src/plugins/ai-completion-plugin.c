/*
 * ai-completion-plugin.c
 * 通用 OpenAI 兼容 AI 补全扩展。密钥仅写入当前用户 0600 权限的本地配置文件。
 *
 * 行为对齐真实补全工具（copilot.vim / Continue）：
 * - SSE 流式输出：候选文本随 token 到达渐进显示，而不是等整段返回；
 * - 多行候选：预览仅显示首行（宿主 overlay 为单行），Tab 接受完整多行补全；
 * - 接受时裁掉补全结尾与光标后已存在文本重叠的部分（Continue 的 SuffixOverlap），
 *   避免把文件里已有的内容重复一遍；
 * - 当前行只输入了空白时，裁掉补全开头与已输入空白重复的缩进（copilot.vim
 *   的 SuggestionTextWithAdjustments）；刚回车时空行保留补全自带的缩进；
 * - 输入停顿约 350 ms 后自动请求（Continue 默认值），期间继续输入会取消旧请求。
 *
 * 保留的独创设计：AI 代码总结（全文摘要随请求复用）、手动快捷键、费用警告、
 * 仅代码文档自动触发、拒绝后短暂抑制与端点归一化。
 */

#include "mt-plugin.h"
#include "ai-code-summary.h"

#include <adwaita.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <gmodule.h>
#include <gtksourceview/gtksource.h>
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#include <string.h>

#define AI_COMPLETION_GROUP "AI Completion"
#define AI_CONTEXT_LIMIT 8000
#define AI_SUFFIX_LIMIT 2000
/* 输入停顿后自动补全的防抖时长。Continue 默认 350 ms、copilot.vim 默认 45 ms；
 * 这里取 350 ms，在响应速度与 token 消耗之间折中。 */
#define AI_AUTO_DELAY_MILLISECONDS 350
/* 候选在屏幕上停留的最长时间；超时后自动消失，避免“鬼影长存”。 */
#define AI_CANDIDATE_TIMEOUT_SECONDS 15
/* 用户用其他文本拒绝候选后，这段时间内不再自动弹出补全。 */
#define AI_REJECT_SUPPRESS_MILLISECONDS 2000
/* 流式响应单次读取与原始缓冲上限；max_tokens 很小，超限说明服务端异常。 */
#define AI_STREAM_READ_SIZE 4096
#define AI_STREAM_BUFFER_LIMIT 262144

typedef struct _AiRequest AiRequest;
typedef struct _AiConfigWidgets AiConfigWidgets;
typedef struct _AiLanguageItem AiLanguageItem;

/* 文档类型的大类：表格按此分组，分隔行上显示类名，可整类开关。 */
typedef enum
{
    AI_LANGUAGE_CATEGORY_CODE,
    AI_LANGUAGE_CATEGORY_WEB,
    AI_LANGUAGE_CATEGORY_CONFIG,
    AI_LANGUAGE_CATEGORY_TEXT,
    AI_LANGUAGE_CATEGORY_OTHER,
    AI_LANGUAGE_CATEGORY_COUNT
} AiLanguageCategory;

G_DECLARE_FINAL_TYPE(AiLanguageItem, ai_language_item, AI, LANGUAGE_ITEM, GObject)

/* 文档类型表格的一行：方形复选框（启用）、文件类型名、备注（扩展名等）。
 * is_category_row 为 TRUE 时是分类分隔行（大类名 + 整类开关）。 */
struct _AiLanguageItem
{
    GObject parent_instance;
    gchar *id;
    gchar *name;
    gchar *remark;
    gboolean enabled;
    gint category;
    gboolean is_category_row;
    gboolean partial;
};

/* 此版本 G_DECLARE_FINAL_TYPE 不生成 *_TYPE_* 宏，手动补上。 */
#define AI_TYPE_LANGUAGE_ITEM (ai_language_item_get_type ())

G_DEFINE_TYPE(AiLanguageItem, ai_language_item, G_TYPE_OBJECT)

static void ai_completion_cost_warning_response(AdwMessageDialog *dialog,
                                                const gchar *response,
                                                gpointer user_data);
static void ai_completion_config_save_clicked(GtkButton *button, gpointer user_data);

struct _AiRequest
{
    MtPluginHost *host;
    SoupMessage *message;
    GInputStream *stream;
    GString *buffer;
    guchar read_buffer[AI_STREAM_READ_SIZE];
    gchar *context;
    gchar *suffix;
    gchar *completion;
    guint generation;
    gboolean automatic;
    gboolean streaming;
    gboolean error_mode;
    guint http_status;
};

struct _AiCandidate
{
    MtPluginHost *host;
    gchar *text;
    gchar *context;
    gchar *suffix;
};

struct _AiConfigWidgets
{
    MtPluginHost *host;
    AdwEntryRow *endpoint_row;
    AdwEntryRow *model_row;
    AdwPasswordEntryRow *key_row;
    AdwSwitchRow *auto_row;
    GListStore *language_items;
    AiCodeSummaryConfigWidgets *summary_widgets;
    GtkWindow *window;
};

static const MtPluginInfo ai_completion_plugin_info = {
    MT_PLUGIN_API_VERSION,
    "io.github.vellum.ai-completion",
    "AI Completion",
    "Complete text through a user-configured OpenAI-compatible API",
    "0.3.0"
};

static SoupSession *ai_session;
static struct _AiCandidate ai_candidate;
/* 每次请求或停用都会推进代际，过期回调绝不能再访问宿主。 */
static guint ai_generation;
/* 输入停顿后自动补全的防抖源；停用插件时必须移除。 */
static guint ai_auto_source_id;
/* 候选显示的自动过期计时器；任何清除候选的路径都必须移除它。 */
static guint ai_candidate_timeout_source_id;
/* 用户拒绝候选的时刻（单调时钟微秒），用于抑制紧接着的自动补全。 */
static gint64 ai_reject_until_us;
static gboolean ai_auto_context_loaded;
static gchar *ai_auto_context;
static gboolean ai_request_in_flight;
/* 输入停顿后自动补全（无需快捷键）；由“偏好设置”中的开关控制并持久化。 */
static gboolean ai_auto_enabled = TRUE;
/* 缓存“禁用补全的文档类型”集合：插件激活与设置保存时从 ini 重建。 */
static GHashTable *ai_disabled_languages;

static gchar *
ai_completion_config_path(void)
{
    gchar *directory;
    gchar *path;

    directory = g_build_filename(g_get_user_config_dir(), "vellum", NULL);
    g_mkdir_with_parents(directory, 0700);
    path = g_build_filename(directory, "ai-completion.ini", NULL);
    g_free(directory);

    return path;
}

static GKeyFile *
ai_completion_load_settings(void)
{
    GKeyFile *settings;
    gchar *path;

    settings = g_key_file_new();
    path = ai_completion_config_path();
    g_key_file_load_from_file(settings, path, G_KEY_FILE_NONE, NULL);
    g_free(path);

    return settings;
}

static gchar *
ai_completion_get_setting(GKeyFile *settings, const gchar *key)
{
    gchar *value;

    value = g_key_file_get_string(settings, AI_COMPLETION_GROUP, key, NULL);
    if (value == NULL)
    {
        value = g_strdup("");
    }

    return value;
}

static gboolean
ai_completion_get_auto_enabled(GKeyFile *settings)
{
    GError *error;
    gboolean value;

    error = NULL;
    value = g_key_file_get_boolean(settings, AI_COMPLETION_GROUP, "auto-complete", &error);
    if (error != NULL)
    {
        g_clear_error(&error);
        return TRUE;
    }

    return value;
}

static void
ai_completion_set_auto_enabled(gboolean enabled)
{
    GKeyFile *settings;
    gchar *path;
    gchar *contents;

    settings = ai_completion_load_settings();
    g_key_file_set_boolean(settings, AI_COMPLETION_GROUP, "auto-complete", enabled);
    contents = g_key_file_to_data(settings, NULL, NULL);
    if (contents != NULL)
    {
        path = ai_completion_config_path();
        if (g_file_set_contents(path, contents, (gssize)strlen(contents), NULL))
        {
            g_chmod(path, 0600);
        }
        g_free(path);
        g_free(contents);
    }
    g_key_file_unref(settings);
}

/* 解析 “disabled-languages” 的分号分隔列表；空值或缺失表示全部启用。 */
static GHashTable *
ai_completion_parse_disabled_languages(const gchar *value)
{
    GHashTable *disabled;
    gchar **parts;
    gchar **part;

    disabled = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    if (value == NULL || *value == '\0')
    {
        return disabled;
    }
    parts = g_strsplit(value, ";", -1);
    for (part = parts; part != NULL && *part != NULL; part++)
    {
        gchar *trimmed;

        trimmed = g_strdup(*part);
        g_strstrip(trimmed);
        if (*trimmed != '\0')
        {
            g_hash_table_add(disabled, trimmed);
        }
        else
        {
            g_free(trimmed);
        }
    }
    g_strfreev(parts);

    return disabled;
}

/* 把禁用集合序列化为分号分隔的字符串（按 id 排序，便于比较与存储）。 */
static gchar *
ai_completion_join_disabled_languages(GHashTable *disabled)
{
    GString *joined;
    GList *ids;
    GList *iter;

    if (disabled == NULL)
    {
        return g_strdup("");
    }
    joined = g_string_new(NULL);
    ids = g_list_sort(g_hash_table_get_keys(disabled), (GCompareFunc)g_strcmp0);
    for (iter = ids; iter != NULL; iter = iter->next)
    {
        if (joined->len > 0)
        {
            g_string_append_c(joined, ';');
        }
        g_string_append(joined, iter->data);
    }
    g_list_free(ids);

    return g_string_free(joined, FALSE);
}

static void
ai_completion_languages_set(GHashTable *disabled)
{
    g_clear_pointer(&ai_disabled_languages, g_hash_table_unref);
    ai_disabled_languages = disabled;
}

/* 从设置重建禁用集合缓存；插件激活与配置保存后调用。 */
static void
ai_completion_languages_load(GKeyFile *settings)
{
    gchar *value;

    value = ai_completion_get_setting(settings, "disabled-languages");
    ai_completion_languages_set(ai_completion_parse_disabled_languages(value));
    g_free(value);
}

static gboolean
ai_completion_language_is_disabled(const gchar *language_id)
{
    return language_id != NULL && ai_disabled_languages != NULL &&
           g_hash_table_contains(ai_disabled_languages, language_id);
}

/* 当前文档是否允许 AI 补全：语言已知时按设置的类型开关判断；
 * 语言未知（纯文本或系统未装语言定义）时，自动补全仍只服务代码文档，
 * 手动请求保持可用，与历史行为一致。 */
static gboolean
ai_completion_document_allowed(MtPluginHost *host, gboolean automatic)
{
    const gchar *language_id;

    if (host->get_document_language_id != NULL)
    {
        language_id = host->get_document_language_id(host);
        if (language_id != NULL && *language_id != '\0')
        {
            return !ai_completion_language_is_disabled(language_id);
        }
        return !automatic;
    }
    if (host->get_is_code_document != NULL && automatic)
    {
        return host->get_is_code_document(host);
    }

    return TRUE;
}

enum
{
    AI_LANGUAGE_ITEM_PROP_ID = 1,
    AI_LANGUAGE_ITEM_PROP_NAME,
    AI_LANGUAGE_ITEM_PROP_REMARK,
    AI_LANGUAGE_ITEM_PROP_ENABLED,
    AI_LANGUAGE_ITEM_N_PROPS
};

static GParamSpec *ai_language_item_properties[AI_LANGUAGE_ITEM_N_PROPS];

static void
ai_language_item_set_property(GObject *object,
                              guint property_id,
                              const GValue *value,
                              GParamSpec *pspec)
{
    AiLanguageItem *item;

    item = AI_LANGUAGE_ITEM(object);
    switch (property_id)
    {
    case AI_LANGUAGE_ITEM_PROP_ID:
        g_free(item->id);
        item->id = g_value_dup_string(value);
        break;
    case AI_LANGUAGE_ITEM_PROP_NAME:
        g_free(item->name);
        item->name = g_value_dup_string(value);
        break;
    case AI_LANGUAGE_ITEM_PROP_REMARK:
        g_free(item->remark);
        item->remark = g_value_dup_string(value);
        break;
    case AI_LANGUAGE_ITEM_PROP_ENABLED:
        item->enabled = g_value_get_boolean(value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
        break;
    }
}

static void
ai_language_item_get_property(GObject *object,
                              guint property_id,
                              GValue *value,
                              GParamSpec *pspec)
{
    AiLanguageItem *item;

    item = AI_LANGUAGE_ITEM(object);
    switch (property_id)
    {
    case AI_LANGUAGE_ITEM_PROP_ID:
        g_value_set_string(value, item->id);
        break;
    case AI_LANGUAGE_ITEM_PROP_NAME:
        g_value_set_string(value, item->name);
        break;
    case AI_LANGUAGE_ITEM_PROP_REMARK:
        g_value_set_string(value, item->remark);
        break;
    case AI_LANGUAGE_ITEM_PROP_ENABLED:
        g_value_set_boolean(value, item->enabled);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
        break;
    }
}

static void
ai_language_item_finalize(GObject *object)
{
    AiLanguageItem *item;

    item = AI_LANGUAGE_ITEM(object);
    g_free(item->id);
    g_free(item->name);
    g_free(item->remark);

    G_OBJECT_CLASS(ai_language_item_parent_class)->finalize(object);
}

static void
ai_language_item_class_init(AiLanguageItemClass *klass)
{
    GObjectClass *object_class;

    object_class = G_OBJECT_CLASS(klass);
    object_class->set_property = ai_language_item_set_property;
    object_class->get_property = ai_language_item_get_property;
    object_class->finalize = ai_language_item_finalize;

    ai_language_item_properties[AI_LANGUAGE_ITEM_PROP_ID] =
        g_param_spec_string("language-id", "Language id", "GtkSourceView language id",
                            NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    ai_language_item_properties[AI_LANGUAGE_ITEM_PROP_NAME] =
        g_param_spec_string("name", "Name", "Localized language name",
                            NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    ai_language_item_properties[AI_LANGUAGE_ITEM_PROP_REMARK] =
        g_param_spec_string("remark", "Remark", "File extensions or mime types",
                            NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    ai_language_item_properties[AI_LANGUAGE_ITEM_PROP_ENABLED] =
        g_param_spec_boolean("enabled", "Enabled", "Whether completion is enabled",
                             TRUE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    g_object_class_install_properties(object_class,
                                      AI_LANGUAGE_ITEM_N_PROPS,
                                      ai_language_item_properties);
}

static void
ai_language_item_init(AiLanguageItem *item)
{
    (void)item;
}

static AiLanguageItem *
ai_language_item_new(const gchar *id, const gchar *name, const gchar *remark, gboolean enabled)
{
    return g_object_new(AI_TYPE_LANGUAGE_ITEM,
                        "language-id", id,
                        "name", name,
                        "remark", remark,
                        "enabled", enabled,
                        NULL);
}

static const gchar *
ai_completion_category_name(gint category)
{
    switch (category)
    {
    case AI_LANGUAGE_CATEGORY_CODE:
        return _("Code");
    case AI_LANGUAGE_CATEGORY_WEB:
        return _("Web");
    case AI_LANGUAGE_CATEGORY_CONFIG:
        return _("Config");
    case AI_LANGUAGE_CATEGORY_TEXT:
        return _("Text");
    default:
        return _("Other");
    }
}

/* 把 GtkSourceView 语言归入大类，便于整类开启/关闭。
 * 归类只是粗略约定，未匹配的语言落入“其他”。 */
static gint
ai_completion_language_category(const gchar *id)
{
    static const gchar * const code[] = {
        "ada", "ansforth94", "awk", "bennugd", "bluespec", "boo", "c", "cg", "chdr",
        "cobol", "commonlisp", "cpp", "cpphdr", "c-sharp", "cuda", "d", "dart", "dtl",
        "eiffel", "elixir", "erlang", "fcl", "fish", "forth", "fortran", "fsharp",
        "gap", "gdscript", "genie", "glsl", "go", "gradle", "groovy", "haskell",
        "haskell-literate", "haxe", "idl", "idl-exelis", "imagej", "j", "java",
        "julia", "kotlin", "lean", "lex", "llvm", "logtalk", "lua", "m4", "makefile",
        "automake", "matlab", "maxima", "meson", "modelica", "nemerle", "netrexx",
        "nix", "nsis", "objc", "objj", "ocaml", "ocl", "octave", "ooc", "opal",
        "opencl", "pascal", "perl", "php", "pig", "powershell", "prolog", "proto",
        "python", "python3", "r", "reasonml", "rpmspec", "ruby", "rust", "scala",
        "scheme", "scilab", "sh", "sml", "solidity", "sparql", "sql", "star",
        "swift", "systemverilog", "tcl", "tera", "terraform", "thrift", "vala",
        "vbnet", "verilog", "vhdl", "wren", "yacc", "yara", "zig", "actionscript",
        "cmake"
    };
    static const gchar * const web[] = {
        "html", "css", "scss", "less", "js", "js-expr", "js-fn", "js-lit", "js-mod",
        "js-st", "js-val", "jsx", "typescript", "typescript-js-expr",
        "typescript-js-fn", "typescript-js-lit", "typescript-js-mod",
        "typescript-js-st", "typescript-jsx", "typescript-type-expr",
        "typescript-type-gen", "typescript-type-lit", "astro", "asp", "erb",
        "erb-html", "erb-js", "ftl", "jade", "mxml", "twig"
    };
    static const gchar * const config[] = {
        "json", "yaml", "csv", "ini", "desktop", "docker", "dpatch", "gtkrc",
        "pkgconfig", "toml", "xml", "xslt", "dtd", "def", "gdb-log", "logcat",
        "abnf", "blueprint", "xkb"
    };
    static const gchar * const text[] = {
        "asciidoc", "bibtex", "changelog", "diff", "docbook", "groff", "gtk-doc",
        "haddock", "latex", "mallard", "markdown", "mediawiki", "rst", "texinfo",
        "t2t", "todotxt", "sweave", "gettext-translation"
    };

    if (id == NULL || *id == '\0')
    {
        return AI_LANGUAGE_CATEGORY_OTHER;
    }
    if (g_strv_contains(code, id))
    {
        return AI_LANGUAGE_CATEGORY_CODE;
    }
    if (g_strv_contains(web, id))
    {
        return AI_LANGUAGE_CATEGORY_WEB;
    }
    if (g_strv_contains(config, id))
    {
        return AI_LANGUAGE_CATEGORY_CONFIG;
    }
    if (g_strv_contains(text, id))
    {
        return AI_LANGUAGE_CATEGORY_TEXT;
    }

    return AI_LANGUAGE_CATEGORY_OTHER;
}

/* 备注列：语言定义未提供 globs 时，用前几个 MIME 类型代替。 */
static gchar *
ai_completion_language_remark_from_mimes(GtkSourceLanguage *language)
{
    gchar **mimes;
    GString *joined;
    gint total;
    gint index;

    mimes = gtk_source_language_get_mime_types(language);
    if (mimes == NULL || mimes[0] == NULL)
    {
        g_strfreev(mimes);
        return NULL;
    }
    joined = g_string_new(NULL);
    for (index = 0; mimes[index] != NULL && index < 3; index++)
    {
        if (joined->len > 0)
        {
            g_string_append_c(joined, ',');
            g_string_append_c(joined, ' ');
        }
        g_string_append(joined, mimes[index]);
    }
    total = 0;
    while (mimes[total] != NULL)
    {
        total++;
    }
    if (total > 3)
    {
        g_string_append(joined, ", …");
    }
    g_strfreev(mimes);

    return g_string_free(joined, FALSE);
}

/* —— 表格工厂回调：全部使用原生 GTK4 组件，行内不引入其他 UI 架构。 —— */

static void ai_completion_check_toggled(GtkCheckButton *check, gpointer user_data);

static void
ai_completion_check_factory_setup(GtkSignalListItemFactory *factory,
                                  gpointer list_item,
                                  gpointer user_data)
{
    GtkCheckButton *check;

    (void)factory;
    (void)user_data;
    check = GTK_CHECK_BUTTON(gtk_check_button_new());
    gtk_widget_set_margin_start(GTK_WIDGET(check), 10);
    gtk_widget_set_margin_end(GTK_WIDGET(check), 4);
    gtk_widget_set_halign(GTK_WIDGET(check), GTK_ALIGN_START);
    g_signal_connect(check, "toggled", G_CALLBACK(ai_completion_check_toggled), NULL);
    gtk_list_item_set_child(GTK_LIST_ITEM(list_item), GTK_WIDGET(check));
}

static void
ai_completion_check_factory_bind(GtkSignalListItemFactory *factory,
                                 gpointer list_item,
                                 gpointer user_data)
{
    GtkCheckButton *check;
    AiLanguageItem *item;
    GBinding *binding;

    (void)factory;
    (void)user_data;
    item = gtk_list_item_get_item(GTK_LIST_ITEM(list_item));
    check = GTK_CHECK_BUTTON(gtk_list_item_get_child(GTK_LIST_ITEM(list_item)));
    g_object_set_data_full(G_OBJECT(check), "vellum-ai-item",
                           g_object_ref(item), g_object_unref);
    /* 双向绑定：行复选框与模型 enabled 保持同步，大类开关才能联动所有行。 */
    binding = g_object_bind_property(item, "enabled", check, "active",
                                     G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);
    g_object_set_data_full(G_OBJECT(check), "vellum-ai-binding", binding, g_object_unref);
}

static void
ai_completion_check_factory_unbind(GtkSignalListItemFactory *factory,
                                   gpointer list_item,
                                   gpointer user_data)
{
    GtkWidget *child;
    GBinding *binding;

    (void)factory;
    (void)user_data;
    child = gtk_list_item_get_child(GTK_LIST_ITEM(list_item));
    if (child == NULL)
    {
        return;
    }
    /* 先持引用再清理：GBinding 强引用 child，且 g_binding_unbind 会消费
     * 绑定引用（GLib ≥ 2.88 语义），直接清空数据槽即可让绑定随 unref
     * 自动解除；child 可能因此被销毁，所以期间持有自己的引用。 */
    g_object_ref(child);
    g_object_set_data(G_OBJECT(child), "vellum-ai-binding", NULL);
    g_object_set_data(G_OBJECT(child), "vellum-ai-item", NULL);
    g_object_unref(child);
}

/* 大类开关：把该大类下所有语言行设为一致状态。
 * 必须经 GObject 属性写入（而非直接改字段），双向绑定才能收到通知、
 * 同步下方表格行的复选框。 */
static void
ai_completion_set_category_enabled(GListStore *items, gint category, gboolean enabled)
{
    guint index;

    if (items == NULL)
    {
        return;
    }
    for (index = 0; index < g_list_model_get_n_items(G_LIST_MODEL(items)); index++)
    {
        AiLanguageItem *item;

        item = g_list_model_get_item(G_LIST_MODEL(items), index);
        if (item != NULL)
        {
            if (!item->is_category_row && item->category == category)
            {
                g_object_set(item, "enabled", enabled, NULL);
            }
            g_object_unref(item);
        }
    }
}

static void
ai_completion_category_toggled(GtkCheckButton *check, gpointer user_data)
{
    gint category;
    GListStore *store;

    (void)user_data;
    category = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(check), "vellum-ai-category"));
    store = g_object_get_data(G_OBJECT(check), "vellum-ai-store");
    ai_completion_set_category_enabled(store, category,
                                       gtk_check_button_get_active(check));
}

static void
ai_completion_check_toggled(GtkCheckButton *check, gpointer user_data)
{
    AiLanguageItem *item;
    GtkWidget *table;
    GtkSelectionModel *selection;
    GListModel *model;
    guint category;
    guint enabled_count;
    guint row_count;
    guint index;
    GHashTable *checks;

    (void)user_data;
    item = g_object_get_data(G_OBJECT(check), "vellum-ai-item");
    if (item == NULL)
    {
        return;
    }
    g_object_set(item, "enabled", gtk_check_button_get_active(check), NULL);

    /* 单行切换后刷新所属大类的开关状态（全开/全关/部分勾选）。 */
    table = gtk_widget_get_ancestor(GTK_WIDGET(check), GTK_TYPE_COLUMN_VIEW);
    if (table == NULL)
    {
        return;
    }
    selection = gtk_column_view_get_model(GTK_COLUMN_VIEW(table));
    model = gtk_single_selection_get_model(GTK_SINGLE_SELECTION(selection));
    if (model == NULL)
    {
        return;
    }
    category = (guint)item->category;
    enabled_count = 0;
    row_count = 0;
    for (index = 0; index < g_list_model_get_n_items(model); index++)
    {
        AiLanguageItem *other;

        other = g_list_model_get_item(model, index);
        if (other != NULL)
        {
            if (!other->is_category_row && other->category == (gint)category)
            {
                row_count++;
                if (other->enabled)
                {
                    enabled_count++;
                }
            }
            g_object_unref(other);
        }
    }
    checks = g_object_get_data(G_OBJECT(table), "vellum-ai-category-checks");
    if (checks != NULL)
    {
        GtkCheckButton *category_check;

        category_check = g_hash_table_lookup(checks, GINT_TO_POINTER(category));
        if (category_check != NULL)
        {
            /* 屏蔽大类处理器再写状态：整类批量更新途中会多次刷新，
             * 若每次都触发大类 toggled，会把尚未更新的行改回去（递归打架）。 */
            g_signal_handlers_block_by_func(category_check,
                                            ai_completion_category_toggled,
                                            NULL);
            gtk_check_button_set_inconsistent(category_check,
                                              enabled_count > 0 &&
                                              enabled_count < row_count);
            gtk_check_button_set_active(category_check,
                                        enabled_count > 0 &&
                                        enabled_count == row_count);
            g_signal_handlers_unblock_by_func(category_check,
                                              ai_completion_category_toggled,
                                              NULL);
        }
    }
}

static void
ai_completion_label_factory_setup(GtkSignalListItemFactory *factory,
                                  gpointer list_item,
                                  gpointer user_data)
{
    GtkLabel *label;

    (void)factory;
    (void)user_data;
    label = GTK_LABEL(gtk_label_new(NULL));
    gtk_label_set_xalign(label, 0.0f);
    gtk_widget_set_margin_start(GTK_WIDGET(label), 4);
    gtk_widget_set_margin_end(GTK_WIDGET(label), 8);
    gtk_label_set_ellipsize(label, PANGO_ELLIPSIZE_END);
    gtk_list_item_set_child(GTK_LIST_ITEM(list_item), GTK_WIDGET(label));
}

static void
ai_completion_name_factory_bind(GtkSignalListItemFactory *factory,
                                gpointer list_item,
                                gpointer user_data)
{
    AiLanguageItem *item;
    GtkLabel *label;

    (void)factory;
    (void)user_data;
    item = gtk_list_item_get_item(GTK_LIST_ITEM(list_item));
    label = GTK_LABEL(gtk_list_item_get_child(GTK_LIST_ITEM(list_item)));
    gtk_label_set_text(label, item->name);
}

static void
ai_completion_remark_factory_bind(GtkSignalListItemFactory *factory,
                                  gpointer list_item,
                                  gpointer user_data)
{
    AiLanguageItem *item;
    GtkLabel *label;

    (void)factory;
    (void)user_data;
    item = gtk_list_item_get_item(GTK_LIST_ITEM(list_item));
    label = GTK_LABEL(gtk_list_item_get_child(GTK_LIST_ITEM(list_item)));
    gtk_label_set_text(label, item->remark);
}

static void
ai_completion_label_factory_unbind(GtkSignalListItemFactory *factory,
                                   gpointer list_item,
                                   gpointer user_data)
{
    (void)factory;
    (void)list_item;
    (void)user_data;
}

static gboolean
ai_completion_pref_auto_get(gpointer user_data)
{
    (void)user_data;

    return ai_auto_enabled;
}

static void
ai_completion_pref_auto_set(gboolean value, gpointer user_data)
{
    (void)user_data;

    ai_auto_enabled = value;
    ai_completion_set_auto_enabled(value);
}

static gchar *
ai_completion_normalize_endpoint(const gchar *endpoint)
{
    gchar *normalized;
    GUri *uri;
    const gchar *path;
    gsize length;

    normalized = g_strdup(endpoint != NULL ? endpoint : "");
    g_strstrip(normalized);

    /* 统一去掉末尾斜杠，便于后续后缀匹配。 */
    length = strlen(normalized);
    while (length > 1 && normalized[length - 1] == '/')
    {
        normalized[--length] = '\0';
    }

    if (g_str_has_suffix(normalized, "/v1"))
    {
        gchar *with_path;

        with_path = g_strconcat(normalized, "/chat/completions", NULL);
        g_free(normalized);
        return with_path;
    }
    if (g_str_has_suffix(normalized, "/chat/completions"))
    {
        return normalized;
    }

    /* 裸主机地址（如 https://api.deepseek.com 或 https://api.deepseek.com/）
     * 自动补上补全端点，与主流 OpenAI 兼容客户端一致。 */
    uri = g_uri_parse(normalized, G_URI_FLAGS_NONE, NULL);
    path = uri != NULL ? g_uri_get_path(uri) : NULL;
    if (uri != NULL && (path == NULL || *path == '\0' || g_str_equal(path, "/")))
    {
        gchar *with_path;

        with_path = g_strconcat(normalized, "/chat/completions", NULL);
        g_free(normalized);
        normalized = with_path;
    }
    if (uri != NULL)
    {
        g_uri_unref(uri);
    }

    return normalized;
}

/* 把表格里未勾选的文档类型收集为禁用集合（分类分隔行不参与）。 */
static GHashTable *
ai_completion_disabled_from_items(GListStore *items)
{
    GHashTable *disabled;
    guint index;

    disabled = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    if (items == NULL)
    {
        return disabled;
    }
    for (index = 0; index < g_list_model_get_n_items(G_LIST_MODEL(items)); index++)
    {
        AiLanguageItem *item;

        item = g_list_model_get_item(G_LIST_MODEL(items), index);
        if (item != NULL)
        {
            if (!item->is_category_row && !item->enabled)
            {
                g_hash_table_add(disabled, g_strdup(item->id));
            }
            g_object_unref(item);
        }
    }

    return disabled;
}

static gint
ai_completion_language_item_compare(gconstpointer a, gconstpointer b, gpointer user_data)
{
    const AiLanguageItem *item_a;
    const AiLanguageItem *item_b;

    (void)user_data;
    item_a = a;
    item_b = b;

    return g_utf8_collate(item_a->name, item_b->name);
}

static gboolean
ai_completion_save_settings(const gchar *endpoint,
                            const gchar *model,
                            const gchar *api_key,
                            gboolean auto_enabled,
                            GListStore *language_items,
                            AiCodeSummaryConfigWidgets *summary_widgets,
                            GError **error)
{
    GKeyFile *settings;
    gchar *path;
    gchar *contents;
    gsize length;
    gboolean saved;
    GHashTable *disabled_languages;
    gchar *disabled_value;

    gchar *normalized_endpoint;

    normalized_endpoint = ai_completion_normalize_endpoint(endpoint);
    disabled_languages = ai_completion_disabled_from_items(language_items);
    disabled_value = ai_completion_join_disabled_languages(disabled_languages);
    settings = g_key_file_new();
    g_key_file_set_string(settings, AI_COMPLETION_GROUP, "endpoint", normalized_endpoint);
    g_key_file_set_string(settings, AI_COMPLETION_GROUP, "model", model);
    g_key_file_set_string(settings, AI_COMPLETION_GROUP, "api-key", api_key);
    g_key_file_set_boolean(settings, AI_COMPLETION_GROUP, "auto-complete", auto_enabled);
    g_key_file_set_string(settings, AI_COMPLETION_GROUP, "disabled-languages", disabled_value);
    ai_code_summary_config_save(settings, summary_widgets);
    contents = g_key_file_to_data(settings, &length, error);

    if (contents == NULL)
    {
        g_free(disabled_value);
        g_hash_table_unref(disabled_languages);
        g_free(normalized_endpoint);
        g_key_file_unref(settings);
        return FALSE;
    }

    path = ai_completion_config_path();
    saved = g_file_set_contents(path, contents, (gssize)length, error);
    if (saved)
    {
        g_chmod(path, 0600);
        /* 让运行中的插件立即使用新选择，无需重启。 */
        ai_completion_languages_set(disabled_languages);
        disabled_languages = NULL;
    }

    g_free(disabled_value);
    if (disabled_languages != NULL)
    {
        g_hash_table_unref(disabled_languages);
    }
    g_free(path);
    g_free(contents);
    g_free(normalized_endpoint);
    g_key_file_unref(settings);

    return saved;
}

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

static void
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

static void
ai_completion_config_save_clicked(GtkButton *button, gpointer user_data)
{
    AiConfigWidgets *widgets;
    const gchar *endpoint;
    const gchar *model;
    const gchar *api_key;
    gboolean auto_enabled;
    GError *error;

    (void)button;

    widgets = user_data;
    endpoint = gtk_editable_get_text(GTK_EDITABLE(widgets->endpoint_row));
    model = gtk_editable_get_text(GTK_EDITABLE(widgets->model_row));
    api_key = gtk_editable_get_text(GTK_EDITABLE(widgets->key_row));
    auto_enabled = adw_switch_row_get_active(widgets->auto_row);
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

static gchar *
ai_completion_trim_context(const gchar *text, glong limit)
{
    glong characters;
    const gchar *start;

    characters = g_utf8_strlen(text, -1);
    if (characters <= limit)
    {
        return g_strdup(text);
    }

    start = g_utf8_offset_to_pointer(text, characters - limit);
    return g_strdup(start);
}

static gchar *
ai_completion_build_body(const gchar *model, const gchar *prefix, const gchar *suffix, const gchar *summary)
{
    JsonBuilder *builder;
    JsonGenerator *generator;
    JsonNode *root;
    gchar *body;
    gchar *prompt;

    if (suffix != NULL && *suffix != '\0')
    {
        /* 与真实补全工具一致的 fill-in-the-middle 思路：同时给出光标前后文，
         * 让模型只补中间段；聊天模型用标记符描述前缀与后缀。指令刻意强调
         * “不是对话”：聊天模型常把“你好”当开场白接一句问候。 */
        prompt = g_strdup_printf("This is not a chat. Continue the code or prose at the cursor. A local AI-maintained summary may appear before the code; treat it as context, not output. The text before the cursor is between <fim_prefix> and <fim_suffix>; the text after the cursor follows <fim_suffix>. Output only the missing middle that flows from the prefix toward the suffix. Never greet, never answer a question, never explain, never use Markdown or fences, never repeat text that already exists.\n\n<document_summary>\n%s\n</document_summary>\n<fim_prefix>\n%s\n<fim_suffix>\n%s\n<fim_middle>\n",
                                 summary != NULL ? summary : "",
                                 prefix,
                                 suffix);
    }
    else
    {
        prompt = g_strdup_printf("This is not a chat. Continue the code or prose at the cursor. A local AI-maintained summary may appear before the code; treat it as context, not output. Output only the characters that should immediately follow the cursor, as if the file keeps going. Never greet, never answer a question, never explain, never use Markdown or fences, never repeat text that already exists.\n\n<document_summary>\n%s\n</document_summary>\n%s",
                                 summary != NULL ? summary : "",
                                 prefix);
    }
    builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "model");
    json_builder_add_string_value(builder, model);
    json_builder_set_member_name(builder, "messages");
    json_builder_begin_array(builder);
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "role");
    json_builder_add_string_value(builder, "system");
    json_builder_set_member_name(builder, "content");
    json_builder_add_string_value(builder,
                                  "You are an inline text completion engine inside a text editor, not a chat assistant. "
                                  "Your only output is the immediate continuation at the cursor: raw characters, no dialogue, "
                                  "no greetings, no question answering, no commentary, no Markdown, no repetition.");
    json_builder_end_object(builder);
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "role");
    json_builder_add_string_value(builder, "user");
    json_builder_set_member_name(builder, "content");
    json_builder_add_string_value(builder, prompt);
    json_builder_end_object(builder);
    json_builder_end_array(builder);
    /* 内联补全需要短、快的正文，不请求默认开启的思考链。 */
    json_builder_set_member_name(builder, "thinking");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "disabled");
    json_builder_end_object(builder);
    json_builder_set_member_name(builder, "temperature");
    json_builder_add_double_value(builder, 0.2);
    json_builder_set_member_name(builder, "max_tokens");
    json_builder_add_int_value(builder, 160);
    /* 流式输出：候选随 token 到达渐进显示；不支持的兼容服务会忽略此字段，
     * 插件对非 text/event-stream 的响应按普通 JSON 回退解析。 */
    json_builder_set_member_name(builder, "stream");
    json_builder_add_boolean_value(builder, TRUE);
    json_builder_end_object(builder);

    root = json_builder_get_root(builder);
    generator = json_generator_new();
    json_generator_set_root(generator, root);
    body = json_generator_to_data(generator, NULL);

    json_node_free(root);
    g_object_unref(generator);
    g_object_unref(builder);
    g_free(prompt);

    return body;
}

static gchar *
ai_completion_extract_content_from_node(JsonNode *content_node)
{
    const gchar *content;

    content = NULL;
    if (content_node == NULL)
    {
        return NULL;
    }
    if (JSON_NODE_HOLDS_VALUE(content_node) &&
        json_node_get_value_type(content_node) == G_TYPE_STRING)
    {
        content = json_node_get_string(content_node);
    }
    else if (JSON_NODE_HOLDS_ARRAY(content_node))
    {
        /* 部分 OpenAI 兼容服务把 content 返回为 [{ "type": "text", "text": "..." }] */
        JsonArray *parts;
        guint part_index;

        parts = json_node_get_array(content_node);
        for (part_index = 0; part_index < json_array_get_length(parts); part_index++)
        {
            JsonNode *part_node;

            part_node = json_array_get_element(parts, part_index);
            if (!JSON_NODE_HOLDS_OBJECT(part_node))
            {
                continue;
            }
            if (json_object_has_member(json_node_get_object(part_node), "text"))
            {
                JsonNode *text_node;

                text_node = json_object_get_member(json_node_get_object(part_node), "text");
                if (JSON_NODE_HOLDS_VALUE(text_node) &&
                    json_node_get_value_type(text_node) == G_TYPE_STRING)
                {
                    content = json_node_get_string(text_node);
                    if (content != NULL && *content != '\0')
                    {
                        break;
                    }
                }
            }
        }
    }

    return content != NULL && *content != '\0' ? g_strdup(content) : NULL;
}

/* 从单个 choice 对象里取出补全文本；SSE 的 delta 与普通响应的 message 共用。 */
static gchar *
ai_completion_extract_choice_content(JsonObject *choice)
{
    JsonObject *message;
    JsonNode *content_node;
    gchar *content;

    content = NULL;
    if (choice == NULL)
    {
        return NULL;
    }
    if (json_object_has_member(choice, "message"))
    {
        message = json_object_get_object_member(choice, "message");
        if (message != NULL && json_object_has_member(message, "content"))
        {
            content_node = json_object_get_member(message, "content");
            content = ai_completion_extract_content_from_node(content_node);
        }
    }
    if (content == NULL && json_object_has_member(choice, "delta"))
    {
        message = json_object_get_object_member(choice, "delta");
        if (message != NULL && json_object_has_member(message, "content"))
        {
            content_node = json_object_get_member(message, "content");
            content = ai_completion_extract_content_from_node(content_node);
        }
    }
    if (content == NULL && json_object_has_member(choice, "text"))
    {
        content = ai_completion_extract_content_from_node(json_object_get_member(choice, "text"));
    }

    return content;
}

static gchar *
ai_completion_extract_content(const gchar *response, gsize length, GError **error)
{
    JsonParser *parser;
    JsonNode *root;
    JsonObject *object;
    JsonArray *choices;
    JsonObject *choice;
    gchar *content;
    gchar *result;

    if (response == NULL || length == 0)
    {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "AI response is empty");
        return NULL;
    }

    parser = json_parser_new();
    if (!json_parser_load_from_data(parser, response, (gssize)length, error))
    {
        g_object_unref(parser);
        return NULL;
    }

    root = json_parser_get_root(parser);
    if (!JSON_NODE_HOLDS_OBJECT(root))
    {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "AI response is not a JSON object");
        g_object_unref(parser);
        return NULL;
    }

    object = json_node_get_object(root);
    if (!json_object_has_member(object, "choices"))
    {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "AI response has no choices array");
        g_object_unref(parser);
        return NULL;
    }

    choices = json_object_get_array_member(object, "choices");
    if (json_array_get_length(choices) == 0)
    {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "AI response has an empty choices array");
        g_object_unref(parser);
        return NULL;
    }

    choice = json_array_get_object_element(choices, 0);
    content = ai_completion_extract_choice_content(choice);
    if (content == NULL)
    {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "AI response contains no completion text");
        g_object_unref(parser);
        return NULL;
    }

    result = content;
    g_object_unref(parser);

    return result;
}

static gchar *
ai_completion_error_detail_data(const gchar *data, gsize length)
{
    if (data == NULL || length == 0)
    {
        return g_strdup("");
    }

    return g_strndup(data, MIN(length, (gsize)160));
}

/* 预览首行：宿主 overlay 是光标后的单行标签，多行补全只能显示第一行。
 * 跳过开头的空行，直到找到第一条非空逻辑行。 */
static gchar *
ai_completion_prepare_inline_candidate(const gchar *completion)
{
    const gchar *start;
    const gchar *end;

    if (completion == NULL)
    {
        return NULL;
    }

    start = completion;
    while (*start == '\r' || *start == '\n')
    {
        start++;
    }
    end = strpbrk(start, "\r\n");
    if (end == NULL)
    {
        end = start + strlen(start);
    }
    if (end == start)
    {
        return NULL;
    }

    return g_strndup(start, (gsize)(end - start));
}

/* 当前行（最后一个换行之后）只输入了空白时，补全开头的空白与已输入空白
 * 重叠的部分会被裁掉，避免 Tab 接受后把缩进重复一遍。刚回车（当前行为空）
 * 时缩进来自补全本身，不做裁剪——与 copilot.vim 的缩进调整一致。 */
static gchar *
ai_completion_outdent(const gchar *completion, const gchar *context)
{
    const gchar *line_start;
    const gchar *walk;
    const gchar *leading_start;
    gsize leading_length;
    gsize typed_length;

    if (completion == NULL)
    {
        return NULL;
    }
    if (context == NULL)
    {
        return g_strdup(completion);
    }

    line_start = strrchr(context, '\n');
    line_start = line_start != NULL ? line_start + 1 : context;
    if (*line_start == '\0')
    {
        return g_strdup(completion);
    }
    for (walk = line_start; *walk != '\0'; walk++)
    {
        if (*walk != ' ' && *walk != '\t')
        {
            return g_strdup(completion);
        }
    }

    leading_start = completion;
    while (*leading_start == ' ' || *leading_start == '\t')
    {
        leading_start++;
    }
    leading_length = (gsize)(leading_start - completion);
    if (leading_length == 0)
    {
        return g_strdup(completion);
    }

    typed_length = strlen(line_start);
    if (typed_length < leading_length)
    {
        return g_strdup(completion);
    }
    if (strncmp(line_start, completion, leading_length) != 0)
    {
        return g_strdup(completion);
    }

    return g_strdup(completion + leading_length);
}

/* 接受时应用 Continue 的 SuffixOverlap：裁掉补全结尾与光标后已存在文本
 * 开头重叠的部分，避免接受后把已有内容重复一遍。 */
static gchar *
ai_completion_trim_suffix_overlap(const gchar *completion, const gchar *suffix)
{
    gsize completion_length;
    gsize suffix_length;
    gsize largest_overlap;
    gsize i;

    if (completion == NULL)
    {
        return NULL;
    }
    if (suffix == NULL || *suffix == '\0')
    {
        return g_strdup(completion);
    }

    completion_length = strlen(completion);
    suffix_length = strlen(suffix);
    largest_overlap = 0;
    for (i = 1; i <= MIN(completion_length, suffix_length); i++)
    {
        if (strncmp(completion + completion_length - i, suffix, i) == 0)
        {
            largest_overlap = i;
        }
    }
    if (largest_overlap == 0)
    {
        return g_strdup(completion);
    }

    return g_strndup(completion, completion_length - largest_overlap);
}

/* 候选最终文本：出缩进 + 去尾部换行 + 后缀去重。展示与接受使用同一份文本，
 * 保证“看到什么就插入什么”。 */
static gchar *
ai_completion_adjust_candidate(const gchar *completion,
                               const gchar *context,
                               const gchar *suffix)
{
    gchar *outdented;
    gchar *stripped;
    gchar *trimmed;
    gsize length;

    outdented = ai_completion_outdent(completion, context);
    stripped = g_strdup(outdented != NULL ? outdented : "");
    g_free(outdented);
    length = strlen(stripped);
    while (length > 0 && (stripped[length - 1] == '\n' || stripped[length - 1] == '\r'))
    {
        stripped[--length] = '\0';
    }
    trimmed = ai_completion_trim_suffix_overlap(stripped, suffix);
    g_free(stripped);

    return trimmed;
}

static void
ai_completion_clear_candidate(void)
{
    if (ai_candidate_timeout_source_id != 0)
    {
        g_source_remove(ai_candidate_timeout_source_id);
        ai_candidate_timeout_source_id = 0;
    }
    if (ai_candidate.host != NULL && ai_candidate.host->clear_inline_completion != NULL)
    {
        ai_candidate.host->clear_inline_completion(ai_candidate.host);
    }
    g_clear_pointer(&ai_candidate.text, g_free);
    g_clear_pointer(&ai_candidate.context, g_free);
    g_clear_pointer(&ai_candidate.suffix, g_free);
    ai_candidate.host = NULL;
}

static void
ai_completion_reject_suppress(void)
{
    ai_reject_until_us = g_get_monotonic_time() +
                         AI_REJECT_SUPPRESS_MILLISECONDS * 1000;
}

static void
ai_completion_auto_cancel(void)
{
    if (ai_auto_source_id != 0)
    {
        g_source_remove(ai_auto_source_id);
        ai_auto_source_id = 0;
    }
}

static gboolean
ai_completion_candidate_timeout(gpointer user_data)
{
    MtPluginHost *host;

    host = user_data;
    ai_candidate_timeout_source_id = 0;
    if (ai_candidate.host == host)
    {
        ai_completion_clear_candidate();
    }
    return G_SOURCE_REMOVE;
}

/* 把累积的补全文本作为候选展示：只在光标前后文与请求时一致时生效，
 * 流式过程中每次文本增长都会调用，因此会不断刷新幽灵文本。 */
static void
ai_completion_show_candidate(AiRequest *request, const gchar *completion)
{
    gchar *adjusted;
    gchar *preview;
    gchar *current_context;
    gchar *current_suffix;
    gboolean contexts_match;

    adjusted = ai_completion_adjust_candidate(completion, request->context, request->suffix);
    preview = ai_completion_prepare_inline_candidate(adjusted);
    if (preview == NULL)
    {
        /* 补全以空行开头：首行还没有内容，继续累积，暂不显示。 */
        g_free(adjusted);
        return;
    }

    if (g_get_monotonic_time() < ai_reject_until_us)
    {
        /* 用户刚拒绝过候选（Escape 或继续输入）：迟到的流式响应不再弹回。 */
        g_free(adjusted);
        g_free(preview);
        return;
    }

    current_context = request->host->get_text_before_cursor(request->host);
    current_suffix = request->host->get_text_after_cursor != NULL ?
                     request->host->get_text_after_cursor(request->host) : g_strdup("");
    contexts_match = g_strcmp0(current_context, request->context) == 0 &&
                     g_strcmp0(current_suffix, request->suffix) == 0;
    g_free(current_context);
    g_free(current_suffix);
    if (!contexts_match)
    {
        g_free(adjusted);
        g_free(preview);
        return;
    }

    if (g_strcmp0(adjusted, ai_candidate.text) != 0 || ai_candidate.host != request->host)
    {
        ai_completion_clear_candidate();
        ai_candidate.host = request->host;
        ai_candidate.text = adjusted;
        adjusted = NULL;
        ai_candidate.context = g_strdup(request->context);
        ai_candidate.suffix = g_strdup(request->suffix);
        if (ai_candidate_timeout_source_id != 0)
        {
            g_source_remove(ai_candidate_timeout_source_id);
        }
        ai_candidate_timeout_source_id =
            g_timeout_add_seconds(AI_CANDIDATE_TIMEOUT_SECONDS,
                                  ai_completion_candidate_timeout,
                                  request->host);
        request->host->show_inline_completion(request->host, preview);
    }
    g_free(adjusted);
    g_free(preview);
}

static gchar *
ai_completion_sse_payload_delta(const gchar *payload)
{
    JsonParser *parser;
    JsonNode *root;
    JsonObject *object;
    JsonArray *choices;
    JsonObject *choice;
    gchar *delta;

    parser = json_parser_new();
    if (!json_parser_load_from_data(parser, payload, -1, NULL))
    {
        g_object_unref(parser);
        return NULL;
    }
    root = json_parser_get_root(parser);
    if (!JSON_NODE_HOLDS_OBJECT(root))
    {
        g_object_unref(parser);
        return NULL;
    }
    object = json_node_get_object(root);
    choices = json_object_get_array_member(object, "choices");
    if (choices == NULL || json_array_get_length(choices) == 0)
    {
        g_object_unref(parser);
        return NULL;
    }
    choice = json_array_get_object_element(choices, 0);
    delta = ai_completion_extract_choice_content(choice);
    g_object_unref(parser);

    return delta;
}

static void
ai_completion_finish_stream(AiRequest *request);

static gboolean
ai_completion_sse_line(AiRequest *request, const gchar *line, gsize line_length)
{
    gchar *owned;
    const gchar *payload;
    gchar *delta;
    gboolean done;

    if (line_length == 0)
    {
        return FALSE;
    }
    owned = g_strndup(line, line_length);
    if (owned[0] == ':')
    {
        /* SSE 注释行 */
        g_free(owned);
        return FALSE;
    }
    done = FALSE;
    payload = NULL;
    if (g_str_has_prefix(owned, "data:"))
    {
        payload = owned + 5;
        while (*payload == ' ')
        {
            payload++;
        }
    }
    else if (owned[0] == '{')
    {
        /* 少数兼容服务不写 data: 前缀，直接输出 JSON 行。 */
        payload = owned;
    }
    if (payload != NULL)
    {
        if (g_str_equal(payload, "[DONE]"))
        {
            ai_completion_finish_stream(request);
            done = TRUE;
        }
        else
        {
            delta = ai_completion_sse_payload_delta(payload);
            if (delta != NULL && *delta != '\0')
            {
                gchar *joined;

                joined = g_strconcat(request->completion != NULL ? request->completion : "",
                                     delta,
                                     NULL);
                g_free(request->completion);
                request->completion = joined;
                ai_completion_show_candidate(request, request->completion);
            }
            g_free(delta);
        }
    }
    g_free(owned);

    return done;
}

/* 处理缓冲区中所有完整的 SSE 行；返回 TRUE 表示已收到 [DONE]，
 * 此时请求已被终结并释放，调用方不得再访问。 */
static gboolean
ai_completion_process_sse(AiRequest *request)
{
    gchar *cursor;
    gboolean done;

    cursor = request->buffer->str;
    done = FALSE;
    while (*cursor != '\0')
    {
        gchar *newline;
        gchar *line;
        gsize line_length;

        newline = strchr(cursor, '\n');
        if (newline == NULL)
        {
            break;
        }
        line = cursor;
        line_length = (gsize)(newline - cursor);
        if (line_length > 0 && line[line_length - 1] == '\r')
        {
            line_length--;
        }
        done = ai_completion_sse_line(request, line, line_length);
        if (done)
        {
            break;
        }
        cursor = newline + 1;
    }
    if (!done)
    {
        g_string_erase(request->buffer, 0, (gssize)(cursor - request->buffer->str));
    }

    return done;
}

static void
ai_completion_request_free(AiRequest *request)
{
    if (request == NULL)
    {
        return;
    }
    g_clear_object(&request->message);
    if (request->stream != NULL)
    {
        g_object_unref(request->stream);
    }
    if (request->buffer != NULL)
    {
        g_string_free(request->buffer, TRUE);
    }
    g_free(request->context);
    g_free(request->suffix);
    g_free(request->completion);
    g_free(request);
}

static void
ai_completion_finish_stream(AiRequest *request)
{
    /* 流在 [DONE] 或 EOF 时结束。若仍有未换行的残余 data 行，按一个事件处理。 */
    if (request->buffer->len > 0 && request->buffer->str[0] != '\0')
    {
        gchar *tail;
        gchar *delta;

        tail = g_strdup(request->buffer->str);
        g_strstrip(tail);
        delta = NULL;
        if (*tail != '\0')
        {
            const gchar *payload;

            if (g_str_has_prefix(tail, "data:"))
            {
                payload = tail + 5;
                while (*payload == ' ')
                {
                    payload++;
                }
            }
            else
            {
                payload = tail;
            }
            if (!g_str_equal(payload, "[DONE]"))
            {
                delta = ai_completion_sse_payload_delta(payload);
            }
            if (delta != NULL && *delta != '\0')
            {
                gchar *joined;

                joined = g_strconcat(request->completion != NULL ? request->completion : "",
                                     delta,
                                     NULL);
                g_free(request->completion);
                request->completion = joined;
            }
            g_free(delta);
        }
        g_free(tail);
    }

    if (request->completion == NULL || *request->completion == '\0')
    {
        if (!request->automatic)
        {
            request->host->show_toast(request->host, _("AI service returned an empty completion"));
        }
        ai_request_in_flight = FALSE;
        ai_completion_request_free(request);
        return;
    }
    ai_completion_show_candidate(request, request->completion);
    if (!request->automatic)
    {
        request->host->show_toast(request->host, _("AI completion ready: press Tab to accept or Escape to dismiss"));
    }
    ai_request_in_flight = FALSE;
    ai_completion_request_free(request);
}

static void
ai_completion_finish_json(AiRequest *request)
{
    GError *error;
    gchar *completion;

    error = NULL;
    completion = ai_completion_extract_content(request->buffer->str,
                                               request->buffer->len,
                                               &error);
    if (completion != NULL)
    {
        ai_completion_show_candidate(request, completion);
        if (!request->automatic)
        {
            request->host->show_toast(request->host, _("AI completion ready: press Tab to accept or Escape to dismiss"));
        }
        g_free(completion);
    }
    else
    {
        gchar *message;

        message = g_strdup_printf(_("Unable to parse AI completion: %s"), error->message);
        if (!request->automatic)
        {
            request->host->show_toast(request->host, message);
        }
        g_free(message);
        g_clear_error(&error);
    }
    ai_request_in_flight = FALSE;
    ai_completion_request_free(request);
}

static void
ai_completion_finish_error(AiRequest *request)
{
    gchar *detail;
    gchar *message;

    detail = ai_completion_error_detail_data(request->buffer->str, request->buffer->len);
    message = g_strdup_printf(_("AI service returned HTTP %u: %s"),
                              request->http_status,
                              (detail != NULL && *detail != '\0') ?
                              detail : soup_message_get_reason_phrase(request->message));
    if (!request->automatic)
    {
        request->host->show_toast(request->host, message);
    }
    g_free(detail);
    g_free(message);
    ai_request_in_flight = FALSE;
    ai_completion_request_free(request);
}

static void
ai_completion_read_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    AiRequest *request;
    GError *error;
    gssize count;

    request = user_data;
    error = NULL;
    count = g_input_stream_read_finish(G_INPUT_STREAM(source), result, &error);

    /* 会话停用或有较新的请求时，只完成并释放 I/O，不再触及插件宿主。 */
    if (request->generation != ai_generation || ai_session == NULL)
    {
        g_clear_error(&error);
        ai_completion_request_free(request);
        return;
    }

    if (count > 0)
    {
        g_string_append_len(request->buffer,
                            (const gchar *)request->read_buffer,
                            (gssize)count);
        if (request->buffer->len > AI_STREAM_BUFFER_LIMIT)
        {
            if (!request->automatic)
            {
                request->host->show_toast(request->host, _("AI completion response was too large"));
            }
            ai_request_in_flight = FALSE;
            ai_completion_request_free(request);
            return;
        }
        if (request->streaming && ai_completion_process_sse(request))
        {
            /* [DONE]：请求已在处理过程中终结并释放。 */
            return;
        }
        g_input_stream_read_async(request->stream,
                                  request->read_buffer,
                                  sizeof(request->read_buffer),
                                  G_PRIORITY_DEFAULT,
                                  NULL,
                                  ai_completion_read_finished,
                                  request);
        return;
    }

    if (error != NULL)
    {
        /* 中断：abort（新请求或停用）在上面的代际检查已释放，这里只处理真实错误。 */
        if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
            gchar *message;

            message = g_strdup_printf(_("AI completion request failed: %s"), error->message);
            if (!request->automatic)
            {
                request->host->show_toast(request->host, message);
            }
            g_free(message);
            ai_request_in_flight = FALSE;
        }
        g_clear_error(&error);
        ai_completion_request_free(request);
        return;
    }

    if (request->error_mode)
    {
        ai_completion_finish_error(request);
    }
    else if (request->streaming)
    {
        ai_completion_finish_stream(request);
    }
    else
    {
        ai_completion_finish_json(request);
    }
}

static void
ai_completion_send_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    AiRequest *request;
    GInputStream *stream;
    GError *error;
    guint status;
    const gchar *content_type;

    request = user_data;
    error = NULL;
    stream = soup_session_send_finish(SOUP_SESSION(source), result, &error);

    if (request->generation != ai_generation || ai_session == NULL)
    {
        g_clear_error(&error);
        if (stream != NULL)
        {
            g_object_unref(stream);
        }
        ai_completion_request_free(request);
        return;
    }

    if (stream == NULL)
    {
        if (error == NULL || !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
            gchar *message;

            message = g_strdup_printf(_("AI completion request failed: %s"),
                                      error != NULL ? error->message : _("No response received"));
            if (!request->automatic)
            {
                request->host->show_toast(request->host, message);
            }
            g_free(message);
        }
        g_clear_error(&error);
        ai_request_in_flight = FALSE;
        ai_completion_request_free(request);
        return;
    }

    request->stream = stream;
    request->buffer = g_string_new(NULL);
    status = soup_message_get_status(request->message);
    if (status < 200 || status >= 300)
    {
        /* 读取错误正文，让 toast 能显示服务端的错误详情。 */
        request->error_mode = TRUE;
        request->http_status = status;
        g_input_stream_read_async(stream,
                                  request->read_buffer,
                                  sizeof(request->read_buffer),
                                  G_PRIORITY_DEFAULT,
                                  NULL,
                                  ai_completion_read_finished,
                                  request);
        return;
    }
    content_type = soup_message_headers_get_content_type(
        soup_message_get_response_headers(request->message), NULL);
    request->streaming = content_type != NULL &&
                         strstr(content_type, "text/event-stream") != NULL;
    g_input_stream_read_async(stream,
                              request->read_buffer,
                              sizeof(request->read_buffer),
                              G_PRIORITY_DEFAULT,
                              NULL,
                              ai_completion_read_finished,
                              request);
}

static void
ai_completion_request_start(MtPluginHost *host, gboolean automatic);

static gboolean
ai_completion_key_is_editing(guint keyval, guint state)
{
    if ((state & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK)) != 0)
    {
        return FALSE;
    }
    if ((keyval >= GDK_KEY_Left && keyval <= GDK_KEY_Down) ||
        (keyval >= GDK_KEY_F1 && keyval <= GDK_KEY_F12) ||
        keyval == GDK_KEY_Home || keyval == GDK_KEY_End ||
        keyval == GDK_KEY_Page_Up || keyval == GDK_KEY_Page_Down ||
        keyval == GDK_KEY_Insert || keyval == GDK_KEY_Tab)
    {
        return FALSE;
    }

    return TRUE;
}

static gboolean
ai_completion_auto_cb(gpointer user_data)
{
    MtPluginHost *host;

    host = user_data;
    ai_auto_source_id = 0;
    if (ai_session == NULL)
    {
        return G_SOURCE_REMOVE;
    }
    ai_completion_request_start(host, TRUE);
    return G_SOURCE_REMOVE;
}

static void
ai_completion_auto_schedule(MtPluginHost *host)
{
    if (g_get_monotonic_time() < ai_reject_until_us)
    {
        /* 用户刚拒绝过候选：短时间内不再自动弹出，避免“阴魂不散”。 */
        return;
    }
    if (!ai_completion_document_allowed(host, TRUE))
    {
        /* 自动补全只服务设置里勾选的文档类型：纯文本、对话或已取消勾选的
         * 格式不主动弹，否则模型会把“你好”续成一句问候，看起来像阴魂不散的聊天。 */
        return;
    }
    if (ai_auto_source_id != 0)
    {
        g_source_remove(ai_auto_source_id);
    }
    ai_auto_source_id = g_timeout_add(AI_AUTO_DELAY_MILLISECONDS,
                                      ai_completion_auto_cb,
                                      host);
}

static gboolean
ai_completion_handle_key(MtPluginHost *host,
                         guint keyval,
                         guint keycode,
                         guint state,
                         gpointer user_data)
{
    gchar *current_context;

    (void)keycode;
    (void)user_data;
    if (ai_candidate.text != NULL && ai_candidate.host == host)
    {
        current_context = host->get_text_before_cursor(host);
        if (g_strcmp0(current_context, ai_candidate.context) == 0)
        {
            if (keyval == GDK_KEY_Tab && state == 0)
            {
                gchar *accepted;

                /* 先移除文本视图覆盖层，再作为一次用户操作插入同一份候选文本。
                 * 避免 buffer 的 changed 回调与 GTK overlay 重绘交错造成半透明残影。
                 * 候选文本在展示时就已完成缩进对齐与后缀去重，接受即所见。 */
                accepted = g_strdup(ai_candidate.text);
                g_free(current_context);
                ai_completion_clear_candidate();
                host->insert_text(host, accepted);
                g_free(accepted);
                return TRUE;
            }
            if (keyval == GDK_KEY_Escape && state == 0)
            {
                g_free(current_context);
                ai_completion_reject_suppress();
                ai_completion_clear_candidate();
                return TRUE;
            }
        }
        g_free(current_context);
        ai_completion_reject_suppress();
        ai_completion_clear_candidate();
    }

    if (ai_auto_enabled && ai_completion_key_is_editing(keyval, state))
    {
        ai_completion_auto_schedule(host);
    }
    return FALSE;
}

static void
ai_completion_request_start(MtPluginHost *host, gboolean automatic)
{
    GKeyFile *settings;
    gchar *endpoint;
    gchar *model;
    gchar *api_key;
    gchar *context;
    gchar *suffix;
    gchar *trimmed_context;
    gchar *trimmed_suffix;
    gchar *body;
    gchar *authorization;
    gchar *summary;
    SoupMessage *message;
    GBytes *body_bytes;
    AiRequest *request;

    if (automatic && !ai_auto_enabled)
    {
        /* 自动补全已在“偏好设置”中关闭，忽略迟到的防抖回调。 */
        return;
    }
    if (!ai_completion_document_allowed(host, automatic))
    {
        if (!automatic)
        {
            host->show_toast(host, _("AI completion is disabled for this file type in the Extensions settings"));
        }
        return;
    }

    settings = ai_completion_load_settings();
    endpoint = ai_completion_get_setting(settings, "endpoint");
    model = ai_completion_get_setting(settings, "model");
    api_key = ai_completion_get_setting(settings, "api-key");
    g_key_file_unref(settings);

    /* 兼容旧版本已保存的 /v1 基础地址，实际请求始终指向补全端点。 */
    {
        gchar *normalized_endpoint;

        normalized_endpoint = ai_completion_normalize_endpoint(endpoint);
        g_free(endpoint);
        endpoint = normalized_endpoint;
    }

    if (*endpoint == '\0' || *model == '\0' || *api_key == '\0')
    {
        if (!automatic)
        {
            host->show_toast(host, _("Configure an AI endpoint, model and API key in Extensions first"));
        }
        g_free(endpoint);
        g_free(model);
        g_free(api_key);
        return;
    }

    context = host->get_text_before_cursor(host);
    {
        gchar *trimmed_check;

        trimmed_check = g_strdup(context);
        g_strstrip(trimmed_check);
        if (*trimmed_check == '\0')
        {
            g_free(trimmed_check);
            if (!automatic)
            {
                host->show_toast(host, _("Type some text before requesting AI completion"));
            }
            g_free(context);
            g_free(endpoint);
            g_free(model);
            g_free(api_key);
            return;
        }
        g_free(trimmed_check);
    }

    suffix = host->get_text_after_cursor != NULL ?
             host->get_text_after_cursor(host) : g_strdup("");
    trimmed_context = ai_completion_trim_context(context, AI_CONTEXT_LIMIT);
    trimmed_suffix = ai_completion_trim_context(suffix, AI_SUFFIX_LIMIT);
    if (automatic && ai_auto_context_loaded && g_strcmp0(trimmed_context, ai_auto_context) == 0)
    {
        /* 与上次自动请求相同的上下文，避免重复消耗额度。 */
        g_free(trimmed_suffix);
        g_free(trimmed_context);
        g_free(suffix);
        g_free(context);
        g_free(endpoint);
        g_free(model);
        g_free(api_key);
        return;
    }
    g_free(ai_auto_context);
    ai_auto_context = g_strdup(trimmed_context);
    ai_auto_context_loaded = TRUE;
    summary = ai_code_summary_get_current(host);
    body = ai_completion_build_body(model, trimmed_context, trimmed_suffix, summary);
    g_free(summary);
    message = soup_message_new("POST", endpoint);
    if (message == NULL)
    {
        if (!automatic)
        {
            host->show_toast(host, _("AI endpoint URL is invalid"));
        }
        g_free(body);
        g_free(trimmed_context);
        g_free(trimmed_suffix);
        g_free(context);
        g_free(suffix);
        g_free(endpoint);
        g_free(model);
        g_free(api_key);
        return;
    }

    authorization = g_strdup_printf("Bearer %s", api_key);
    soup_message_headers_append(soup_message_get_request_headers(message), "Authorization", authorization);
    soup_message_headers_append(soup_message_get_request_headers(message), "Accept", "application/json");
    body_bytes = g_bytes_new_take(body, strlen(body));
    soup_message_set_request_body_from_bytes(message, "application/json", body_bytes);
    g_bytes_unref(body_bytes);

    if (ai_request_in_flight)
    {
        /* 新输入使旧请求作废：推进代际并中止会话，旧回调只释放 I/O。 */
        ai_generation++;
        soup_session_abort(ai_session);
    }
    ai_completion_clear_candidate();
    ai_generation++;
    request = g_new0(AiRequest, 1);
    request->host = host;
    request->message = g_object_ref(message);
    request->context = g_strdup(context);
    request->suffix = g_strdup(suffix);
    request->generation = ai_generation;
    request->automatic = automatic;
    ai_request_in_flight = TRUE;
    soup_session_send_async(ai_session,
                            message,
                            G_PRIORITY_DEFAULT,
                            NULL,
                            ai_completion_send_finished,
                            request);
    if (!automatic)
    {
        host->show_toast(host, _("Requesting AI completion…"));
        /* 手动请求是用户的明确意图：取消拒绝抑制，让响应正常展示。 */
        ai_reject_until_us = 0;
    }

    g_object_unref(message);
    g_free(authorization);
    g_free(trimmed_context);
    g_free(trimmed_suffix);
    g_free(context);
    g_free(suffix);
    g_free(endpoint);
    g_free(model);
    g_free(api_key);
}

static void
ai_completion_activate_action(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    MtPluginHost *host;

    (void)action;
    (void)parameter;

    host = user_data;
    ai_completion_auto_cancel();
    ai_completion_request_start(host, FALSE);
}

G_MODULE_EXPORT const MtPluginInfo *
mt_plugin_query(void)
{
    return &ai_completion_plugin_info;
}

G_MODULE_EXPORT gboolean
mt_plugin_activate(MtPluginHost *host, GError **error)
{
    static const gchar *accelerators[] = { "<Primary><Shift>space", NULL };

    if (host->show_inline_completion == NULL || host->clear_inline_completion == NULL ||
        host->add_key_handler == NULL)
    {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_NOT_SUPPORTED,
                    "Vellum host does not provide inline completion services");
        return FALSE;
    }

    if (ai_session == NULL)
    {
        ai_session = soup_session_new();
        g_object_set(ai_session, "timeout", 30, NULL);
    }
    ai_auto_context_loaded = FALSE;
    g_clear_pointer(&ai_auto_context, g_free);
    ai_request_in_flight = FALSE;
    {
        GKeyFile *settings;

        settings = ai_completion_load_settings();
        ai_auto_enabled = ai_completion_get_auto_enabled(settings);
        ai_completion_languages_load(settings);
        g_key_file_unref(settings);
    }

    if (!host->add_action(host,
                          "ai-complete",
                          ai_completion_activate_action,
                          host,
                          NULL))
    {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_EXISTS,
                    "The ai-complete action is already registered");
        return FALSE;
    }

    host->set_accelerators(host, "app.ai-complete", accelerators);
    host->add_key_handler(host, ai_completion_handle_key, NULL, NULL);
    ai_code_summary_activate(host);

    if (host->add_preference_switch != NULL)
    {
        host->add_preference_switch(host,
                                    _("AI Completion"),
                                    _("Automatic AI completion"),
                                    _("Wait for a short pause after typing, then request a completion without pressing a shortcut."),
                                    ai_completion_pref_auto_get,
                                    ai_completion_pref_auto_set,
                                    NULL,
                                    NULL);
    }

    return TRUE;
}

G_MODULE_EXPORT void
mt_plugin_deactivate(MtPluginHost *host)
{
    (void)host;

    ai_completion_auto_cancel();
    ai_completion_clear_candidate();
    ai_code_summary_deactivate();
    g_clear_pointer(&ai_auto_context, g_free);
    ai_auto_context_loaded = FALSE;
    ai_request_in_flight = FALSE;
    ai_generation++;
    ai_completion_languages_set(NULL);
    if (ai_session != NULL)
    {
        soup_session_abort(ai_session);
        g_clear_object(&ai_session);
    }
}

G_MODULE_EXPORT void
mt_plugin_configure(MtPluginHost *host, gpointer parent_window)
{
    GKeyFile *settings;
    gchar *endpoint;
    gchar *model;
    gchar *api_key;
    gboolean auto_enabled;
    AdwPreferencesWindow *window;
    AdwPreferencesPage *page;
    AdwPreferencesGroup *group;
    AdwActionRow *save_row;
    GtkWidget *save_button;
    AiConfigWidgets *widgets;

    settings = ai_completion_load_settings();
    endpoint = ai_completion_get_setting(settings, "endpoint");
    model = ai_completion_get_setting(settings, "model");
    api_key = ai_completion_get_setting(settings, "api-key");
    auto_enabled = ai_completion_get_auto_enabled(settings);
    /* settings 在页面全部组构建完成后再释放：文档类型组还要读它。 */

    window = ADW_PREFERENCES_WINDOW(adw_preferences_window_new());
    gtk_window_set_title(GTK_WINDOW(window), _("AI Completion Settings"));
    gtk_window_set_transient_for(GTK_WINDOW(window), GTK_WINDOW(parent_window));
    gtk_window_set_modal(GTK_WINDOW(window), TRUE);
    page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(group, _("OpenAI-Compatible Service"));
    adw_preferences_group_set_description(group,
                                          _("Enter a full Chat Completions URL, an OpenAI-compatible URL ending in /v1, or a bare service host. Text around the cursor is sent to this service."));

    widgets = g_new0(AiConfigWidgets, 1);
    widgets->host = host;
    widgets->window = GTK_WINDOW(window);
    widgets->endpoint_row = ADW_ENTRY_ROW(adw_entry_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(widgets->endpoint_row), _("API Endpoint URL"));
    gtk_editable_set_text(GTK_EDITABLE(widgets->endpoint_row), endpoint);
    adw_preferences_group_add(group, GTK_WIDGET(widgets->endpoint_row));

    widgets->model_row = ADW_ENTRY_ROW(adw_entry_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(widgets->model_row), _("Model"));
    gtk_editable_set_text(GTK_EDITABLE(widgets->model_row), model);
    adw_preferences_group_add(group, GTK_WIDGET(widgets->model_row));

    widgets->key_row = ADW_PASSWORD_ENTRY_ROW(adw_password_entry_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(widgets->key_row), _("API Key"));
    gtk_editable_set_text(GTK_EDITABLE(widgets->key_row), api_key);
    adw_preferences_group_add(group, GTK_WIDGET(widgets->key_row));

    widgets->auto_row = ADW_SWITCH_ROW(adw_switch_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(widgets->auto_row),
                                  _("Suggest automatically while typing"));
    adw_action_row_set_subtitle(ADW_ACTION_ROW(widgets->auto_row),
                                _("Wait for a short pause after typing, then request a completion without pressing a shortcut."));
    adw_switch_row_set_active(widgets->auto_row, auto_enabled);
    adw_preferences_group_add(group, GTK_WIDGET(widgets->auto_row));

    {
        /* 保存设置固定在页面最顶部，长列表设置（文档类型）不再被保存行截断。 */
        AdwPreferencesGroup *save_group;

        save_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
        save_row = ADW_ACTION_ROW(adw_action_row_new());
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(save_row), _("Save AI Settings"));
        save_button = gtk_button_new_with_label(_("Save"));
        gtk_widget_set_valign(save_button, GTK_ALIGN_CENTER);
        adw_action_row_add_suffix(save_row, save_button);
        adw_preferences_group_add(save_group, GTK_WIDGET(save_row));
        g_signal_connect(save_button,
                         "clicked",
                         G_CALLBACK(ai_completion_config_save_clicked),
                         widgets);
        adw_preferences_page_add(page, save_group);
    }
    adw_preferences_page_add(page, group);


    {
        /* AI 自动总结设置组：位于 API 配置与文档类型之间。
         * 开启开关 + 修改行数间隔 + 提示说明。 */
        widgets->summary_widgets = ai_code_summary_add_config_group(page, settings);
    }

    {
        /* 文档类型表格：列出系统安装的全部 GtkSourceView 语言。
         * 第一列方形复选框（勾选=启用该格式），第二列文件类型，第三列备注
         * （扩展名或 MIME 类型）。全部使用原生 GTK4 组件。 */
        AdwPreferencesGroup *language_group;
        GtkSourceLanguageManager *language_manager;
        const gchar * const *language_ids;
        GHashTable *disabled_languages;
        gboolean category_enabled[AI_LANGUAGE_CATEGORY_COUNT];
        gboolean category_partial[AI_LANGUAGE_CATEGORY_COUNT];
        guint language_index;

        language_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
        adw_preferences_group_set_title(language_group, _("Document Types for AI Completion"));
        adw_preferences_group_set_description(language_group,
                                              _("AI completion is available only for the checked document types. Unchecked types are skipped for both automatic and shortcut-triggered completion."));
        widgets->language_items = g_list_store_new(AI_TYPE_LANGUAGE_ITEM);
        {
            gchar *disabled_value;

            disabled_value = ai_completion_get_setting(settings, "disabled-languages");
            disabled_languages = ai_completion_parse_disabled_languages(disabled_value);
            g_free(disabled_value);
        }
        language_manager = gtk_source_language_manager_get_default();
        language_ids = gtk_source_language_manager_get_language_ids(language_manager);
        {
            /* 按大类分组收集（用于统计各类初始开关状态），
             * 模型本身只存语言行，按名称排序；大类块单独放在表格上方。 */
            GPtrArray *groups[AI_LANGUAGE_CATEGORY_COUNT];
            gint category;
            guint language_count;

            for (category = 0; category < AI_LANGUAGE_CATEGORY_COUNT; category++)
            {
                groups[category] = g_ptr_array_new();
                category_enabled[category] = FALSE;
                category_partial[category] = FALSE;
            }
            for (language_index = 0;
                 language_ids != NULL && language_ids[language_index] != NULL;
                 language_index++)
            {
                GtkSourceLanguage *language;
                const gchar *id;
                const gchar *name;
                const gchar *remark;
                gchar *remark_owned;
                AiLanguageItem *item;

                language = gtk_source_language_manager_get_language(language_manager,
                                                                    language_ids[language_index]);
                if (language == NULL)
                {
                    continue;
                }
                id = gtk_source_language_get_id(language);
                name = gtk_source_language_get_name(language);
                remark = gtk_source_language_get_metadata(language, "globs");
                remark_owned = NULL;
                if (remark == NULL || *remark == '\0')
                {
                    remark_owned = ai_completion_language_remark_from_mimes(language);
                    remark = remark_owned;
                }
                item = ai_language_item_new(id,
                                            name != NULL ? name : id,
                                            remark != NULL ? remark : "",
                                            !g_hash_table_contains(disabled_languages, id));
                item->category = ai_completion_language_category(id);
                g_ptr_array_add(groups[item->category], item);
                g_free(remark_owned);
            }
            g_hash_table_unref(disabled_languages);

            language_count = 0;
            for (category = 0; category < AI_LANGUAGE_CATEGORY_COUNT; category++)
            {
                guint enabled_count;
                guint group_index;

                enabled_count = 0;
                for (group_index = 0; group_index < groups[category]->len; group_index++)
                {
                    AiLanguageItem *item;

                    item = g_ptr_array_index(groups[category], group_index);
                    if (item->enabled)
                    {
                        enabled_count++;
                    }
                    g_list_store_append(widgets->language_items, item);
                    g_object_unref(item);
                    language_count++;
                }
                if (groups[category]->len > 0)
                {
                    category_enabled[category] =
                        enabled_count == groups[category]->len;
                    category_partial[category] =
                        enabled_count > 0 && enabled_count < groups[category]->len;
                }
                g_ptr_array_unref(groups[category]);
            }
            g_list_store_sort(widgets->language_items,
                              (GCompareDataFunc)ai_completion_language_item_compare,
                              NULL);

            if (language_count == 0)
            {
                category_enabled[0] = FALSE;
                category_partial[0] = FALSE;
            }
        }

        if (g_list_model_get_n_items(G_LIST_MODEL(widgets->language_items)) == 0)
        {
            GtkWidget *note;

            note = gtk_label_new(_("No GtkSourceView language definitions were found on this system."));
            gtk_widget_set_margin_start(note, 4);
            gtk_widget_set_margin_end(note, 4);
            gtk_widget_set_margin_bottom(note, 4);
            gtk_widget_set_hexpand(note, TRUE);
            gtk_label_set_wrap(GTK_LABEL(note), TRUE);
            gtk_label_set_justify(GTK_LABEL(note), GTK_JUSTIFY_FILL);
            adw_preferences_group_add(language_group, note);
        }
        else
        {
            GtkColumnView *table;
            GtkSingleSelection *selection;
            GtkListItemFactory *factory;
            GtkColumnViewColumn *column;
            GtkFlowBox *category_box;
            GHashTable *category_checks;
            gint category;

            /* 大类块：全部大类统一放在最上方，用分隔线与下方格式表隔开；
             * 大类开关联动下方对应格式的勾选。每个大类复选框横向铺满
             * 整行（均分宽度），窗口变窄时自动换行。 */
            category_box = GTK_FLOW_BOX(gtk_flow_box_new());
            gtk_flow_box_set_selection_mode(category_box, GTK_SELECTION_NONE);
            gtk_flow_box_set_column_spacing(category_box, 8);
            gtk_flow_box_set_row_spacing(category_box, 2);
            gtk_widget_set_margin_top(GTK_WIDGET(category_box), 2);
            gtk_widget_set_margin_bottom(GTK_WIDGET(category_box), 2);
            category_checks = g_hash_table_new(NULL, NULL);
            for (category = 0; category < AI_LANGUAGE_CATEGORY_COUNT; category++)
            {
                GtkCheckButton *category_check;

                category_check = GTK_CHECK_BUTTON(
                    gtk_check_button_new_with_label(ai_completion_category_name(category)));
                gtk_widget_set_hexpand(GTK_WIDGET(category_check), TRUE);
                gtk_widget_set_halign(GTK_WIDGET(category_check), GTK_ALIGN_CENTER);
                g_object_set_data(G_OBJECT(category_check),
                                  "vellum-ai-category",
                                  GINT_TO_POINTER(category));
                g_object_set_data(G_OBJECT(category_check),
                                  "vellum-ai-store",
                                  widgets->language_items);
                g_signal_connect(category_check,
                                 "toggled",
                                 G_CALLBACK(ai_completion_category_toggled),
                                 NULL);
                gtk_check_button_set_active(category_check,
                                            category_enabled[category]);
                gtk_check_button_set_inconsistent(category_check,
                                                  category_partial[category]);
                g_hash_table_replace(category_checks,
                                     GINT_TO_POINTER(category),
                                     category_check);
                gtk_flow_box_append(category_box, GTK_WIDGET(category_check));
            }
            adw_preferences_group_add(language_group, GTK_WIDGET(category_box));
            adw_preferences_group_add(language_group,
                                      gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

            table = GTK_COLUMN_VIEW(gtk_column_view_new(NULL));
            gtk_widget_set_margin_top(GTK_WIDGET(table), 4);
            gtk_widget_set_margin_bottom(GTK_WIDGET(table), 4);
            gtk_column_view_set_show_column_separators(table, TRUE);
            gtk_column_view_set_show_row_separators(table, TRUE);
            /* 大类复选框登记表：单行切换后刷新对应大类状态。 */
            g_object_set_data_full(G_OBJECT(table),
                                   "vellum-ai-category-checks",
                                   category_checks,
                                   (GDestroyNotify)g_hash_table_unref);
            selection = gtk_single_selection_new(G_LIST_MODEL(widgets->language_items));
            gtk_single_selection_set_autoselect(selection, FALSE);
            gtk_column_view_set_model(table, GTK_SELECTION_MODEL(selection));

            /* 第一列：方形复选框。 */
            factory = gtk_signal_list_item_factory_new();
            g_signal_connect(factory, "setup",
                             G_CALLBACK(ai_completion_check_factory_setup), NULL);
            g_signal_connect(factory, "bind",
                             G_CALLBACK(ai_completion_check_factory_bind), NULL);
            g_signal_connect(factory, "unbind",
                             G_CALLBACK(ai_completion_check_factory_unbind), NULL);
            column = gtk_column_view_column_new(NULL, factory);
            gtk_column_view_column_set_fixed_width(column, 48);
            gtk_column_view_append_column(table, column);

            /* 第二列：文件类型名称。 */
            factory = gtk_signal_list_item_factory_new();
            g_signal_connect(factory, "setup",
                             G_CALLBACK(ai_completion_label_factory_setup), NULL);
            g_signal_connect(factory, "bind",
                             G_CALLBACK(ai_completion_name_factory_bind), NULL);
            g_signal_connect(factory, "unbind",
                             G_CALLBACK(ai_completion_label_factory_unbind), NULL);
            column = gtk_column_view_column_new(_("File Type"), factory);
            gtk_column_view_column_set_expand(column, TRUE);
            gtk_column_view_append_column(table, column);

            /* 第三列：备注（扩展名/MIME）。 */
            factory = gtk_signal_list_item_factory_new();
            g_signal_connect(factory, "setup",
                             G_CALLBACK(ai_completion_label_factory_setup), NULL);
            g_signal_connect(factory, "bind",
                             G_CALLBACK(ai_completion_remark_factory_bind), NULL);
            g_signal_connect(factory, "unbind",
                             G_CALLBACK(ai_completion_label_factory_unbind), NULL);
            column = gtk_column_view_column_new(_("Remarks"), factory);
            gtk_column_view_column_set_expand(column, TRUE);
            gtk_column_view_append_column(table, column);

            adw_preferences_group_add(language_group, GTK_WIDGET(table));
        }


        adw_preferences_page_add(page, language_group);
    }

    g_object_set_data_full(G_OBJECT(window),
                           "vellum-ai-config-widgets",
                           widgets,
                           (GDestroyNotify)ai_completion_config_widgets_free);

    adw_preferences_window_add(window, page);
    gtk_window_present(GTK_WINDOW(window));

    g_key_file_unref(settings);
    g_free(endpoint);
    g_free(model);
    g_free(api_key);
}
