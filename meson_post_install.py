#!/usr/bin/env python3
# Meson post-install script: recompile glib schemas and update icon cache.
import os
import subprocess
import sys

destdir = os.environ.get("DESTDIR", "")
prefix = sys.argv[1] if len(sys.argv) > 1 else "/usr/local"

datadir = os.path.join(destdir, prefix, "share")

# Recompile glib schemas
schemas_dir = os.path.join(datadir, "glib-2.0", "schemas")
if os.path.isdir(schemas_dir):
    subprocess.call(["glib-compile-schemas", schemas_dir])

# Update icon cache for hicolor
icons_dir = os.path.join(datadir, "icons", "hicolor")
if os.path.isdir(icons_dir):
    subprocess.call(["gtk-update-icon-cache", "-f", "-t", icons_dir])

# Update desktop database
apps_dir = os.path.join(datadir, "applications")
if os.path.isdir(apps_dir):
    subprocess.call(["update-desktop-database", "-q", apps_dir])
