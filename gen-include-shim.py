#!/usr/bin/env python3
"""Creates build/include-shim: symlinks that fix the letter case of #include names
(the code was written on case-insensitive Windows)."""
import os, re, sys

import glob, subprocess
root = os.path.dirname(os.path.abspath(__file__))
srcdir = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.path.join(root, "src")
app = os.path.join(srcdir, "SmashSoda")
shim = os.path.join(srcdir, "include-shim")

def mingw_include_dirs():
    """System include directories of the MinGW-w64 C++ compiler (they differ between distributions)."""
    try:
        r = subprocess.run(["x86_64-w64-mingw32-g++", "-E", "-Wp,-v", "-xc++", "-"], input="", capture_output=True, text=True)
        dirs = [l.strip() for l in r.stderr.splitlines() if l.startswith(" /")]
        if dirs: return [d for d in dirs if os.path.isdir(d)]
    except OSError:
        pass
    return ["/usr/x86_64-w64-mingw32/include"] + glob.glob("/usr/x86_64-w64-mingw32/include/c++/*")
sysincs = mingw_include_dirs()

search = [app] + [os.path.join(app, d) for d in os.listdir(app) if os.path.isdir(os.path.join(app, d))] + [
    os.path.join(srcdir, "external"),
    os.path.join(srcdir, "Dependencies", "parsecsdk"),
    os.path.join(srcdir, "Dependencies", "matoya"),
    os.path.join(srcdir, "Dependencies", "vigem", "include"),
    os.path.join(srcdir, "Dependencies", "sdl", "include"),
] + sysincs
for p in list(search):
    if os.path.isdir(p):
        for dp, dn, _ in os.walk(p):
            pass

def exists(rel, base):
    return os.path.exists(os.path.join(base, rel))

def ci_find(rel, base):
    """Case-insensitive resolve of a relative path under base; returns real path or None."""
    cur = base
    for part in rel.replace("\\", "/").split("/"):
        if part in ("", "."):
            continue
        if part == "..":
            cur = os.path.dirname(cur); continue
        try:
            names = os.listdir(cur)
        except OSError:
            return None
        if part in names:
            cur = os.path.join(cur, part)
        else:
            m = [n for n in names if n.lower() == part.lower()]
            if not m:
                return None
            cur = os.path.join(cur, m[0])
    return cur

inc_re = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.M)
names = set()
for dp, dn, fn in os.walk(app):
    if "include-shim" in dp:
        continue
    for f in fn:
        if f.endswith((".h", ".cpp", ".hpp", ".c", ".inl")):
            try:
                txt = open(os.path.join(dp, f), encoding="utf-8-sig", errors="ignore").read()
            except OSError:
                continue
            names.update(inc_re.findall(txt))

os.makedirs(shim, exist_ok=True)
made = 0
for n in sorted(names):
    n = n.replace("\\", "/")
    if any(exists(n, b) for b in search):
        continue
    target = None
    for b in search:
        t = ci_find(n, b)
        if t and os.path.exists(t):
            target = t; break
    if not target:
        # strip leading ../ components (relative includes) and retry on basename dirs
        base = os.path.basename(n)
        for b in search:
            t = ci_find(base, b)
            if t and os.path.exists(t):
                target = t; break
    if not target:
        print("unresolved:", n)
        continue
    dst = os.path.join(shim, n)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if os.path.lexists(dst):
        os.remove(dst)
    os.symlink(target, dst)
    made += 1
print(f"{made} shim links in {shim}")
