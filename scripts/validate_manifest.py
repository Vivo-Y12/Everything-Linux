#!/usr/bin/env python3
"""
Static structural validation of the Everything release metadata
(AppStream, desktop entry, gschema) — no flatpak dependency.
requires PyGObject, not installable in this sandbox).

This is NOT a substitute for the official Flathub linter; it only
catches the most common authoring mistakes. The user should run the
real linter on their host:


"""
import sys
import re
import xml.etree.ElementTree as ET
from pathlib import Path

try:
    import yaml  # PyYAML
except ImportError:
    print("PyYAML not installed; trying pip install pyyaml", file=sys.stderr)
    import subprocess
    subprocess.check_call([sys.executable, "-m", "pip", "install", "--quiet", "pyyaml"])
    import yaml

ROOT = Path("/home/z/my-project")
MANIFESTS = [
    
]
APPDATA = ROOT / "everything-app" / "data" / "org.voidtools.Everything.appdata.xml.in"
DESKTOP = ROOT / "everything-app" / "data" / "org.voidtools.Everything.desktop.in"
GSCHEMA = ROOT / "everything-app" / "data" / "org.voidtools.Everything.gschema.xml"

errors = []
warnings = []

def err(msg): errors.append(msg)
def warn(msg): warnings.append(msg)

# -------------------- Manifest checks --------------------
for path in MANIFESTS:
    print(f"\n--- Validating manifest: {path.relative_to(ROOT)} ---")
    text = path.read_text()
    try:
        m = yaml.safe_load(text)
    except yaml.YAMLError as e:
        err(f"{path.name}: invalid YAML: {e}")
        continue

    if not isinstance(m, dict):
        err(f"{path.name}: top-level must be a mapping")
        continue

    # Required keys
    required = ["app-id", "runtime", "runtime-version", "sdk", "command", "modules"]
    for k in required:
        if k not in m:
            err(f"{path.name}: missing required key '{k}'")

    # app-id format (reverse-DNS, GApplication-compatible)
    # Each dot-separated segment must contain only [a-zA-Z0-9-];
    # the first segment's first char must be a letter.
    aid = m.get("app-id", "")
    if not re.match(r"^[a-zA-Z][a-zA-Z0-9-]*(\.[a-zA-Z0-9-]+)+$", aid):
        err(f"{path.name}: app-id '{aid}' is not a valid GApplication-style reverse-DNS ID")

    # Filename must match app-id
    if path.stem != aid:
        warn(f"{path.name}: filename '{path.stem}' != app-id '{aid}' (Flathub requires match)")

    # runtime-version should be a string
    rv = m.get("runtime-version")
    if rv is not None and not isinstance(rv, str):
        warn(f"{path.name}: runtime-version should be quoted string, got {type(rv).__name__}")

    # finish-args sanity
    fa = m.get("finish-args", [])
    if not isinstance(fa, list):
        err(f"{path.name}: finish-args must be a list")
    else:
        for arg in fa:
            if not arg.startswith("--"):
                err(f"{path.name}: finish-arg '{arg}' doesn't start with --")
            # Discourage broad permissions
            if arg in ("--filesystem=host", "--filesystem=home"):
                # We use host:ro which is more conservative
                if "ro" not in arg:
                    warn(f"{path.name}: '{arg}' is broad — Flathub may push back; consider :ro")

    # Modules
    mods = m.get("modules", [])
    if not mods:
        err(f"{path.name}: no modules defined")
    for i, mod in enumerate(mods):
        if not isinstance(mod, dict):
            err(f"{path.name}: module[{i}] is not a mapping")
            continue
        if "name" not in mod:
            err(f"{path.name}: module[{i}] missing 'name'")
        if "buildsystem" not in mod:
            warn(f"{path.name}: module[{i}] has no buildsystem (defaults to autotools)")
        if "sources" not in mod:
            err(f"{path.name}: module[{i}] has no sources")
            continue
        for j, src in enumerate(mod.get("sources", [])):
            if not isinstance(src, dict):
                err(f"{path.name}: module[{i}].sources[{j}] is not a mapping")
                continue
            if "type" not in src:
                err(f"{path.name}: module[{i}].sources[{j}] missing 'type'")
            t = src.get("type")
            if t == "git":
                if "url" not in src:
                    err(f"{path.name}: git source missing url")
                if not str(src.get("url","")).startswith(("https://", "git@")):
                    warn(f"{path.name}: git url '{src.get('url')}' looks unusual")
            elif t == "dir":
                p = src.get("path")
                if p and not (ROOT / p.lstrip("../")).resolve().is_dir():
                    # Only warn — paths are resolved at runtime
                    warn(f"{path.name}: dir source path '{p}' not found from {ROOT}")
            elif t == "archive":
                for k in ("url", "sha256"):
                    if k not in src:
                        err(f"{path.name}: archive source missing {k}")

    # cleanup
    cu = m.get("cleanup", [])
    if not isinstance(cu, list):
        err(f"{path.name}: cleanup must be a list")

    print(f"  app-id: {aid}")
    print(f"  runtime: {m.get('runtime')}//{m.get('runtime-version')}")
    print(f"  command: {m.get('command')}")
    print(f"  finish-args: {len(fa)} entries")
    print(f"  modules: {len(mods)}, sources: {sum(len(m.get('sources',[])) for m in mods)}")

# -------------------- AppStream checks --------------------
print(f"\n--- Validating AppStream: {APPDATA.relative_to(ROOT)} ---")
try:
    tree = ET.parse(APPDATA)
    root = tree.getroot()
except ET.ParseError as e:
    err(f"AppStream: invalid XML: {e}")
else:
    if root.tag != "component":
        err(f"AppStream: root must be <component>, got <{root.tag}>")
    if root.get("type") != "desktop-application":
        err(f"AppStream: component type must be 'desktop-application'")
    cid = root.find("id")
    if cid is None or cid.text != "org.voidtools.Everything":
        err(f"AppStream: <id> must be 'org.voidtools.Everything'")
    if root.find("metadata_license") is None:
        err(f"AppStream: missing <metadata_license>")
    if root.find("project_license") is None:
        err(f"AppStream: missing <project_license>")
    if root.find("name") is None:
        err(f"AppStream: missing <name>")
    if root.find("summary") is None:
        err(f"AppStream: missing <summary>")
    if root.find("description") is None:
        err(f"AppStream: missing <description>")
    if root.find("launchable") is None:
        err(f"AppStream: missing <launchable>")
    if root.find("releases") is None:
        warn("AppStream: no <releases> section")
    if root.find("url[@type='homepage']") is None:
        warn("AppStream: no homepage URL")
    print(f"  id: {cid.text if cid is not None else '?'}")
    print(f"  license: {(root.find('project_license') or ET.Element('x')).text}")
    print(f"  launchable: present" if root.find('launchable') is not None else "  launchable: MISSING")

# -------------------- Desktop entry checks --------------------
print(f"\n--- Validating desktop entry: {DESKTOP.relative_to(ROOT)} ---")
desktop_text = DESKTOP.read_text()
required_keys = [("Type", "Application"), ("Name", None), ("Exec", None),
                 ("Icon", "org.voidtools.Everything"), ("Categories", None)]
for key, expected in required_keys:
    m = re.search(rf"^{key}\s*=\s*(.*)$", desktop_text, re.MULTILINE)
    if not m:
        err(f"desktop entry: missing '{key}='")
        continue
    val = m.group(1)
    if expected and val != expected:
        err(f"desktop entry: {key}='{val}' (expected '{expected}')")
    print(f"  {key}={val}")

# -------------------- GSchema checks --------------------
print(f"\n--- Validating GSchema: {GSCHEMA.relative_to(ROOT)} ---")
try:
    tree = ET.parse(GSCHEMA)
    root = tree.getroot()
except ET.ParseError as e:
    err(f"GSchema: invalid XML: {e}")
else:
    if root.tag != "schemalist":
        err(f"GSchema: root must be <schemalist>, got <{root.tag}>")
    schemas = root.findall("schema")
    if not schemas:
        err("GSchema: no <schema> elements")
    for s in schemas:
        sid = s.get("id")
        if sid != "org.voidtools.Everything":
            err(f"GSchema: schema id '{sid}' should be 'org.voidtools.Everything'")
        path = s.get("path")
        if not path or not path.startswith("/org/voidtools/Everything"):
            err(f"GSchema: schema path '{path}' should be '/org/voidtools/Everything/'")
    print(f"  schemas: {len(schemas)}")

# -------------------- Summary --------------------
print("\n" + "=" * 60)
print(f"VALIDATION SUMMARY")
print("=" * 60)
print(f"Errors:   {len(errors)}")
print(f"Warnings: {len(warnings)}")
if errors:
    print("\nERRORS:")
    for e in errors:
        print(f"  ! {e}")
if warnings:
    print("\nWARNINGS:")
    for w in warnings:
        print(f"  ? {w}")
sys.exit(1 if errors else 0)
