/*
 * welcome-private.h
 * 新手引导插件的内部共享声明。
 */

#ifndef WELCOME_PRIVATE_H
#define WELCOME_PRIVATE_H

#include <adwaita.h>
#include <glib.h>
#include <gmodule.h>

#include "mt-plugin.h"

#define VELLUM_EXTENSION_INTRO_DIR "/usr/local/share/vellum/extension-intros"

typedef struct _WelcomeExtensionIntro WelcomeExtensionIntro;
typedef struct _MtWelcomeData MtWelcomeData;

struct _WelcomeExtensionIntro
{
    gchar *id;
    gchar *icon;
    gchar *title;
    gchar *description;
    gchar *lesson;
};

struct _MtWelcomeData
{
    MtPluginHost *host;
    gchar *plugin_id;
    GtkWidget *guide_window;
    GtkStack *pages;
    GtkLabel *progress_label;
    GtkButton *back_button;
    GtkButton *next_button;
    GPtrArray *extension_buttons;
    guint page;
    guint show_source_id;
    guint remove_source_id;
    gboolean guide_complete;
    GtkCheckButton *binary_radio;
    GtkCheckButton *source_radio;
    GtkLabel *arch_label;
    GtkWidget *source_detail_box;
    GtkLabel *env_status_label;
    GtkButton *check_env_button;
    AdwEntryRow *custom_source_entry;
    GtkLabel *custom_source_status;
    gboolean is_x64;
    gboolean prefer_source;
    GPtrArray *pending_install_ids;
    guint install_index;
    guint install_success;
    guint install_failed;
};

/* 语言与偏好 */
static const gchar * const welcome_extension_files[] = {
    "ai-completion.ini",
    "timestamp.ini",
    "word-count.ini",
    "link-check.ini",
    "project-sidebar.ini",
    "dev-experience.ini",
    "vim-mode.ini",
    "screenshot.ini",
    NULL
};

gboolean welcome_is_zh(void);
const gchar *welcome_text(const gchar *zh, const gchar *en);
gchar *welcome_flag_path(void);
gchar *welcome_install_pref_path(void);
gchar *welcome_market_sources_path(void);
gboolean welcome_is_x64_arch(void);
gboolean welcome_load_install_pref(void);
void welcome_save_install_pref(gboolean prefer_source);
gchar *welcome_check_build_env(gboolean *out_ready);
gchar *welcome_get_market_sources_display(void);
gboolean welcome_add_market_source(const gchar *url, GError **error);

/* 安装流程 */
void welcome_install_next(MtWelcomeData *data);
void welcome_start_installs(MtWelcomeData *data);

/* 扩展介绍 */
void welcome_extension_intro_free(WelcomeExtensionIntro *intro);
gchar *welcome_intro_path(const gchar *filename);
WelcomeExtensionIntro *welcome_load_intro(const gchar *filename);

/* 导航 */
void welcome_update_navigation(MtWelcomeData *data);
void welcome_show_page(MtWelcomeData *data, guint page);

/* UI 构建 */
GtkWidget *welcome_build_intro_page(void);
void welcome_add_extension_action_row(GtkBox *list,
                                     MtWelcomeData *data,
                                     WelcomeExtensionIntro *intro);
void welcome_install_mode_toggled(GtkCheckButton *button,
                                  gpointer user_data);
void welcome_check_env_clicked(GtkButton *button,
                                gpointer user_data);
void welcome_custom_source_add_clicked(GtkButton *button,
                                        gpointer user_data);
GtkWidget *welcome_build_extensions_page(MtWelcomeData *data);

/* 引导管理 */
void welcome_apply_extension_choices(MtWelcomeData *data);
void welcome_ask_remove(MtWelcomeData *data);
void welcome_close_guide(MtWelcomeData *data);
void welcome_next_clicked(GtkButton *button, gpointer user_data);
void welcome_back_clicked(GtkButton *button, gpointer user_data);
gboolean welcome_guide_close_request(GtkWidget *widget, gpointer user_data);
void welcome_show_guide(MtWelcomeData *data);
void welcome_show_action(GSimpleAction *action, GVariant *parameter, gpointer user_data);
void welcome_data_free(gpointer user_data);

/* 回调前向声明 */
static void welcome_remove_response(AdwMessageDialog *dialog,
                                    const gchar *response,
                                    gpointer user_data);
static void welcome_install_done(GObject *source,
                                 GAsyncResult *result,
                                 gpointer user_data);
gboolean welcome_auto_show(gpointer user_data);

#endif /* WELCOME_PRIVATE_H */