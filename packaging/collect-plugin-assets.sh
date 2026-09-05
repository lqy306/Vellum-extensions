#!/bin/sh
# 收集内置扩展的两种分发形态（二进制 .so 与源码 .vut）并生成扩展目录
# extensions.json，全部用于上传到 GitHub Release。
# Vellum 的 deb/rpm/AppImage 不再自带扩展；首次启动或"扩展市场"按目录
# 从 https://github.com/lqy306/Vellum/releases/latest/download/ 拉取。

set -eu

PROJECT_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD_DIR=${BUILD_DIR:-"$PROJECT_ROOT/build"}
OUT_DIR=${OUT_DIR:-"$PROJECT_ROOT/dist/plugin-assets"}
BASE_URL=${BASE_URL:-"https://github.com/lqy306/Vellum-extensions/releases/latest/download"}

if [ ! -d "$BUILD_DIR/src/plugins" ] && [ ! -d "$BUILD_DIR" ]; then
    echo "Build directory not found: $BUILD_DIR" >&2
    echo "Run: meson setup build && ninja -C build (or make in extensions repo)" >&2
    exit 1
fi

rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR"

# 源码 .vut 包（含 Makefile，导入后由本机 make/cc 重建）
"$PROJECT_ROOT/packaging/vut/build-source-packages.sh" "$OUT_DIR"

# 插件清单：构建产物名（.so / .vut）、显示信息
# 格式：name|id|display|description|version|source_package（用于生成源码文件名）
PLUGIN_LIST="timestamp-plugin|io.github.vellum.timestamp|Timestamp|Insert the current date and time|1.0.0|timestamp
word-count-plugin|io.github.vellum.document-statistics|Document Statistics|Live word, line and character counts|1.0.0|document-statistics
ai-completion-plugin|io.github.vellum.ai-completion|AI Completion|Complete text through an OpenAI-compatible API|0.3.0|ai-completion
link-check-plugin|io.github.vellum.link-check|Test Links|Check HTTP/HTTPS links in the document|1.0.0|link-check
project-sidebar-plugin|io.github.vellum.project-sidebar|Project Sidebar|Browse a project directory|1.0.0|project-sidebar
dev-experience-plugin|io.github.vellum.dev-experience|Dev Experience|Build, run and debug with compiler error underlines and breakpoints|1.1.0|dev-experience
vim-mode-plugin|io.github.vellum.vim-mode|Vi Mode|Modal editing keybindings|1.0.0|vim-mode
screenshot-plugin|io.github.vellum.screenshot|Screenshot|Capture the editor window|1.0.0|screenshot
welcome-plugin|io.github.vellum.welcome|Welcome Guide|Interactive first-run guide|0.3.0|welcome"

# 复制二进制：编辑器仓库构建在 build/src/plugins，扩展仓库 Makefile 输出在 build
printf '%s\n' "$PLUGIN_LIST" | while IFS='|' read -r name _id _display _desc _version _source_pkg; do
    if [ -f "$BUILD_DIR/src/plugins/$name.so" ]; then
        cp "$BUILD_DIR/src/plugins/$name.so" "$OUT_DIR/$name.so"
    elif [ -f "$BUILD_DIR/$name.so" ]; then
        cp "$BUILD_DIR/$name.so" "$OUT_DIR/$name.so"
    else
        echo "缺少 $name.so（请先在编辑器仓库或扩展仓库构建）" >&2
        exit 1
    fi
done

# 用 python3 生成 extensions.json（避免手写 JSON 的转义与逗号问题）
OUT_DIR="$OUT_DIR" BASE_URL="$BASE_URL" PLUGIN_LIST="$PLUGIN_LIST" python3 - <<'PYEOF'
import json
import os

out_dir = os.environ['OUT_DIR']
base = os.environ['BASE_URL']
entries = []
for line in os.environ['PLUGIN_LIST'].splitlines():
    name, plugin_id, display, desc, version, source_pkg = line.split('|')
    entries.append({
        "id": plugin_id,
        "name": display,
        "description": desc,
        "version": version,
        "binary": name + ".so",
        "source": source_pkg + "-linux-source.vut",
    })

with open(os.path.join(out_dir, 'extensions.json'), 'w', encoding='utf-8') as f:
    json.dump({"format": 1, "base": base, "extensions": entries}, f, ensure_ascii=False, indent=2)
    f.write('\n')
PYEOF

echo "已收集 $(ls "$OUT_DIR" | wc -l) 个文件到 $OUT_DIR"
echo "请将 $OUT_DIR 下的全部文件作为资产上传到 GitHub Release（含 extensions.json）。"
