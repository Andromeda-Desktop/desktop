#!/usr/bin/env python3
"""Fetches arm64 macOS Homebrew bottles of the libdesk-ui and terminal dependencies, with their runtime dependencies,
into a sysroot for cross-builds: headers, dylibs and pkg-config files under PREFIX (default ~/.cache/andromeda-mac/deps).
Also writes PREFIX/../cross.ini, the meson cross file for arm64-apple-macos26 with the xtool Darwin SDK (DARWIN_SDK).
Cairo is built from source instead: its bottle links X11 and xcb, which a Mac app never uses.
Usage: deps.py [PREFIX]"""
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request

ROOTS = ["glib", "cairo", "pango", "harfbuzz", "freetype", "fontconfig", "fribidi", "librsvg", "libxkbcommon",
         "webp", "jpeg-xl", "md4c", "nlohmann-json", "tomlplusplus"]
# Floating-point std::from_chars needs macOS 26 libc++; the bottles match the target.
TAG = "arm64_tahoe"
TARGET = "arm64-apple-macos26"
SDK = os.environ.get("DARWIN_SDK", os.path.expanduser(
    "~/.swiftpm/swift-sdks/darwin.artifactbundle/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk"))
# ANGLE (BSD-3-Clause) with the Metal backend gives macOS its EGL and GLES; kivy/angle-builder pins chromium/7151.
ANGLE_URL = "https://github.com/kivy/angle-builder/releases/download/chromium-7151_rev1/angle-macos-arm64.tar.gz"
ANGLE_SHA256 = "fea89ee72ebb3426ef8fd5ec90db463f9b06beb74ea858a1091bcef5a2e99b73"
API = "https://formulae.brew.sh/api/formula/{}.json"
PREFIX = os.path.abspath(os.path.expanduser(sys.argv[1] if len(sys.argv) > 1 else "~/.cache/andromeda-mac/deps"))
CACHE = os.path.join(os.path.dirname(PREFIX), "bottles")


def fetch(url, path, auth=False):
    req = urllib.request.Request(url, headers={"Authorization": "Bearer QQ=="} if auth else {})
    with urllib.request.urlopen(req) as r, open(path + ".part", "wb") as f:
        shutil.copyfileobj(r, f)
    os.replace(path + ".part", path)


def angle(pc_dir):
    path = os.path.join(CACHE, f"angle-{ANGLE_SHA256[:16]}.tar.gz")
    if not os.path.exists(path):
        fetch(ANGLE_URL, path)
    with open(path, "rb") as f:
        if hashlib.sha256(f.read()).hexdigest() != ANGLE_SHA256:
            sys.exit(f"{path}: sha256 mismatch")
    with tempfile.TemporaryDirectory() as tmp, tarfile.open(path) as tar:
        tar.extractall(tmp, filter="data")
        for h in ("EGL", "GLES2", "GLES3", "KHR"):
            shutil.copytree(os.path.join(tmp, "include", h), os.path.join(PREFIX, "include", h), dirs_exist_ok=True)
        os.makedirs(os.path.join(PREFIX, "share", "licenses", "angle"), exist_ok=True)
        shutil.copy(os.path.join(tmp, "LICENSE"), os.path.join(PREFIX, "share", "licenses", "angle"))
        for lib, pc in (("EGL", "egl"), ("GLESv2", "glesv2")):
            dst = os.path.join(PREFIX, "lib", f"lib{lib}.dylib")
            shutil.copy(os.path.join(tmp, f"lib{lib}.dylib"), dst)
            # The release names itself ./libX.dylib; the app bundle (step 11) puts it on the rpath and re-signs it.
            subprocess.run(["llvm-install-name-tool", "-id", f"@rpath/lib{lib}.dylib", dst], check=True)
            with open(os.path.join(pc_dir, pc + ".pc"), "w") as f:
                f.write(f"prefix={PREFIX}\nName: {pc}\nDescription: ANGLE {lib} (Metal)\nVersion: 7151\n"
                        f"Libs: -L${{prefix}}/lib -l{lib}\nCflags: -I${{prefix}}/include\n")
    print("angle chromium-7151")


def cairo(info, cross):
    """Builds the bottle's cairo version without xlib and xcb; quartz stays, as the pangocairo bottle calls it."""
    version, src = info["versions"]["stable"], info["urls"]["stable"]
    tarball = os.path.join(CACHE, os.path.basename(src["url"]))
    if not os.path.exists(tarball):
        fetch(src["url"], tarball)
    with open(tarball, "rb") as f:
        if hashlib.sha256(f.read()).hexdigest() != src["checksum"]:
            sys.exit(f"cairo: sha256 mismatch for {tarball}")
    with tempfile.TemporaryDirectory(dir="/var/tmp") as tmp:
        with tarfile.open(tarball) as tar:
            tar.extractall(tmp, filter="data")
        source, build = os.path.join(tmp, f"cairo-{version}"), os.path.join(tmp, "build")
        subprocess.run(["meson", "setup", build, source, "--cross-file", cross, "--prefix", PREFIX, "--libdir", "lib",
                        "--buildtype=release", "-Dxlib=disabled", "-Dxcb=disabled", "-Dquartz=enabled",
                        "-Dtests=disabled", "-Dspectre=disabled", "-Dsymbol-lookup=disabled", "-Dlzo=disabled",
                        "-Dglib=enabled", "-Dfontconfig=enabled", "-Dfreetype=enabled", "-Dpng=enabled"],
                       check=True, stdout=subprocess.DEVNULL)
        subprocess.run(["meson", "install", "-C", build, "--quiet"], check=True, stdout=subprocess.DEVNULL)
    print("cairo", version, "(source, no X11)")


def formula(name):
    with urllib.request.urlopen(API.format(name)) as r:
        return json.load(r)


def resolve(roots):
    seen, order = {}, []

    def visit(name):
        if name in seen:
            return
        seen[name] = formula(name)
        for dep in seen[name]["dependencies"]:
            visit(dep)
        order.append(name)

    for root in roots:
        visit(root)
    return seen, order


def main():
    os.makedirs(CACHE, exist_ok=True)
    shutil.rmtree(PREFIX, ignore_errors=True)
    os.makedirs(PREFIX)
    infos, order = resolve(ROOTS)
    for name in order:
        if name == "cairo":
            continue
        files = infos[name]["bottle"]["stable"]["files"]
        bottle = files.get(TAG) or files.get("all")
        if not bottle:
            sys.exit(f"{name}: no {TAG} bottle")
        path = os.path.join(CACHE, f"{name}-{bottle['sha256'][:16]}.tar.gz")
        if not os.path.exists(path):
            fetch(bottle["url"], path, auth=True)
        # A bottle holds NAME/VERSION/{include,lib,share}; its contents merge into PREFIX like a Homebrew prefix.
        with tempfile.TemporaryDirectory() as tmp, tarfile.open(path) as tar:
            tar.extractall(tmp, filter="data")
            (version,) = os.listdir(os.path.join(tmp, name))
            shutil.copytree(os.path.join(tmp, name, version), PREFIX, dirs_exist_ok=True, symlinks=True)
        print(name, version)
    # Bottle .pc files name @@HOMEBREW_PREFIX@@/opt/NAME and @@HOMEBREW_CELLAR@@/NAME/VERSION; all merge into PREFIX.
    pc_dir = os.path.join(PREFIX, "lib", "pkgconfig")
    pcs = {os.path.realpath(os.path.join(d, f)) for d in (pc_dir, os.path.join(PREFIX, "share", "pkgconfig"))
           if os.path.isdir(d) for f in os.listdir(d) if f.endswith(".pc")}
    for p in pcs:
        text = open(p).read()
        text = re.sub(r"@@HOMEBREW_CELLAR@@/[^/\s]+/[^/\s]+|@@HOMEBREW_PREFIX@@(/opt/[^/\s]+)?", PREFIX, text)
        text = re.sub(r"(?m)^prefix=.*$", f"prefix={PREFIX}", text)
        open(p, "w").write(text)
    # Homebrew takes these from macOS itself (uses_from_macos), so no bottle ships their .pc files.
    sdk_libs = {"zlib": "-lz", "bzip2": "-lbz2", "expat": "-lexpat", "libffi": "-lffi", "libxml-2.0": "-lxml2",
                "libcurl": "-lcurl"}
    sdk_include = {"libffi": "-I=/usr/include/ffi", "libxml-2.0": "-I=/usr/include/libxml2"}
    for name, lib in sdk_libs.items():
        with open(os.path.join(pc_dir, name + ".pc"), "w") as f:
            f.write(f"Name: {name}\nDescription: from the macOS SDK\nVersion: 99\nLibs: {lib}\n"
                    f"Cflags: {sdk_include.get(name, '')}\n")
    shutil.copytree(os.environ.get("STB_INCLUDE", "/usr/include/stb"), os.path.join(PREFIX, "include", "stb"), dirs_exist_ok=True)
    angle(pc_dir)

    # The SDK may be newer than the target (xtool ships 27): a call into a newer API must fail the build.
    flags = f"['-target', '{TARGET}', '-isysroot', '{SDK}']"
    with open(os.path.join(os.path.dirname(PREFIX), "cross.ini"), "w") as f:
        f.write(f"""[binaries]
c = ['clang'] + {flags}
cpp = ['clang++'] + {flags}
objc = ['clang'] + {flags}
objcpp = ['clang++'] + {flags}
ar = 'llvm-ar'
strip = 'llvm-strip'
pkg-config = 'pkg-config'

[built-in options]
c_args = ['-isystem', '{PREFIX}/include', '-Werror=unguarded-availability-new']
cpp_args = ['-isystem', '{PREFIX}/include', '-Werror=unguarded-availability-new']
objc_args = ['-isystem', '{PREFIX}/include', '-Werror=unguarded-availability-new']
objcpp_args = ['-isystem', '{PREFIX}/include', '-Werror=unguarded-availability-new']
c_link_args = ['-fuse-ld=lld', '-L{PREFIX}/lib']
cpp_link_args = ['-fuse-ld=lld', '-L{PREFIX}/lib']
objc_link_args = ['-fuse-ld=lld', '-L{PREFIX}/lib']
objcpp_link_args = ['-fuse-ld=lld', '-L{PREFIX}/lib']

[properties]
pkg_config_libdir = ['{PREFIX}/lib/pkgconfig', '{PREFIX}/share/pkgconfig']

[host_machine]
system = 'darwin'
subsystem = 'macos'
kernel = 'xnu'
cpu_family = 'aarch64'
cpu = 'arm64'
endian = 'little'
""")
    cairo(infos["cairo"], os.path.join(os.path.dirname(PREFIX), "cross.ini"))


if __name__ == "__main__":
    main()
