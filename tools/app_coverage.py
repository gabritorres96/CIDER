#!/usr/bin/env python
"""Static coverage of a macOS application against what ORVYN implements. Nothing is executed: llvm-objdump reads the Mach-O file.

usage: python -I tools/app_coverage.py <App.app | executable> --orvyn <orvyn.exe> --objdump <llvm-objdump.exe> [--json out.json]

Reports: architecture slices, linked libraries (and which of them ORVYN provides), Swift usage, and per library the imported symbols / classes that ORVYN
implements versus the ones it does not. "Implemented" means a host function or class exists, not that its behaviour matches Apple's.
"""
import argparse, collections, json, os, plistlib, re, subprocess, sys


def run(cmd):
    return subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace").stdout


def main_executable(path):
    if os.path.isdir(path):
        info = os.path.join(path, "Contents", "Info.plist")
        exe = None
        if os.path.exists(info):
            exe = plistlib.load(open(info, "rb")).get("CFBundleExecutable")
        macos = os.path.join(path, "Contents", "MacOS")
        return os.path.join(macos, exe) if exe else os.path.join(macos, sorted(os.listdir(macos))[0])
    return path


def imports(objdump, exe):
    out = []
    text = run([objdump, "--macho", "--arch=x86_64", "--chained-fixups", exe])
    for m in re.finditer(r"lib_ordinal = (\d+) \(([^)]*)\)\n\s+weak_import = (\d)\n\s+name_offset = \d+ \((.*)\)", text):
        out.append((m.group(2), m.group(4), m.group(3) == "1"))
    if out:
        return out
    text = run([objdump, "--macho", "--arch=x86_64", "--bind", "--lazy-bind", "--weak-bind", exe])
    for line in text.splitlines():
        parts = line.split()
        if len(parts) >= 7 and parts[2].startswith('0x'):
            out.append((parts[-2], parts[-1], False))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("app")
    ap.add_argument("--orvyn", required=True)
    ap.add_argument("--objdump", required=True)
    ap.add_argument("--json")
    a = ap.parse_args()
    exe = main_executable(a.app)
    implemented = collections.defaultdict(set)
    classes = set()
    for line in run([a.orvyn, "sdk-scan", "@implemented"]).splitlines():
        if "\t" in line:
            lib, name = line.split("\t", 1)
            (classes if lib == "class" else implemented[lib]).add(name)
    all_impl = set().union(*implemented.values())
    imps = imports(a.objdump, exe)
    by_lib = collections.OrderedDict()
    for lib, sym, weak in imps:
        by_lib.setdefault(lib, []).append(sym)
    headers = run([a.objdump, "--macho", "--private-headers", exe])
    slices = re.findall(r"^Mach header|architecture (\S+)|cputype \(CPU_TYPE_(\w+)\)", headers, re.M)
    deps = run([a.objdump, "--macho", "--arch=x86_64", "--dylibs-used", exe]).splitlines()[1:]
    report = {"executable": os.path.basename(exe), "imports": len(imps), "swift_libraries": [d.strip().split(" ")[0] for d in deps if "libswift" in d], "libraries": {}}
    total_ok = 0
    for lib, syms in by_lib.items():
        ok, missing = [], []
        for s in syms:
            m = re.match(r"_OBJC_CLASS_\$_(.*)", s)
            good = (m.group(1) in classes) if m else (s in all_impl)
            (ok if good else missing).append(s)
        total_ok += len(ok)
        report["libraries"][lib] = {"imported": len(syms), "implemented": len(ok), "missing": len(missing), "missing_sample": missing[:12], "implemented_names": ok}
    report["implemented_total"] = total_ok
    print(f"{report['executable']}: {len(imps)} imported symbols, {total_ok} implemented by ORVYN; Swift libraries linked: {len(report['swift_libraries'])}")
    for lib, r in sorted(report["libraries"].items(), key=lambda kv: -kv[1]["imported"]):
        print(f"  {lib:34s} imported {r['imported']:5d}  implemented {r['implemented']:4d}")
    if a.json:
        json.dump(report, open(a.json, "w"), indent=1)


main()
