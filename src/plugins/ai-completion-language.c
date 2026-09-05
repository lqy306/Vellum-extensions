/*
 * ai-completion-language.c
 * 文档类型表格：AiLanguageItem GObject 类型、分类、表格工厂与构建函数。
 */

#include "ai-completion-private.h"
#include "ai-completion-language.h"

#include <adwaita.h>
#include <glib/gi18n.h>
#include <gtksourceview/gtksource.h>
#include <string.h>

G_DECLARE_FINAL_TYPE(AiLanguageItem, ai_language_item, AI, LANGUAGE_ITEM, GObject)

typedef struct _AiLanguageItem
{
    GObject parent_instance;
    gchar *id;
    gchar *name;
    gchar *remark;
    gboolean enabled;
    gint category;
    gboolean is_category_row;
    gboolean partial;
} AiLanguageItem;

/* 此版本 G_DECLARE_FINAL_TYPE 不生成 *_TYPE_* 宏，手动补上。 */
#define AI_TYPE_LANGUAGE_ITEM (ai_language_item_get_type ())

G_DEFINE_TYPE(AiLanguageItem, ai_language_item, G_TYPE_OBJECT)

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

/* 把表格里未勾选的文档类型收集为禁用集合（分类分隔行不参与）。 */
GHashTable *
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

/* 构建文档类型设置组（大类开关 + 格式表格）。
 * 返回 AdwPreferencesGroup；通过 out_items 返回表格数据模型
 *（调用者需持有引用，用于保存/释放）。 */
AdwPreferencesGroup *
ai_completion_build_language_group(GKeyFile *settings, GListStore **out_items)
{
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
    *out_items = g_list_store_new(AI_TYPE_LANGUAGE_ITEM);
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
                g_list_store_append(*out_items, item);
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
        g_list_store_sort(*out_items,
                          (GCompareDataFunc)ai_completion_language_item_compare,
                          NULL);

        if (language_count == 0)
        {
            category_enabled[0] = FALSE;
            category_partial[0] = FALSE;
        }
    }

    if (g_list_model_get_n_items(G_LIST_MODEL(*out_items)) == 0)
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
                              *out_items);
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
        selection = gtk_single_selection_new(G_LIST_MODEL(*out_items));
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

    return language_group;
}