/*
 * ai-completion-language.h
 * 文档类型表格接口：由 ai-completion-language.c 实现，
 * 供 ai-completion-plugin.c 的配置页面调用。
 */

#ifndef AI_COMPLETION_LANGUAGE_H
#define AI_COMPLETION_LANGUAGE_H

#include "mt-plugin.h"

#include <adwaita.h>
#include <gtksourceview/gtksource.h>

#include <glib.h>

/* 构建文档类型设置组（大类开关 + 格式表格）。
 * 返回 AdwPreferencesGroup；通过 out_items 返回表格数据模型
 *（调用者需持有引用，用于保存/释放）。 */
AdwPreferencesGroup *
ai_completion_build_language_group(GKeyFile *settings, GListStore **out_items);

/* 把表格里未勾选的文档类型收集为禁用集合（分类分隔行不参与）。 */
GHashTable *
ai_completion_disabled_from_items(GListStore *items);

#endif