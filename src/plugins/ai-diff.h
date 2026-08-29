/*
 * ai-diff.h
 * 行级 LCS diff：用于错误修复时把原文与修正文以红/绿差异呈现。
 */

#ifndef AI_DIFF_H
#define AI_DIFF_H

#include <glib.h>

typedef enum
{
    AI_DIFF_EQUAL,
    AI_DIFF_ADD,
    AI_DIFF_DEL
} AiDiffOp;

typedef struct
{
    AiDiffOp op;
    gchar *text;
} AiDiffLine;

/* 计算 a→b 的行级差异，返回 GPtrArray<AiDiffLine*>（自带释放函数）。
 * 调用方用 g_ptr_array_unref 释放。超大输入退化为整段替换。 */
GPtrArray *ai_diff_lines(const gchar *a, const gchar *b);

void ai_diff_line_free(AiDiffLine *line);

/* 计算 original→fixed 的第一个“替换”片段（行级定位 + 行内最小 span），
 * 供输入区行内红/绿 diff 使用。成功返回 TRUE 并通过 out_* 给出文档字符偏移与
 * 被替换的旧文本 / 新文本（调用方释放）。无相邻 DEL+ADD 对时返回 FALSE。 */
gboolean ai_diff_inline_span(const gchar *original,
                             const gchar *fixed,
                             gint *out_offset,
                             gchar **out_old,
                             gchar **out_new);

#endif
