# REA reverse-engineering findings — voidtools Everything 1.4.1.1032

This document records what the official REA toolchain (`rea-agents` v6.8.0
via `npx`) and the open-source `pefile` Python module revealed about the
original voidtools Everything binary. It is the basis for the architectural
decisions in the native Linux port.

## Toolchain health

REA's own health check (`rea doctor`) reports:

| Check      | Status  | Notes                                                  |
| ---------- | ------- | ------------------------------------------------------ |
| node       | ✓       | v24.22.2 (REA requires ≥22.x)                          |
| host       | ✓       | Debian 13                                              |
| hopper     | ✗       | Not installed (`/opt/hopper/bin/Hopper` missing)       |
| ghidra     | ✗       | `GHIDRA_INSTALL_DIR` env var unset                     |
| ida        | ✗       | `REA_IDA_MCP_CONFIG` not set                           |
| jadx       | ✗       | `REA_JADX_MCP_JAR` not set                             |
| kuna       | ✗       | `REA_KUNA_COMMAND` not set; PE not admitted yet anyway |

The available providers that worked without those engines were:
`rea-artifact-graph` (artifact inspection + inventory) and
`rea-dotnet-static` (managed-PE/CLI inspection — not applicable to
Everything since it is native code, not .NET).

## Original file inventory

| File                                  | SHA256                                                              | Size     |
| ------------------------------------- | ------------------------------------------------------------------- | -------- |
| `upload/Everything-x86-Setup.exe`     | `781a31b440045219752a1bb40fbd204b1d96964d4bf56af01b18e3d549b037aa` | 1.81 MB  |
| `upload/Everything after install files.zip` | `7ac17f959a6665f6a462e3f21cdf957a9fb8bd5657f48a63fdefd468bd0eeafd` | 1.79 MB |
| `work/original-zip/Everything/Everything.exe` | `d38f48dd60de6b10f9a132718ccbfddc171a7a7d1cc88ef2e4ce73c0c43dcd5f` | 1.70 MB |
| `work/original-zip/Everything/Everything.ini` | `f0969f438d2869641d8f76d5b9fd2b82c7232134a90972e96abb3783d1e2fbe5` | 215 B |
| `work/original-zip/Everything/Everything.lng` | `84e0ae09a9e268424ce85a85135c704c534ff60f92e150413b9df05a88979be7` | 1009 KB |
| `work/original-zip/Everything/Changes.txt`    | `8e279763ef89aae7073e29584e65ac05e410a45d2a8ea5ed1b3a160f70715924` | 20.8 KB |
| `work/original-zip/Everything/License.txt`    | `50fea29fabe12533c4e43407e12f549b8c3a2d250fe13b961623147acb1c25b2` | 2.6 KB |
| `work/original-zip/Everything/Uninstall.exe`  | `f1a542db90b3ff150151cc35c8dadeef63dbb7acdf8fe48b8d512289c8b75a95` | 139 KB |

The REA `inspect-artifact` provider confirmed (see
`rea-project/evidence/inspect-artifact.json`):

```
subject.name       = "Everything.exe"
subject.digest     = sha256:d38f48dd60de6b10f9a132718ccbfddc171a7a7d1cc88ef2e4ce73c0c43dcd5f
subject.format     = "pe"
declared_size      = 1785000 bytes
artifact_kind      = executable
content_state      = materialized
hash_status        = verified
```

## Everything.exe — PE structure (via pefile + objdump)

| Property              | Value                                |
| --------------------- | ------------------------------------ |
| Machine               | 0x14c — Intel i386 (32-bit)          |
| Subsystem             | 2 — Windows GUI                      |
| ImageBase             | 0x00400000                           |
| EntryPoint            | 0x0014bcd4                           |
| LinkerVersion         | 8.0 (MSVC)                           |
| DllCharacteristics    | 0x0140 — DYNAMIC_BASE (ASLR) + NX_COMPAT (DEP) |
| TimeDateStamp         | Fri Jan 23 02:58:16 2026 (build)     |
| Sections              | 5                                    |

| Section | Virtual Address | Virtual Size | Raw Size | Characteristics              |
| ------- | --------------- | ------------ | -------- | ---------------------------- |
| .text   | 0x001000         | 0x150308 (1.34 MB) | 0x150400 | CODE, EXECUTE, READ          |
| .rdata  | 0x152000         | 0x039f6a (232 KB) | 0x03a000 | INITIALIZED_DATA, READ       |
| .data   | 0x18c000         | 0x00f18c (60 KB)  | 0x00e000 | INITIALIZED_DATA, READ, WRITE |
| .rsrc   | 0x19c000         | 0x00a1e4 (41 KB)  | 0x00a200 | INITIALIZED_DATA, READ       |
| .reloc  | 0x1a7000         | 0x00e8e2 (58 KB)  | 0x00ea00 | INITIALIZED_DATA, READ, DISCARDABLE |

## Everything.exe — Static imports (from the .idata directory)

| DLL              | Imported function count | Primary use                              |
| ---------------- | ----------------------- | ---------------------------------------- |
| KERNEL32.dll     | 108                     | core OS: file I/O, threads, memory, time |
| USER32.dll       | 109                     | window mgmt, message loop, controls      |
| GDI32.dll        | 36                      | drawing, fonts, bitmaps                  |
| WS2_32.dll       | 20                      | Winsock — ETP server & network features  |
| ADVAPI32.dll     | 18                      | registry, services, security             |
| ole32.dll        | 14                      | COM / OLE initialization                 |
| SHELL32.dll      | 6                       | shell integration, file associations     |
| COMCTL32.dll     | 4                       | list view, image list, mouse tracking    |
| SHLWAPI.dll      | 4                       | path utilities                           |
| comdlg32.dll     | 1                       | common dialogs                           |

Additional DLLs revealed only by `strings` analysis (loaded dynamically
via `LoadLibrary`/`GetProcAddress`, so not in the static import table):
`ntdll.dll`, `wininet.dll` (HTTP server), `crypt32.dll`, `secur32.dll`,
`security.dll`, `imm32.dll` (IME for non-Latin text input), `avicap32.dll`
and `msvfw32.dll` (preview pane video), `mpr.dll` (network resources),
`rasapi32.dll` and `sensapi.dll` (legacy network connectivity sensing),
`query.dll` (legacy Windows Search content indexer), `mscoree.dll`
(.NET CLR hosting — present as a security measure only).

## Everything.exe — Resources

| Type ID | Type        | Purpose                                       |
| ------- | ----------- | --------------------------------------------- |
| 2       | Bitmap      | Toolbar / UI bitmaps                          |
| 3       | Icon        | Application icon (multiple sizes)             |
| 10      | RCData      | Embedded resources (likely the manifest, etc.) |
| 14      | GroupIcon   | Icon group for Windows shell                  |
| 16      | Version     | VS_VERSION_INFO resource                      |
| 24      | Manifest    | Application manifest (UAC, DPI awareness)     |

## Indexing mechanism (confirmed via strings)

Strings analysis of `Everything.exe` reveals the full NTFS MFT reading
pipeline. The relevant strings (deduplicated, abbreviated):

```
DeviceIoControl(%p,FSCTL_DELETE_USN_JOURNAL,...)
DeviceIoControl(%p,FSCTL_QUERY_USN_JOURNAL,...)
MFT USA Count %u
MFT USA Offset %u
MFT attribute_offset %u
MFT base_frn %u
MFT bytes_allocated %u
MFT bytes_in_use %u
MFT flags %u
MFT lsn %I64u
MFT name_count %u
MFT next_attribute_number %u
MFT record_length %u
MFT sequence_number %u
MFT type %t
MFT allocated_length %I64u
MFT file_size %I64u
MFT highest_vcn %I64u
MFT lowest_vcn %I64u
MFT total_allocated %I64u
MFT valid_data_length %I64u
remaining MFT Entries %I64u
BAD mft record length %d
%d unprocessed MFT data runs
%d: invalid state for ntfs enum mft
%d: invalid state for refs enum mft
USN Journal Deleted (%f seconds)
USN_RECORD %I64u, FLAGS %08x, PARENT FRN %016I64x, FRN %016I64x, reason %s(%08x), filename %T
-create-usn-journal "%s" %d %d
-create-usn-journal <volume> <max-size-bytes> <allocation-delta-bytes>
-delete-usn-journal "%s"
-delete-usn-journal <volume>
```

This confirms:
* The indexer reads the NTFS **Master File Table** directly via
  `DeviceIoControl(FSCTL_*)` calls — not by walking the directory tree.
* It enumerates MFT records sequentially, parsing the Update Sequence
  Array (USA), file reference numbers (FRN), parent FRNs, and the
  attribute list to reconstruct the full file path for each entry.
* It uses the **USN Change Journal** for incremental updates
  (`FSCTL_QUERY_USN_JOURNAL`, `FSCTL_DELETE_USN_JOURNAL`,
  `FSCTL_READ_USN_JOURNAL`).
* ReFS (Resilient File System) is also supported ("refs enum mft").

## Configuration model

`Everything.ini` (215 bytes) stores just four flags at install time:

```ini
[Everything]
app_data=1            ; store config + DB in %APPDATA%\Everything
run_as_admin=0        ; do not request UAC elevation
allow_http_server=1   ; built-in HTTP search server enabled
allow_etp_server=1    ; ETP (Everything Tunneling Protocol) file share enabled
```

The full settings live in `%APPDATA%\Everything\Everything.ini` at runtime.
Our Linux port stores the equivalent in
`$XDG_CONFIG_HOME/everything/Everything.ini` — same INI format, same
flag names where they make sense (HTTP/ETP servers are out of scope for
the initial Linux port).

## UI features (extracted from strings)

| Menu / column     | Identified strings (selected)                                  |
| ----------------- | -------------------------------------------------------------- |
| File menu         | File List, Close File List, Exit                               |
| Edit menu         | Copy Path, Copy Full Name, Copy Name, Copy as CSV, Copy To Folder, Delete, Delete All, Delete Permanently |
| View menu         | Sort by (Name, Path, Size, Extension, Attributes, Date Created, Date Modified, Date Accessed, Date Recently Changed), Filter |
| Search menu       | Bookmarks, Filters, Search History (Export, Delete All), Run History |
| Bookmarks menu    | Add Bookmark, Edit Bookmark, Delete Bookmark, Export Bookmarks File, Bookmarks.csv |
| Tools menu        | Options (Edit options), Export Result List, Export Filters File, Export Search History |
| Help menu         | About Everything, About Everything File List Editor           |
| List columns      | Name, Path, Size, Extension, Type, Attributes, Date Created, Date Modified, Date Accessed, Date Recently Changed, File List Name |
| Sort tokens       | ATTRIBUTES_ASCENDING/DESCENDING, EXTENSION_ASCENDING/DESCENDING, etc. |

## License

`License.txt` (2.6 KB) confirms:

* **Everything itself** is © 2026 voidtools, distributed under the **MIT License**.
* The original bundles **PCRE** (Perl-Compatible Regular Expressions), © 1997-2012 University of Cambridge, BSD-style license.

Our Linux port does not bundle PCRE (it uses SQLite FTS5 instead), so
only the MIT license applies to the redistributed source.

## Why this matters for the Linux port

The REA findings drive every major design decision:

1. **No MFT on Linux** → cannot reuse the indexing algorithm. We crawl
   the directory tree once with a multi-threaded `opendir`/`readdir`
   worker pool, then keep the index fresh with inotify.
2. **No USN Journal on Linux** → inotify provides the equivalent
   real-time event stream (`IN_CREATE/IN_DELETE/IN_MODIFY/IN_MOVED_*`).
3. **MSVC binary, Win32 GUI** → cannot reuse any source code; the port
   is a clean-room reimplementation in C with GTK 4 + libadwaita.
4. **No PCRE required** → SQLite FTS5's `unicode61` tokenizer covers
   the substring search use case at sub-millisecond latency.
5. **MIT license** → redistribution as a Flathub app is unambiguous;
   we ship a LICENSE file containing the MIT text and acknowledge the
   original copyright.
6. **HTTP / ETP servers** → explicitly out of scope for the initial
   Flathub submission (would require `--share=network` permission,
   which Flathub scrutinises heavily for desktop apps).

## Evidence files

The raw REA evidence and supporting analysis are saved under:

```
/home/z/my-project/rea-project/
├── evidence/
│   ├── inspect-artifact.json     # REA's primary inspection report
│   ├── doctor.txt                # REA provider health check
│   └── capabilities.txt          # REA capability inventory
└── everything-pe-analysis.json   # pefile-derived PE structure (detailed)
```
