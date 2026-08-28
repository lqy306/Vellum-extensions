# Vellum Extensions

Vellum 内置扩展的**发布仓库**（Release 托管与目录源）。Vellum 的 deb/rpm/AppImage 不再自带扩展：首次启动或"扩展市场"从本仓库的 Release 拉取。

## 内容

- `extensions.json` — 扩展目录（id、名称、说明、版本、二进制与源码资产名）
- 每个 Release 附带 19 个资产：9 个 `.so` 二进制、9 个 `.vut` 源码包、`extensions.json`

## 发布流程（在 Vellum 源码仓库执行）

```sh
BASE_URL=https://github.com/lqy306/Vellum-extensions/releases/latest/download \
  ./packaging/collect-plugin-assets.sh
# 把 dist/plugin-assets/ 下全部文件上传到本仓库的 Release（tag 形如 extensions-x.y.z）
```

## 添加为自定义源

在 `~/.config/vellum/market-sources.ini`：

```ini
[Sources]
urls=https://github.com/lqy306/Vellum-extensions/releases/latest/download
```

默认源即本仓库；多源按 id 去重、先到先得。
