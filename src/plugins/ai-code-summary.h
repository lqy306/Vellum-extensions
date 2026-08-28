/*
 * ai-code-summary.h
 * AI 代码总结子模块：按修改行数触发、缓存摘要并为补全提供上下文。
 */

#ifndef AI_CODE_SUMMARY_H
#define AI_CODE_SUMMARY_H

#include "mt-plugin.h"

#include <adwaita.h>

G_BEGIN_DECLS

typedef struct _AiCodeSummaryConfigWidgets AiCodeSummaryConfigWidgets;

AiCodeSummaryConfigWidgets *ai_code_summary_add_config_group(AdwPreferencesPage *page,
                                                              GKeyFile *settings);
void ai_code_summary_config_widgets_free(AiCodeSummaryConfigWidgets *widgets);
gboolean ai_code_summary_config_requires_cost_warning(AiCodeSummaryConfigWidgets *widgets);
void ai_code_summary_config_save(GKeyFile *settings, AiCodeSummaryConfigWidgets *widgets);

gboolean ai_code_summary_auto_enabled(GKeyFile *settings);
void ai_code_summary_activate(MtPluginHost *host);
void ai_code_summary_deactivate(void);
gchar *ai_code_summary_get_current(MtPluginHost *host);

G_END_DECLS

#endif
