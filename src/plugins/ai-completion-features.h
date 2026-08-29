/*
 * ai-completion-features.h
 * 由 ai-completion-plugin.c 调用的“扩展特性”接口：活动感知触发、补全预览面板、
 * 错误修复（红/绿 diff）、本地统计。实现见 ai-completion-features.c。
 */

#ifndef AI_COMPLETION_FEATURES_H
#define AI_COMPLETION_FEATURES_H

#include "mt-plugin.h"

#include <adwaita.h>

/* 激活时调用一次：初始化面板、活动监视器、统计与“错误修复”动作。 */
void ai_features_init(MtPluginHost *host);
/* 停用时调用：保存统计、隐藏面板、释放状态。 */
void ai_features_shutdown(void);

/* 流式候选每增长一次都调用，用于刷新多行预览面板。 */
void ai_features_on_candidate(MtPluginHost *host, const gchar *text);
/* 候选被清除（接受/取消/超时）时调用，隐藏补全预览。 */
void ai_features_clear(void);

/* 错误修复：取当前全文交 LLM 修正，并以 diff 形式预览。 */
void ai_features_fix_start(MtPluginHost *host);
/* 构造错误修复/生成的请求体（含多文件上下文）。 */
gchar *ai_features_build_fix_body(const gchar *model, const gchar *code,
                                  const gchar *language_id, const gchar *multifile);
/* 是否正在展示错误修复 diff（供按键路由判断是否吞掉 Esc）。 */
gboolean ai_features_is_fix_visible(void);

/* 自适应触发：返回当前应使用的防抖延时（毫秒）。 */
guint ai_features_adaptive_delay(void);
/* 设置自动补全的基础防抖延时（毫秒），越小越灵敏。 */
void ai_features_set_base_delay(guint ms);
/* 光标是否处于适合自动触发的位点（标识符/注释中间则否）。 */
gboolean ai_features_cursor_safe(MtPluginHost *host);

/* 本地统计：请求数、接受数、估算字符数。 */
void ai_features_stats_add_request(void);
void ai_features_stats_add_accepted(void);
void ai_features_stats_add_chars(gsize chars);
void ai_features_stats_load(void);
void ai_features_stats_save(void);

/* 在扩展设置页追加“统计”分组。 */
void ai_features_configure_add_stats(MtPluginHost *host, AdwPreferencesPage *page);

#endif
