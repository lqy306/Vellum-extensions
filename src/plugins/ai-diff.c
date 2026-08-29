/*
 * ai-diff.c
 * 行级 LCS（最长公共子序列）diff 实现，无外部依赖。
 * 用于错误修复预览：删除行标红、新增行标绿、不变行正常。
 */

#include "ai-diff.h"

#include <string.h>

void
ai_diff_line_free(AiDiffLine *line)
{
    if (line == NULL)
    {
        return;
    }
    g_free(line->text);
    g_free(line);
}

/* 计算 original→fixed 的第一个“替换”片段：行级定位 + 行内最小 span。
 * 成功返回 TRUE，并通过 out_* 给出文档字符偏移及被替换的旧/新文本（调用方释放）。 */
gboolean
ai_diff_inline_span(const gchar *original,
                    const gchar *fixed,
                    gint *out_offset,
                    gchar **out_old,
                    gchar **out_new)
{
    GPtrArray *lines;
    gint del_idx = -1;
    gint i;
    const gchar *p;
    gchar *del_text = NULL;
    gchar *add_text = NULL;
    gboolean ok = FALSE;

    if (original == NULL || fixed == NULL || out_offset == NULL ||
        out_old == NULL || out_new == NULL)
    {
        return FALSE;
    }
    lines = ai_diff_lines(original, fixed);
    for (i = 0; i < lines->len; i++)
    {
        AiDiffLine *ln = lines->pdata[i];
        if (ln->op == AI_DIFF_DEL)
        {
            del_idx = i;
            break;
        }
    }
    if (del_idx < 0)
    {
        g_ptr_array_unref(lines);
        return FALSE;
    }
    /* 文档内字符偏移 = 删除行之前的字符数。 */
    p = original;
    for (i = 0; i < del_idx; i++)
    {
        while (*p != '\0' && *p != '\n')
        {
            p++;
        }
        if (*p == '\n')
        {
            p++;
        }
    }
    *out_offset = (gint)g_utf8_strlen(original, (gssize)(p - original));
    del_text = g_strdup(((AiDiffLine *)lines->pdata[del_idx])->text);
    if (del_idx + 1 < lines->len &&
        ((AiDiffLine *)lines->pdata[del_idx + 1])->op == AI_DIFF_ADD)
    {
        add_text = g_strdup(((AiDiffLine *)lines->pdata[del_idx + 1])->text);
    }
    else
    {
        g_free(del_text);
        g_ptr_array_unref(lines);
        return FALSE;
    }
    /* 取两行公共前缀/后缀，得到最小变更片段。 */
    {
        gsize a_len = strlen(del_text);
        gsize b_len = strlen(add_text);
        gsize pre = 0;

        while (pre < a_len && pre < b_len && del_text[pre] == add_text[pre])
        {
            pre++;
        }
        if (a_len > pre || b_len > pre)
        {
            gsize s_a = a_len;
            gsize s_b = b_len;
            while (s_a > pre && s_b > pre &&
                   del_text[s_a - 1] == add_text[s_b - 1])
            {
                s_a--;
                s_b--;
            }
            *out_offset += (gint)g_utf8_strlen(del_text, (gssize)pre);
            *out_old = g_strndup(del_text + pre, a_len - pre);
            *out_new = g_strndup(add_text + pre, b_len - pre);
            ok = TRUE;
        }
    }
    g_free(del_text);
    g_free(add_text);
    g_ptr_array_unref(lines);
    return ok;
}

static gchar **
ai_diff_split_lines(const gchar *text, gint *count)
{
    gchar **lines;
    gint n;
    gint capacity;
    const gchar *p;
    const gchar *start;

    n = 0;
    capacity = 16;
    lines = g_new(gchar *, capacity);
    if (text == NULL)
    {
        *count = 0;
        return lines;
    }
    p = text;
    start = text;
    while (TRUE)
    {
        if (*p == '\n' || *p == '\0')
        {
            gsize len;
            gchar *line;

            len = (gsize)(p - start);
            if (len > 0 && p > start && p[-1] == '\r')
            {
                len--;
            }
            line = g_strndup(start, len);
            if (n == capacity)
            {
                capacity *= 2;
                lines = g_renew(gchar *, lines, capacity);
            }
            lines[n++] = line;
            if (*p == '\0')
            {
                break;
            }
            start = p + 1;
        }
        p++;
    }
    *count = n;

    return lines;
}

GPtrArray *
ai_diff_lines(const gchar *a, const gchar *b)
{
    gint n_a;
    gint n_b;
    gint i;
    gint j;
    gchar **lines_a;
    gchar **lines_b;
    GPtrArray *result;
    gint64 limit;
    gint **dp;

    result = g_ptr_array_new_with_free_func((GDestroyNotify)ai_diff_line_free);
    lines_a = ai_diff_split_lines(a, &n_a);
    lines_b = ai_diff_split_lines(b, &n_b);

    /* 超大文件退化为整段替换，避免 O(n*m) 内存爆炸。 */
    limit = (gint64)4 * 1024 * 1024;
    if (n_a > 0 && n_b > 0 && (gint64)n_a * (gint64)n_b > limit)
    {
        for (i = 0; i < n_a; i++)
        {
            AiDiffLine *d = g_new0(AiDiffLine, 1);
            d->op = AI_DIFF_DEL;
            d->text = g_strdup(lines_a[i]);
            g_ptr_array_add(result, d);
        }
        for (j = 0; j < n_b; j++)
        {
            AiDiffLine *d = g_new0(AiDiffLine, 1);
            d->op = AI_DIFF_ADD;
            d->text = g_strdup(lines_b[j]);
            g_ptr_array_add(result, d);
        }
        goto done;
    }

    dp = g_new0(gint *, (guint)(n_a + 1));
    for (i = 0; i <= n_a; i++)
    {
        dp[i] = g_new0(gint, (guint)(n_b + 1));
    }
    for (i = n_a - 1; i >= 0; i--)
    {
        for (j = n_b - 1; j >= 0; j--)
        {
            if (g_strcmp0(lines_a[i], lines_b[j]) == 0)
            {
                dp[i][j] = dp[i + 1][j + 1] + 1;
            }
            else
            {
                dp[i][j] = MAX(dp[i + 1][j], dp[i][j + 1]);
            }
        }
    }

    i = 0;
    j = 0;
    while (i < n_a && j < n_b)
    {
        if (g_strcmp0(lines_a[i], lines_b[j]) == 0)
        {
            AiDiffLine *d = g_new0(AiDiffLine, 1);
            d->op = AI_DIFF_EQUAL;
            d->text = g_strdup(lines_a[i]);
            g_ptr_array_add(result, d);
            i++;
            j++;
        }
        else if (dp[i + 1][j] >= dp[i][j + 1])
        {
            AiDiffLine *d = g_new0(AiDiffLine, 1);
            d->op = AI_DIFF_DEL;
            d->text = g_strdup(lines_a[i]);
            g_ptr_array_add(result, d);
            i++;
        }
        else
        {
            AiDiffLine *d = g_new0(AiDiffLine, 1);
            d->op = AI_DIFF_ADD;
            d->text = g_strdup(lines_b[j]);
            g_ptr_array_add(result, d);
            j++;
        }
    }
    while (i < n_a)
    {
        AiDiffLine *d = g_new0(AiDiffLine, 1);
        d->op = AI_DIFF_DEL;
        d->text = g_strdup(lines_a[i]);
        g_ptr_array_add(result, d);
        i++;
    }
    while (j < n_b)
    {
        AiDiffLine *d = g_new0(AiDiffLine, 1);
        d->op = AI_DIFF_ADD;
        d->text = g_strdup(lines_b[j]);
        g_ptr_array_add(result, d);
        j++;
    }

    for (i = 0; i <= n_a; i++)
    {
        g_free(dp[i]);
    }
    g_free(dp);

done:
    for (i = 0; i < n_a; i++)
    {
        g_free(lines_a[i]);
    }
    for (j = 0; j < n_b; j++)
    {
        g_free(lines_b[j]);
    }
    g_free(lines_a);
    g_free(lines_b);

    return result;
}
