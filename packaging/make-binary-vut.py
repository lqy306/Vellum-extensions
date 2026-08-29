#!/usr/bin/env python3
import os, zipfile, platform

REPO = "/home/lqy/项目/30_计算机/30-01_一些AI编程项目/Vellum-extensions"
SO = os.path.join(REPO, "build", "ai-completion-plugin.so")
OUT = os.path.join(REPO, "dist", "ai-completion-plugin.vut")

# Match mt_vut_package_get_host_*() exactly, or the import rejects the target.
host_os = "linux"
host_arch = platform.machine() or "x86_64"
# glibc -> "gnu", other libc (musl) -> "linux"
try:
    with open("/usr/bin/ldd", "rb") as f:
        ldd = f.read()
except OSError:
    ldd = b""
host_abi = "gnu" if b"GLIBC" in ldd else "linux"

manifest = f"""[Vellum Extension]
format-version=1
id=io.github.vellum.ai-completion
name=AI Completion
version=0.3.0
plugin-api=2
license=BSD-2-Clause
module=module.so

[Target]
os={host_os}
architecture={host_arch}
abi={host_abi}
"""

with open(SO, "rb") as f:
    so_data = f.read()

os.makedirs(os.path.dirname(OUT), exist_ok=True)

# Standard ZIP (Deflate) is accepted by Vellum's custom parser (method 0 or 8,
# no data-descriptor flag, correct CRC).
with zipfile.ZipFile(OUT, "w", zipfile.ZIP_DEFLATED) as z:
    z.writestr("vellum-extension.ini", manifest)
    zi = zipfile.ZipInfo("module.so")
    zi.compress_type = zipfile.ZIP_DEFLATED
    z.writestr(zi, so_data)

print(f"wrote {OUT} ({os.path.getsize(OUT)} bytes), .so payload {len(so_data)} bytes")

# sanity: read back
with zipfile.ZipFile(OUT) as z:
    names = z.namelist()
    assert "vellum-extension.ini" in names and "module.so" in names
    assert z.read("module.so") == so_data
    assert z.read("vellum-extension.ini").decode() == manifest
print("verified: archive round-trips, module matches build/ai-completion-plugin.so")
