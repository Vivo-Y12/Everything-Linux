# Flathub submission walkthrough

This document describes how to submit the project to Flathub once the
build has been verified locally. The actual publication is **not** done
here — Flathub's review team must approve every submission.

## 1. Pre-flight checklist

Before opening a PR against the Flathub repository, confirm:

- [ ] The app builds locally with `flatpak-builder` (Option A in BUILD.md).
- [ ] `flatpak-builder-lint manifest` produces no errors (warnings are OK
      if justified).
- [ ] `appstream-util validate` passes on the AppStream metadata.
- [ ] The app ID `org.voidtools.Everything` is the correct reverse-DNS
      identifier (the `org.voidtools.*` namespace reflects that this is a
      port of voidtools' MIT-licensed Everything; Flathub may ask you to
      use a different namespace if you are not affiliated with voidtools).
- [ ] The license in the AppStream metadata matches the `LICENSE` file.
- [ ] At least one icon in `scalable/` is present and matches the app ID.
- [ ] No Wine, Proton, or bundled Windows dependencies are in the build.
- [ ] No binaries from the original `Everything.exe` are shipped.

## 2. Push the source to a public git host

Flathub manifests use `type: git` to pull source from upstream. Push the
contents of `/home/z/my-project/everything-app/` to a public GitHub,
GitLab, or Codeberg repository and tag the release:

```bash
cd /home/z/my-project/everything-app
git init
git add .
git commit -m "Everything-1.0.0 native Linux port"
git tag v1.0.0
git remote add origin git@github.com:YOUR_USERNAME/everything-linux-port.git
git push -u origin main
git push origin v1.0.0
```

Get the commit SHA of the tag:

```bash
git rev-list -n 1 v1.0.0
```

## 3. Update the Flathub manifest with the real source URL

Edit `/home/z/my-project/flatpak/flathub-submission/org.voidtools.Everything.yaml`:

```yaml
sources:
  - type: git
    url: https://github.com/YOUR_USERNAME/everything-linux-port.git
    tag: 'v1.0.0'
    commit: <paste the SHA from step 2>
```

## 4. Fork flathub/flathub and open a PR

```bash
# In a web browser: fork https://github.com/flathub/flathub
# UNCHECK "Copy the master branch only" so you get the new-pr branch.

git clone --branch=new-pr git@github.com:YOUR_USERNAME/flathub.git
cd flathub
git checkout -b everything-submission new-pr
cp /home/z/my-project/flatpak/flathub-submission/org.voidtools.Everything.yaml \
   ./org.voidtools.Everything.yaml
git add org.voidtools.Everything.yaml
git commit -m "Add org.voidtools.Everything"
git push origin everything-submission
```

Open the PR on GitHub:
- Base repository: `flathub/flathub`
- Base branch: `new-pr`
- Compare branch: `your-username:everything-submission`
- Title: `Add org.voidtools.Everything`

## 5. Trigger a test build

Comment on the PR with the single line:

```
bot, build
```

The Flathub build bot will run `flatpak-builder` against your manifest
and report back. Common failures at this stage:
- A missing source URL or wrong commit SHA.
- A runtime version that doesn't exist (use `47` or whatever the latest
  stable GNOME runtime is at submission time).
- Permission objections from reviewers (be ready to justify each
  `--filesystem=` and `--talk-name=` entry).

## 6. Address reviewer feedback

Do **not** close and reopen the PR to address comments. Push new commits
to the same branch. The Flathub review team may ask you to:
- Reduce the permissions (e.g. remove `--filesystem=host:ro` if you can
  restructure to use the document portal instead — for a file indexer
  this is unlikely to be feasible).
- Switch to the latest stable runtime.
- Add a screenshots section to the AppStream metadata.
- Add additional metadata fields (developer name, content rating, etc.).

## 7. After merge

When the PR is merged, Flathub will:
- Create a new repository `flathub/org.voidtools.Everything` and invite
  you to it (requires 2FA on GitHub).
- Build and publish the app to the Flathub repo.
- Send you a notification. From then on, updates are pushed by opening
  PRs against the new app repository (not `flathub/flathub`).

## Permissions justification (paste into the PR description)

```
Permissions requested by org.voidtools.Everything:

* --filesystem=host:ro
    The app is a filesystem indexer. It must traverse the entire host
    filesystem to build the search index. Read-only is sufficient —
    file operations on results (Open, Open Path) go through xdg-open
    on the host. No host file is ever written to by the app itself.

* --filesystem=xdg-data/everything:create
    The app's SQLite index lives at $XDG_DATA_HOME/everything/index.sqlite.
    The Flatpak-wrapped location resolves to
    ~/.var/app/org.voidtools.Everything/data/everything/.

* --filesystem=xdg-config/everything:create
    The app's INI-format configuration lives at
    $XDG_CONFIG_HOME/everything/Everything.ini.

* --share=ipc, --socket=wayland, --socket=fallback-x11
    Standard windowing for a GTK4 app.

* --device=dri
    Hardware-accelerated rendering.

* --talk-name=org.freedesktop.portal.*
    Used only for the file-chooser portal invoked by the
    "Export results as CSV…" menu item.

No network access, no system bus, no device access beyond GPU rendering,
no process introspection.
```
