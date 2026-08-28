# 构建 Vellum 扩展

本仓库存放 9 个内置扩展的源码（`src/plugins/` 与 ABI 头 `src/mt-plugin.h`）。可以从源码构建扩展二进制（`.so`）与源码包（`.vut`）。

## 环境要求

需要 `make`、`cc`、`pkg-config` 以及各插件清单列出的 GTK/GLib 开发包。Debian / Ubuntu 大致为：

```sh
sudo apt install make gcc pkg-config \
  libglib2.0-dev libgtk-4-dev libadwaita-1-dev libgtksourceview-5-dev \
  libsoup-3.0-dev libjson-glib-dev
```

各插件的 pkg-config 依赖：

| 插件 | 依赖 |
|---|---|
| timestamp-plugin | `gio-2.0 gmodule-2.0` |
| word-count-plugin | `gio-2.0 gmodule-2.0` |
| ai-completion-plugin | `gtk4 libadwaita-1 gio-2.0 gmodule-2.0 libsoup-3.0 json-glib-1.0 gtksourceview-5` |
| link-check-plugin | `gio-2.0 gmodule-2.0 libsoup-3.0` |
| project-sidebar-plugin | `gtk4 libadwaita-1 gio-2.0 gmodule-2.0` |
| build-run-plugin | `gtk4 libadwaita-1 gio-2.0 gmodule-2.0` |
| vim-mode-plugin | `gtk4 gio-2.0 gmodule-2.0` |
| screenshot-plugin | `gtk4 gio-2.0 gmodule-2.0` |
| welcome-plugin | `gtk4 libadwaita-1 gio-2.0 gmodule-2.0` |

## 构建二进制

```sh
make            # 输出全部 9 个 .so 到 build/
make build/ai-completion-plugin.so   # 或单独构建
make clean
```

## 生成发行包并发布

```sh
# 1. 构建二进制
make

# 2. 生成 .vut 源码包与 extensions.json 清单到 dist/plugin-assets/
./packaging/collect-plugin-assets.sh

# 3. 把 dist/plugin-assets/ 下全部文件上传到本仓库的 Release
#    （tag 形如 extensions-x.y.z）
```

> 插件源码与编辑器仓库保持同步：每次扩展发布时从编辑器仓库同步 `src/plugins/` 与 `src/mt-plugin.h`。
