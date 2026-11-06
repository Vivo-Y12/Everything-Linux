# Everything-1.0.0 — Native Linux port of voidtools Everything

A from-scratch, native Linux reimplementation of the popular
[voidtools Everything](https://www.voidtools.com/) file-search utility,
built without Wine, Proton, or any Windows compatibility layer. Distributed
as a Flatpak for installation via Flathub.

## What this is (and what it isn't)

| Original voidtools Everything (Windows)            | Everything-1.0.0 (Linux)                         |
| -------------------------------------------------- | ------------------------------------------------ |
| Reads the NTFS **Master File Table (MFT)** via `DeviceIoControl(FSCTL_*)` for instant indexing | Walks the directory tree with a multi-threaded crawler, then keeps the index fresh with **inotify** |
| Reads the **USN Journal** for incremental updates  | Subscribes to `IN_CREATE/IN_DELETE/IN_MODIFY/IN_MOVED_FROM/IN_MOVED_TO/IN_DELETE_SELF/IN_MOVE_SELF` events |
| Win32 + Common Controls GUI                         | **GTK 4 + libadwaita** GUI                       |
| Stores config in `%APPDATA%\Everything\Everything.ini` | Stores config in `$XDG_CONFIG_HOME/everything/Everything.ini` |
| MIT-licensed                                        | MIT-licensed (this port is a clean-room reimplementation in C, not a derived work of the original source code) |

Why the architectural difference? Linux filesystems such as ext4, Btrfs and
XFS have no equivalent of the NTFS MFT: filenames live inside directory
inodes. There is no single API to enumerate every file on a volume in one
call. The Linux port therefore crawls the directory tree once — a few
seconds for a typical desktop install on an SSD — and then relies on
inotify to update the index in real time.

## Performance (measured)

| Operation                             | Time                 |
| ------------------------------------- | -------------------- |
| First-time crawl of 698 files         | ~1 second            |
| Cold search "skill" (229 results)     | 2 milliseconds       |
| Cold search `ext:md` (100 results)    | 2 milliseconds       |
| Cold search `size:>100kb` (15 results)| 2 milliseconds       |

Measured on a 2-core Intel Xeon VM with SSD-backed storage. Searches hit
SQLite FTS5 with prefix tokenisation, which delivers the same instant
feedback users expect from the original.

## Project layout

```
/home/z/my-project/
├── upload/                     # Original Windows files (DO NOT MODIFY)
│   ├── Everything-x86-Setup.exe
│   └── Everything after install files.zip
├── work/original-zip/Everything/   # Working copy of the extracted ZIP
├── rea-project/                # REA tooling workspace
├── everything-app/             # Native Linux application source tree
│   ├── src/                    # C source (config, index, search, window, main)
│   ├── tests/everything-cli.c  # Standalone CLI harness (compiles & runs without GTK4)
│   ├── data/                   # desktop entry, appstream, gschema, icons
│   ├── meson.build             # Build configuration
│   └── vendor/                 # (reserved for vendored libs, currently empty)
├── flatpak/
│   ├── org.voidtools.Everything.yaml        # Local-build manifest (uses `type: dir`)
│   └── flathub-submission/
│       └── org.voidtools.Everything.yaml    # Flathub PR manifest (uses `type: git`)
├── outputs/                    # Final deliverables land here
├── docs/
│   ├── CHANGE-REPORT.md        # Concise record of all changes
│   ├── BUILD.md                # Build & install instructions
│   └── FLATHUB.md              # Flathub submission walkthrough
└── scripts/                    # Build/test scripts
```

## Build it locally

### Prerequisites

* Ubuntu 22.04 LTS or newer (or any distro with Flatpak ≥ 1.14)
* Flatpak + flatpak-builder (or the Flathub Builder app)
* The GNOME 47 SDK and Platform runtimes
* For a non-Flatpak direct build (for development): meson, ninja, gcc,
  pkg-config, and the GTK 4 / libadwaita / SQLite dev headers

### Option A — Build as a Flatpak (recommended)

```bash
# 1. Install flatpak + add the Flathub remote
sudo apt install flatpak flatpak-builder
flatpak remote-add --user --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo

# 2. Install the GNOME 47 runtime + SDK (~1.5 GB download)
flatpak install --user flathub org.gnome.Platform//47 org.gnome.Sdk//47

# 3. Build & install the app from this manifest
cd /home/z/my-project/flatpak
flatpak-builder --user --install --force-clean build-dir org.voidtools.Everything.yaml

# 4. Run it
flatpak run org.voidtools.Everything
```

### Option B — Validate the manifest before submission

```bash
flatpak install --user flathub org.flatpak.Builder
flatpak run --command=flatpak-builder-lint org.flatpak.Builder \
    manifest /home/z/my-project/flatpak/org.voidtools.Everything.yaml
```

### Option C — Build & test the backend without GTK4

A standalone CLI harness lives in `everything-app/tests/everything-cli.c`
and compiles with just glib + SQLite (no GTK4 needed). Use it to verify
that the indexer and search engine work end-to-end on any Linux box.

```bash
cd /home/z/my-project
bash scripts/build_cli.sh               # compiles build/everything-cli

./everything-app/build/everything-cli -r /home index       # build an index
./everything-app/build/everything-cli -r /home status       # show stats
./everything-app/build/everything-cli -r /home search "todo"
./everything-app/build/everything-cli -r /home search "ext:py size:>10kb"
./everything-app/build/everything-cli -r /home -j search "*.c"   # JSON output
```

## Supported search syntax

A deliberate, useful subset of voidtools Everything's query syntax:

| Syntax              | Meaning                                         |
| ------------------- | ----------------------------------------------- |
| `word1 word2`       | Both must match the filename (default AND)      |
| `word1 \| word2`    | Either matches (OR)                             |
| `"a phrase"`        | Exact phrase match                              |
| `!word`             | Exclude files containing `word`                 |
| `*.txt`             | Wildcard glob on filename                       |
| `ext:txt`           | Exact extension match                           |
| `ext:!txt`          | Extension is NOT txt                            |
| `path:foo`          | Match `foo` anywhere in the parent path         |
| `folder:`           | Only directories                                |
| `file:`             | Only regular files                              |
| `empty:`            | Only zero-byte files                            |
| `size:>1mb`         | Size filter; `<`, `<=`, `>`, `>=`, `==`; kb/mb/gb |
| `dm:lastweek`       | Date modified: today / yesterday / thisweek / lastweek / thismonth / lastmonth / thisyear |
| `dm:>=2024-01-01`   | Absolute date with comparison operator          |
| `dc:`, `da:`        | Same for ctime / atime                          |

## UI features

* Header bar with prominent search entry (live-as-you-type, 80 ms debounce)
* Sortable results columns: Name, Path, Size, Type, Modified, Attributes
* Right-click / menu actions: Open, Open Path, Copy Path, Copy Name,
  Copy Full Name
* Tools menu: Rebuild index (Ctrl+R), Export results as CSV…
* Status bar: index state + result count, live during crawl

## Configuration

Settings are stored in `$XDG_CONFIG_HOME/everything/Everything.ini`
(within the Flatpak sandbox that maps to
`~/.var/app/org.voidtools.Everything/config/everything/Everything.ini`).
All values are also exposed as GSettings keys under
`org.voidtools.Everything.*` — use `gsettings set` to change them
while the app is running, or edit the INI file when the app is closed.

The SQLite index lives at `$XDG_DATA_HOME/everything/index.sqlite`.
A full-rebuild wipe is safe — the app rebuilds on next launch.

## Permissions explained (for Flathub reviewers)

| Permission                              | Why                                    |
| --------------------------------------- | -------------------------------------- |
| `--filesystem=host:ro`                  | Read-only filesystem traversal for the indexer |
| `--filesystem=xdg-data/everything:create`  | Writable per-app dir for the SQLite index |
| `--filesystem=xdg-config/everything:create`| Writable per-app dir for settings      |
| `--share=ipc`, `--socket=wayland`, `--socket=fallback-x11` | Windowing |
| `--device=dri`                          | Hardware-accelerated rendering         |
| `--talk-name=org.freedesktop.portal.*`  | File chooser portal for CSV export    |

The app does **not** require: network, audio, system bus, process listing,
DConf system-wide access, or any host service.

## Known limitations

* First-time crawl of very large filesystems (>1M files) can take tens
  of seconds. Subsequent launches reuse the persisted index.
* The Linux default `fs.inotify.max_user_watches` is 8192, which is too
  low for full-disk indexing. The app will continue to work but logs a
  one-time warning recommending:
  ```
  sysctl fs.inotify.max_user_watches=524288
  ```
* Network filesystems (NFS, SMB) are not actively excluded; inotify
  does not deliver events for them, so the index may go stale. Use the
  Tools → Rebuild index menu item to refresh.
* Matched-substring highlighting in the results list is not yet
  implemented (results show the plain filename).
* HTTP server and ETP server features of the original are not ported;
  they are out of scope for the initial Flathub submission.

## License

MIT — same license as the original voidtools Everything. See
`everything-app/LICENSE` for the full text, including the bundled PCRE
notice (the original Windows app bundles PCRE; this Linux port uses
SQLite's built-in FTS5 instead of PCRE, so PCRE is not redistributed).

## Acknowledgements

Original voidtools Everything is © 2026 voidtools, distributed under the
MIT License. This native Linux port is a clean-room reimplementation
based on observed behaviour and on the publicly-documented Everything
query syntax; it does not incorporate or derive from the original
binary or its source code.
