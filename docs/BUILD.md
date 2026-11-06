# Build & install instructions

## Prerequisites

* Ubuntu 22.04 LTS or newer (or any distro with Flatpak ≥ 1.14)
* `sudo` access for installing system packages (Flatpak daemon)
* ~3 GB free disk space for the GNOME 47 runtime + SDK

## Option A — Flatpak install (recommended end-user flow)

```bash
# 1. Install flatpak and add the Flathub remote
sudo apt update
sudo apt install -y flatpak flatpak-builder
flatpak remote-add --user --if-not-exists flathub \
    https://dl.flathub.org/repo/flathub.flatpakrepo

# Restart your session so flatpak paths are picked up, or run:
export PATH=$PATH:/var/lib/flatpak/exports/bin

# 2. Install the GNOME 47 runtime + SDK (one-time, ~1.5 GB download)
flatpak install --user -y flathub org.gnome.Platform//47 org.gnome.Sdk//47

# 3. Build & install the app
cd /home/z/my-project/flatpak
flatpak-builder --user --install --force-clean \
    build-dir org.voidtools.Everything.yaml

# 4. Run it
flatpak run org.voidtools.Everything
```

On first launch the app will crawl the configured roots (default
`/,/home`) and build the SQLite index. A typical desktop install with
~500 k files finishes in 5–10 seconds on an SSD. Subsequent launches
reuse the persisted index.

## Option B — Direct (non-Flatpak) build for development

This is the fastest loop for hacking on the source.

```bash
sudo apt install -y meson ninja-build gcc pkg-config \
    libgtk-4-dev libadwaita-1-dev libsqlite3-dev \
    libglib2.0-dev gettext

cd /home/z/my-project/everything-app
meson setup build --prefix=/usr/local
ninja -C build
sudo ninja -C build install
glib-compile-schemas /usr/local/share/glib-2.0/schemas/
gtk-update-icon-cache -f -t /usr/local/share/icons/hicolor
update-desktop-database -q /usr/local/share/applications

# Run it
everything
```

## Option C — Standalone CLI backend (no GUI)

Compiles with just glib + SQLite. Useful for testing the indexer on a
headless box or in CI.

```bash
cd /home/z/my-project
bash scripts/build_cli.sh

# Build an index of your home dir
./everything-app/build/everything-cli -r /home index

# Show index stats
./everything-app/build/everything-cli status

# Run searches
./everything-app/build/everything-cli search "todo"
./everything-app/build/everything-cli search "ext:py size:>10kb"
./everything-app/build/everything-cli -j search "*.c"   # JSON output
```

## Verifying the manifest

```bash
flatpak install --user -y flathub org.flatpak.Builder
flatpak run --command=flatpak-builder-lint org.flatpak.Builder \
    manifest /home/z/my-project/flatpak/org.voidtools.Everything.yaml

flatpak run --command=appstream-util org.flatpak.Builder \
    validate /home/z/my-project/everything-app/data/org.voidtools.Everything.appdata.xml.in
```

## Building a `.flatpak` bundle for offline distribution

```bash
cd /home/z/my-project/flatpak
flatpak-builder --user --repo=repo --force-clean \
    build-dir org.voidtools.Everything.yaml
flatpak build-bundle repo /home/z/my-project/outputs/Everything-1.0.0.flatpak \
    org.voidtools.Everything
```

The resulting `.flatpak` file can be installed offline with:

```bash
flatpak install --user Everything-1.0.0.flatpak
```

## Troubleshooting

| Symptom                                        | Fix                                  |
| ---------------------------------------------- | ------------------------------------ |
| First launch is slow / hangs on "Indexing…"    | Wait — full crawl of `/` is one-time |
| `Error: inotify add watch failed`              | `sudo sysctl fs.inotify.max_user_watches=524288` |
| Search returns 0 results for everything        | Check `~/.var/app/org.voidtools.Everything/data/everything/index.sqlite` exists |
| App launches but window is blank               | Try `flatpak run --env=GDK_BACKEND=x11 org.voidtools.Everything` |
