#!/usr/bin/env python3
"""构建二进制 .vut 扩展包。

用法：make-binary-vut.py <plugin-name> <id> <name> <version> [plugin-api]

例：
  make-binary-vut.py dev-experience-plugin io.github.vellum.dev-experience "Dev Experience" 1.1.0 3

清单须与 mt_vut_package_get_host_*() 一致，否则导入时会拒绝目标平台。
"""
import os, sys, zipfile, platform

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

def main():
    if len(sys.argv) < 5:
        sys.exit(__doc__)
    plugin_name, ext_id, ext_name, version = sys.argv[1:5]
    plugin_api = sys.argv[5] if len(sys.argv) > 5 else "3"

    so_path = os.path.join(REPO, "build", f"{plugin_name}.so")
    out_dir = os.path.join(REPO, "dist")
    out_path = os.path.join(out_dir, f"{plugin_name}.vut")

    host_os = "linux"
    host_arch = platform.machine() or "x86_64"
    # glibc -> "gnu"，musl -> "linux"（与 mt_vut_package_get_host_abi 一致）
    try:
        with open("/usr/bin/ldd", "rb") as f:
            ldd = f.read()
    except OSError:
        ldd = b""
    host_abi = "gnu" if b"GLIBC" in ldd else "linux"

    manifest = f"""[Vellum Extension]
format-version=1
id={ext_id}
name={ext_name}
version={version}
plugin-api={plugin_api}
license=Unspecified
module=module.so

[Target]
os={host_os}
architecture={host_arch}
abi={host_abi}
"""

    with open(so_path, "rb") as f:
        so_data = f.read()

    os.makedirs(out_dir, exist_ok=True)

    # 标准 ZIP（Deflate），与 Vellum 自带解析器兼容（method 0/8，无 data-descriptor，CRC 正确）。
    with zipfile.ZipFile(out_path, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("vellum-extension.ini", manifest)
        zi = zipfile.ZipInfo("module.so")
        zi.compress_type = zipfile.ZIP_DEFLATED
        z.writestr(zi, so_data)

    print(f"wrote {out_path} ({os.path.getsize(out_path)} bytes), .so payload {len(so_data)} bytes")

    # 回读校验
    with zipfile.ZipFile(out_path) as z:
        names = z.namelist()
        assert "vellum-extension.ini" in names and "module.so" in names
        assert z.read("module.so") == so_data
        assert z.read("vellum-extension.ini").decode() == manifest
    print("manifest and module verified")

if __name__ == "__main__":
    main()
