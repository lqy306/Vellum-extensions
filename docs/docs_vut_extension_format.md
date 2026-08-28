# Vellum 原生扩展包格式（`.vut`）

`.vut` 是 Vellum 的**原生扩展归档格式**。文件扩展名为 `.vut`，容器是标准 ZIP；每个包只含一个目标平台的原生动态模块和一个必需的 UTF-8 INI 清单。该格式用于导入、导出和分发，不能将原生机器码变成跨平台代码。

> 原生扩展与 Vellum 在同一进程、以当前用户权限运行。`.vut` 的清单校验只防止误导入不兼容模块，**不构成沙箱、签名验证或恶意代码防护**。只应导入可信来源的包。

## 归档布局

```text
example.vut                         # 标准 ZIP
├── vellum-extension.ini            # 必需：清单，UTF-8
└── module.so                       # 必需：当前目标的单一原生模块
```

导入器拒绝 ZIP64、加密条目、符号链接、路径穿越（`..`、绝对路径、目录分隔符）、不受支持的压缩方法、超过大小/条目限制的归档。导出器生成标准 ZIP 的非压缩条目；导入器支持 ZIP 的 Store 与 Deflate 方法。

## 必需清单

```ini
[Vellum Extension]
format-version=1
id=io.example.sample
name=Sample Extension
version=1.2.0
plugin-api=1
license=BSD-2-Clause
module=module.so

[Target]
os=linux
architecture=x86_64
abi=gnu
```

| 字段 | 含义与导入要求 |
|---|---|
| `format-version` | 当前必须为 `1`。 |
| `id`、`name`、`version` | 面向用户和诊断的包标识；动态模块自身仍须通过 Vellum 插件 ABI 查询。 |
| `plugin-api` | 必须等于当前 `MT_PLUGIN_API_VERSION`。 |
| `license` | 模块作者声明的许可证；导出未知来源模块时会标注为 `Unknown`。 |
| `module` | 模块的 ZIP 内相对文件名；当前格式只接受根目录单一模块，防止路径穿越。 |
| `os` | 目标操作系统，例如 `linux`、`windows`、`freebsd`。必须与运行 Vellum 的系统一致。 |
| `architecture` | 目标 CPU 架构，例如 `x86_64`、`aarch64`、`armv7`。必须精确匹配。 |
| `abi` | 目标 C 运行时 ABI 标签，例如 Linux 上的 `gnu`。必须与当前构建支持的标签一致。 |

## 兼容性边界

`.so` 是 ELF 原生模块，不能直接跨 Windows、Linux、BSD 或 CPU 架构加载。Windows 原生模块通常为 `.dll`，macOS 为 `.dylib`，不同 BSD 也需要针对目标系统重新编译。即便同为 Linux/x86_64，仍需匹配 Vellum 插件 ABI、动态加载器、C 运行时，以及 GTK/GLib/Libadwaita/GtkSourceView 等运行时 ABI；不同发行版应分别构建，或明确最低兼容基线并在目标系统验证。

因此，发布者应针对每个目标至少生成独立包，例如：

| 目标 | 推荐包名 | `Target` 清单 |
|---|---|---|
| GNU/Linux x86_64 | `sample-linux-x86_64.vut` | `os=linux`、`architecture=x86_64`、`abi=gnu` |
| GNU/Linux ARM64 | `sample-linux-aarch64.vut` | `os=linux`、`architecture=aarch64`、`abi=gnu` |
| Windows x86_64 | `sample-windows-x86_64.vut` | `os=windows`、`architecture=x86_64`、相应 ABI 标签 |
| FreeBSD x86_64 | `sample-freebsd-x86_64.vut` | `os=freebsd`、`architecture=x86_64`、相应 ABI 标签 |

同一扩展可以共享 C 源码和插件 ABI，但必须对每个目标进行单独构建和测试。当前 Vellum Linux 发行构建会拒绝其他平台或架构的 `.vut`，并在导入前给出具体不兼容原因。

## 可选源码扩展包

`.vut` 可不携带预编译模块，而是携带可审阅源码。源码包仍是 ZIP，但清单以 `payload=source` 声明构建入口和输出模块；源码目录中的条目可使用相对路径，但不得包含绝对路径、`..`、符号链接或 ZIP 加密条目。

```ini
[Vellum Extension]
format-version=1
id=io.example.sample
name=Sample Extension
version=1.2.0
plugin-api=1
license=BSD-2-Clause
payload=source

[Target]
os=linux
architecture=x86_64
abi=gnu

[Source]
source-root=source
build-tool=make
build-arguments=plugin
output-module=build/module.so
required-tools=make;gcc
```

| 字段 | 规则 |
|---|---|
| `payload` | `binary`（默认）或 `source`。源码包不能同时声明二进制 `module`。 |
| `source-root` | ZIP 内源码根目录，必须是安全相对路径。 |
| `build-tool` | 只允许 `make` 或编译器驱动 `cc`/`gcc`；由 Vellum 使用参数数组直接启动，**不调用 shell**。 |
| `build-arguments` | 以空格分隔的受限参数；不得包含 `;`、`|`、`&`、反引号、`$` 或重定向字符。 |
| `output-module` | 构建后相对于临时源码根目录的模块路径，必须以当前平台动态模块后缀结尾。 |
| `required-tools` | 分号分隔的本机工具名；Vellum 在构建前逐一检测，例如 `make;gcc`。 |

源码包可使用 `architecture=any` 和 `abi=any`，表示在**同一目标操作系统**上针对当前机器重新编译；二进制包不允许该通配符，必须准确声明自身机器码的 OS、架构和 ABI。源码仍应针对不同 OS 分别构建或测试，因为编译器、动态模块后缀和依赖约定可能不同。

导入源码包时，Vellum 先检查目标 OS、架构、ABI、插件 API、清单安全性与所需工具。满足条件后会展示**构建源码扩展**的明确确认步骤；用户确认后，源码才会被解包至私有临时目录并使用 `GSubprocess` 直接执行清单允许的工具和参数。构建不联网、不提升权限，输出仅在通过动态模块 ABI 加载后写入用户扩展目录。任何构建失败都保留可读输出，且不会安装半成品模块。

源码包让目标系统自行编译，因而可作为跨架构发布的方式；但它仍要求该系统具备匹配的编译器、构建工具、Vellum 开发头文件和 GTK/GLib 依赖。它不是通用脚本运行器，也不应视作针对不可信代码的隔离机制。

## BSD-2-Clause 与来源

`.vut` 容器本身不改变模块许可证。Vellum 自有源码、内置插件源码和格式规范采用 BSD-2-Clause；第三方扩展必须由其作者在清单的 `license` 字段中如实声明。导入外部包前仍应核验模块来源、许可证和信任关系。
