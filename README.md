# Vellum Extensions

Vellum 内置扩展的**源码与发布仓库**：仓库源码放扩展源码（`src/plugins/`），Release 放发行包（`.so` 二进制、`.vut` 源码包）与目录 `extensions.json`。Vellum 的 deb/rpm/AppImage 不再自带扩展：首次启动或"扩展市场"从本仓库的 Release 拉取。

## 内容

- 仓库源码：`src/plugins/`（9 个内置扩展源码）与 ABI 头 `src/mt-plugin.h`；`Makefile` 可从源码构建扩展二进制，`BUILDING.md` 说明环境要求
- Release：`extensions.json` 目录 + 每个 Release 附带的 19 个资产（9 个 `.so` 二进制、9 个 `.vut` 源码包、`extensions.json`）

## 发布流程（在本仓库执行）

```sh
make                      # 构建 9 个 .so 到 build/
./packaging/collect-plugin-assets.sh   # 生成 .vut 源码包与 extensions.json 到 dist/plugin-assets/
# 把 dist/plugin-assets/ 下全部文件上传到本仓库的 Release（tag 形如 extensions-x.y.z）
```

> 插件源码与编辑器仓库保持同步：每次扩展发布时从编辑器仓库同步 `src/plugins/` 与 `src/mt-plugin.h`。

## 添加为自定义源

在 `~/.config/vellum/market-sources.ini`：

```ini
[Sources]
urls=https://github.com/lqy306/Vellum-extensions/releases/latest/download
```

默认源即本仓库；多源按 id 去重、先到先得。
