/*
 * welcome-plugin.c
 * Vellum 的分关卡新手引导。引导把首次输入与扩展选择拆成明确、可完成的交互，
 * 每个扩展的介绍信息由独立 INI 配置提供，便于新增扩展时无需改写引导流程。
 * 第二关新增：源码/二进制安装方式选择、x86_64 架构检测、多发行版环境说明、
 * 本地构建环境检测与自定义软件源配置。
 */

#include "welcome-private.h"

static MtWelcomeData *welcome_data;

G_MODULE_EXPORT const MtPluginInfo *
mt_plugin_query(void)
{
    static MtPluginInfo info;

    info.api_version = MT_PLUGIN_API_VERSION;
    info.id = "io.github.vellum.welcome";
    info.name = welcome_text("新手引导", "Welcome Guide");
    info.description = welcome_text("通过交互关卡完成首次输入和扩展选择", "Complete first input and extension selection through interactive levels");
    info.version = "0.3.0";
    return &info;
}

G_MODULE_EXPORT gboolean
mt_plugin_activate(MtPluginHost *host, GError **error)
{
    MtWelcomeData *data;
    gchar *flag;

    data = g_new0(MtWelcomeData, 1);
    data->host = host;
    data->plugin_id = g_strdup("io.github.vellum.welcome");
    data->extension_buttons = g_ptr_array_new();
    welcome_data = data;
    if (!host->add_action(host, "show-welcome", welcome_show_action, data,
                          (GDestroyNotify)welcome_data_free))
    {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                    "The show-welcome action is already registered");
        welcome_data = NULL;
        welcome_data_free(data);
        return FALSE;
    }
    flag = welcome_flag_path();
    if (!g_file_test(flag, G_FILE_TEST_EXISTS))
    {
        data->show_source_id = g_timeout_add(600, welcome_auto_show, data);
    }
    g_free(flag);
    return TRUE;
}

G_MODULE_EXPORT void
mt_plugin_deactivate(MtPluginHost *host)
{
    (void)host;
    if (welcome_data != NULL)
    {
        welcome_close_guide(welcome_data);
    }
}