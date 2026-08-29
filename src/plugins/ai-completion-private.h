/*
 * ai-completion-private.h
 * 同一插件内部（ai-completion-plugin.c 与 ai-completion-features.c）共享的
 * 少量辅助函数声明。宿主 ABI 见 mt-plugin.h，本文件只是插件自身的内部契约。
 */

#ifndef AI_COMPLETION_PRIVATE_H
#define AI_COMPLETION_PRIVATE_H

#include "mt-plugin.h"

#include <glib.h>

GKeyFile *ai_completion_load_settings(void);
gchar *ai_completion_get_setting(GKeyFile *settings, const gchar *key);
gchar *ai_completion_normalize_endpoint(const gchar *endpoint);
gchar *ai_completion_extract_content(const gchar *response, gsize length, GError **error);
/* 按设置装配多文件上下文（0=当前文件 1=已打开文件 2=项目目录）。
 * 返回新分配字符串（可能为空），调用者 g_free。 */
gchar *ai_completion_gather_context(MtPluginHost *host);

#endif
