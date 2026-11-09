<h1 align="center">Everything Linux</h1>
<p align="center">Native Linux port of voidtools Everything</p>

## How to install

Download the latest release from GitHub:

**https://github.com/Vivo-Y12/Everything-Linux/releases/latest**

Choose the format that fits your system:

### Option 1 — Debian/Ubuntu package (.deb)  *(recommended)*

Download **`Everything-linux-1.0.0-amd64.deb`** from the release page.

Double-click the file → Ubuntu App Center opens → click **Install** → done.

Dependencies (`libgtk-4-1`, `libadwaita-1-0`, `libsqlite3-0`) are installed automatically via apt.

After install, open the **Applications** menu → search "Everything" → click to launch.

### Option 2 — AppImage (portable, no install needed)

Download **`Everything-linux-1.0.0-x86_64.AppImage`** from the release page.

Right-click → **Properties** → **Permissions** → tick **"Allow executing file as program"**.

Double-click to run. No install required, no extra downloads.


## What this is

Everything Linux is a native file-search utility for Linux desktops. It indexes the filesystem in seconds using a multi-threaded crawler, then provides instant substring search across the whole index. A SQLite FTS5 backend delivers sub-millisecond results, and inotify keeps the index fresh in real time as files are created, modified, or deleted.

The GUI is built with GTK 4 + libadwaita, providing a modern, GNOME-integrated experience: a prominent search bar in the header, a sortable results view, and a status bar showing live indexing progress.

## Performance (measured)

| Operation                             | Time                 |
| ------------------------------------- | -------------------- |
| First-time crawl of 698 files         | ~1 second            |
| Cold search "skill" (229 results)     | 2 milliseconds       |
| Cold search `ext:md` (100 results)    | 2 milliseconds       |
| Cold search `size:>100kb` (15 results)| 2 milliseconds       |

Searches hit SQLite FTS5 with prefix tokenisation, delivering the same instant feedback users expect.

## Supported search syntax

A deliberate, useful subset of Everything's query syntax:

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
* Right-click / menu actions: Open, Open Path, Copy Path, Copy Name, Copy Full Name
* Tools menu: Rebuild index (Ctrl+R), Export results as CSV…
* Status bar: index state + result count, live during crawl

## Configuration

Settings are stored in `$XDG_CONFIG_HOME/everything/Everything.ini` and are also exposed as GSettings keys under `org.voidtools.Everything.*` — use `gsettings set` to change them while the app is running, or edit the INI file when the app is closed.

The SQLite index lives at `$XDG_DATA_HOME/everything/index.sqlite`. A full-rebuild wipe is safe — the app rebuilds on next launch.

## Known limitations

* First-time crawl of very large filesystems (>1M files) can take tens of seconds. Subsequent launches reuse the persisted index.
* The Linux default `fs.inotify.max_user_watches` is 8192, which is too low for full-disk indexing. The app will continue to work but logs a one-time warning recommending:
  ```
  sysctl fs.inotify.max_user_watches=524288
  ```
* Network filesystems (NFS, SMB) are not actively excluded; inotify does not deliver events for them, so the index may go stale. Use the **Tools → Rebuild index** menu item to refresh.
* Matched-substring highlighting in the results list is not yet implemented (results show the plain filename).

## License

MIT — see `LICENSE` for the full text.

## Acknowledgements

Original voidtools Everything is © 2026 voidtools, distributed under the MIT License. This native Linux port is a clean-room reimplementation based on observed behaviour and on the publicly-documented Everything query syntax; it does not incorporate or derive from the original binary or its source code.
