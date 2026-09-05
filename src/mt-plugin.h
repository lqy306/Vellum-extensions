/*
 * mt-plugin.h
 * Vellum C 插件 ABI。插件只能使用此头文件中声明的宿主服务。
 */

#ifndef MT_PLUGIN_H
#define MT_PLUGIN_H

#include <glib.h>
#include <gio/gio.h>

typedef struct _GtkWidget GtkWidget;
typedef struct _GtkWindow GtkWindow;

G_BEGIN_DECLS

#define MT_PLUGIN_API_VERSION 3

typedef struct _MtPluginHost MtPluginHost;
typedef struct _MtPluginInfo MtPluginInfo;

typedef enum
{
    MT_PLUGIN_PANEL_SIDEBAR,
    MT_PLUGIN_PANEL_AUXILIARY
} MtPluginPanelLocation;
typedef void (*MtPluginActionCallback)(GSimpleAction *action,
                                       GVariant *parameter,
                                       gpointer user_data);
typedef gboolean (*MtPluginKeyCallback)(MtPluginHost *host,
                                        guint keyval,
                                        guint keycode,
                                        guint state,
                                        gpointer user_data);
typedef gboolean (*MtPluginPreferenceGetFunc)(gpointer user_data);
typedef void (*MtPluginPreferenceSetFunc)(gboolean value, gpointer user_data);
/* 仅在用户编辑代码文档时触发；changed_lines 为本次插入或删除涉及的逻辑行数。 */
typedef void (*MtPluginDocumentChangeFunc)(MtPluginHost *host,
                                           guint changed_lines,
                                           gpointer user_data);

typedef enum
{
    MT_PLUGIN_EDITOR_MOVE_LEFT,
    MT_PLUGIN_EDITOR_MOVE_RIGHT,
    MT_PLUGIN_EDITOR_MOVE_UP,
    MT_PLUGIN_EDITOR_MOVE_DOWN,
    MT_PLUGIN_EDITOR_MOVE_LINE_START,
    MT_PLUGIN_EDITOR_MOVE_LINE_END,
    MT_PLUGIN_EDITOR_MOVE_DOCUMENT_START,
    MT_PLUGIN_EDITOR_MOVE_DOCUMENT_END,
    MT_PLUGIN_EDITOR_MOVE_WORD_FORWARD,
    MT_PLUGIN_EDITOR_MOVE_WORD_BACKWARD,
    MT_PLUGIN_EDITOR_DELETE_FORWARD_CHAR,
    MT_PLUGIN_EDITOR_DELETE_CURRENT_LINE,
    MT_PLUGIN_EDITOR_UNDO,
    MT_PLUGIN_EDITOR_REDO,
    MT_PLUGIN_EDITOR_SAVE,
    MT_PLUGIN_EDITOR_SAVE_AND_CLOSE,
    MT_PLUGIN_EDITOR_CLOSE,
    MT_PLUGIN_EDITOR_FORCE_CLOSE,
    MT_PLUGIN_EDITOR_YANK_LINE,
    MT_PLUGIN_EDITOR_CHANGE_LINE,
    MT_PLUGIN_EDITOR_PASTE
} MtPluginEditorCommand;

struct _MtPluginHost
{
    guint api_version;
    gpointer private_data;
    gboolean (*add_action)(MtPluginHost *host,
                           const gchar *name,
                           MtPluginActionCallback callback,
                           gpointer user_data,
                           GDestroyNotify destroy_notify);
    void (*set_accelerators)(MtPluginHost *host,
                             const gchar *detailed_action_name,
                             const gchar * const *accelerators);
    void (*insert_text)(MtPluginHost *host, const gchar *text);
    gchar *(*get_current_text)(MtPluginHost *host);
    gchar *(*get_text_before_cursor)(MtPluginHost *host);
    gchar *(*get_text_after_cursor)(MtPluginHost *host);
    gchar *(*get_current_file_path)(MtPluginHost *host);
    void (*open_file_path)(MtPluginHost *host, const gchar *path);
    GtkWindow *(*get_parent_window)(MtPluginHost *host);
    void (*set_panel)(MtPluginHost *host,
                      const gchar *id,
                      MtPluginPanelLocation location,
                      GtkWidget *panel);
    void (*hide_panel)(MtPluginHost *host,
                       const gchar *id,
                       MtPluginPanelLocation location);
    gboolean (*add_key_handler)(MtPluginHost *host,
                                MtPluginKeyCallback callback,
                                gpointer user_data,
                                GDestroyNotify destroy_notify);
    gboolean (*run_editor_command)(MtPluginHost *host,
                                   MtPluginEditorCommand command);
    void (*show_toast)(MtPluginHost *host, const gchar *message);
    void (*show_inline_completion)(MtPluginHost *host, const gchar *text);
    void (*clear_inline_completion)(MtPluginHost *host);
    /* 错误修复的行内 diff 覆盖层：在 offset 处把 old_text 以红色删除线、
     * new_text 以绿色展示；apply 时在 offset 处将 old 替换为 new。 */
    void (*show_inline_diff)(MtPluginHost *host, gint offset,
                             const gchar *old_text, const gchar *new_text);
    void (*clear_inline_diff)(MtPluginHost *host);
    void (*apply_inline_diff)(MtPluginHost *host);
    /* 编译器报错的红色波浪下划线 + 悬停描述。offset/length 为字节偏移，
     * message 为悬停时显示的完整描述（可为 NULL，仅画线不给描述）。
     * clear_error_underlines 一次性清除当前文档全部下划线。 */
    void (*show_error_underline)(MtPluginHost *host,
                                 gint offset,
                                 gint length,
                                 const gchar *message);
    void (*clear_error_underlines)(MtPluginHost *host);
    /* 断点：1-based 逻辑行号。set/clear 同步 gutter 标记；插件自行维护断点集合。 */
    void (*set_breakpoint)(MtPluginHost *host, gint line);
    void (*clear_breakpoint)(MtPluginHost *host, gint line);
    void (*clear_all_breakpoints)(MtPluginHost *host);
    /* 滚动到指定 1-based 行并短暂高亮，用于错误跳转与断点定位。 */
    void (*scroll_to_line)(MtPluginHost *host, gint line);
    /* 请求宿主卸载并移除指定插件（含内置插件）。用于插件引导用户
     * 完成向导后自我删除：确认后插件会从列表隐藏且重启后不再加载。 */
    void (*request_plugin_removal)(MtPluginHost *host, const gchar *plugin_id);
    gboolean (*add_preference_switch)(MtPluginHost *host,
                                      const gchar *group_title,
                                      const gchar *title,
                                      const gchar *subtitle,
                                      MtPluginPreferenceGetFunc get_callback,
                                      MtPluginPreferenceSetFunc set_callback,
                                      gpointer user_data,
                                      GDestroyNotify destroy_notify);
    /* 当前活动文档是否已识别为代码语言；自动补全等仅限代码的功能依赖它。 */
    gboolean (*get_is_code_document)(MtPluginHost *host);
    /* 注册活动文档的用户编辑通知，供摘要、格式分析等扩展使用。 */
    gboolean (*add_document_change_handler)(MtPluginHost *host,
                                            MtPluginDocumentChangeFunc callback,
                                            gpointer user_data,
                                            GDestroyNotify destroy_notify);
    /* 按稳定扩展标识启用或停用扩展；供新手引导保存用户选择。 */
    gboolean (*set_extension_enabled)(MtPluginHost *host,
                                      const gchar *plugin_id,
                                      gboolean enabled,
                                      GError **error);
    /* 当前活动文档的 GtkSourceView 语言 ID；非代码文档返回 NULL。
     * AI 补全等按文档类型启停的扩展依赖它区分具体格式。 */
    const gchar *(*get_document_language_id)(MtPluginHost *host);
    /* 返回当前窗口已打开文档的绝对文件路径列表（仅已保存文件），用于多文件上下文。
     * count 可为 NULL；返回以 NULL 结尾的 GStrv，调用者用 g_strfreev 释放。 */
    gchar **(*get_open_documents)(MtPluginHost *host, gsize *count);
    /* 扩展是否已安装（已加载且启用）；供欢迎引导等判断是否需要下载。 */
    gboolean (*has_plugin)(MtPluginHost *host, const gchar *plugin_id);
    /* 按稳定 ID 异步安装扩展，prefer_source 为 TRUE 时优先源码；热更新无需重启。 */
    void (*install_extension_async)(MtPluginHost *host,
                                   const gchar *plugin_id,
                                   gboolean prefer_source,
                                   GCancellable *cancellable,
                                   GAsyncReadyCallback callback,
                                   gpointer user_data);
    gboolean (*install_extension_finish)(MtPluginHost *host,
                                        GAsyncResult *result,
                                        GError **error);
};

struct _MtPluginInfo
{
    guint api_version;
    const gchar *id;
    const gchar *name;
    const gchar *description;
    const gchar *version;
};

typedef const MtPluginInfo *(*MtPluginQueryFunc)(void);
typedef gboolean (*MtPluginActivateFunc)(MtPluginHost *host, GError **error);
typedef void (*MtPluginDeactivateFunc)(MtPluginHost *host);
typedef void (*MtPluginConfigureFunc)(MtPluginHost *host, gpointer parent_window);

/* 每个插件必须实现以下两个导出函数。 */
const MtPluginInfo *mt_plugin_query(void);
gboolean mt_plugin_activate(MtPluginHost *host, GError **error);

/* 此函数可选；宿主退出前会调用它。 */
void mt_plugin_deactivate(MtPluginHost *host);

/* 此函数可选；宿主在扩展管理页面请求设置时调用它。 */
void mt_plugin_configure(MtPluginHost *host, gpointer parent_window);

G_END_DECLS

#endif
