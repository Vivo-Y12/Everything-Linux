# Build & install instructions

## Prerequisites

* Ubuntu 22.04 LTS or newer (or any Debian-based distro with GTK 4.10+)
* `sudo` access for installing build dependencies
* ~200 MB free disk for dev headers

## Option 1 — Build the .deb package

```bash
# 1. Install build dependencies
sudo apt update
sudo apt install -y meson ninja-build gcc pkg-config \
    libgtk-4-dev libadwaita-1-dev libsqlite3-dev \
    libglib2.0-dev gettext

# 2. Build
meson setup build --prefix=/usr
ninja -C build

# 3. Create the .deb
bash scripts/build_deb.sh
# Output: outputs/Everything-linux-1.0.0-amd64.deb

# 4. Install
sudo apt install ./outputs/Everything-linux-1.0.0-amd64.deb
```

## Option 2 — Build the AppImage (portable, self-contained)

```bash
# Same build deps as Option 1, plus:
sudo apt install -y libglib2.0-dev gettext

# Build the AppImage (downloads linuxdeploy + appimagetool automatically)
bash scripts/build_appimage.sh
# Output: outputs/Everything-linux-1.0.0-x86_64.AppImage
```

## Option 3 — Direct install (no package, for development)

```bash
sudo apt install -y meson ninja-build gcc pkg-config \
    libgtk-4-dev libadwaita-1-dev libsqlite3-dev \
    libglib2.0-dev gettext

meson setup build --prefix=/usr/local
ninja -C build
sudo ninja -C build install
glib-compile-schemas /usr/local/share/glib-2.0/schemas/
gtk-update-icon-cache -f -t /usr/local/share/icons/hicolor
update-desktop-database -q /usr/local/share/applications

everything
```

## Standalone CLI backend (no GUI deps)

Compiles with just glib + SQLite — useful for testing the indexer on a
headless box or in CI.

```bash
bash scripts/build_cli.sh
./build/everything-cli -r $HOME index
./build/everything-cli search "todo"
```

## Verify the manifest metadata

```bash
python3 scripts/validate_manifest.py
```

Checks AppStream XML, desktop entry, and GSettings schema for structural
correctness.
