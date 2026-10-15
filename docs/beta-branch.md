# Beta branch and Windows shared data

The public beta fork is https://github.com/SoulToxic3119/citron-nextendo and the
branch is `codex/beta54-3-qlaunch-review`. The same commits are mirrored to the
existing upstream PR18 for continuity. It remains draft, not officially merged.

For later changes, fetch NextendoNetwork/main, merge into the beta, resolve the
conflicts, rebuild and repeat automated/manual acceptance before publishing.

```sh
git remote add upstream https://github.com/NextendoNetwork/citron-nextendo.git
git fetch upstream main
git switch codex/beta54-3-qlaunch-review
git merge upstream/main
# resolve, build, test, then push the beta branch to your fork
```

## Windows data and first run

Normal packages do **not** include `user`. The existing path manager therefore
uses `%APPDATA%\citron`, shared between executable versions. Detect a nonempty
qt-config.ini before QtConfig initializes/saves it. Existing configuration,
imported keys, firmware or game folders prevent automatic setup. Older settings
without setupWizardSeen are migrated without reopening the wizard. Tools > Setup
Wizard / --setup-wizard remains available explicitly. The state is saved in the
active data folder, not beside every new executable.

Keep intentional portable installations compatible. Do not delete an old `user`
directory or silently merge it into Roaming. Unpack the normal beta into a fresh
application folder. Cached homebrew belongs under `homebrew/unpacked`; preparation
copies verified tools to the active virtual SD, without creating portable data.

## Beta artifact CI

Run **Build Windows beta artifact** manually on the beta branch, supplying a
public desktop OAuth application identifier, never an account token. It builds
and uploads a 14-day artifact. It does not publish releases, modify LATEST_VERSION,
post to Discord or deploy servers. It reuses the existing Windows build procedure.
The manual workflow still requires a full CI build acceptance on the fork; its
presence does not establish a successful run.

`tools/package-beta-windows.py` copies allowed runtime files, source and licenses,
without account data or a portable user folder. Optional homebrew cache files are
checked against homebrew_setup.inc hashes. Supply --binary for a separately built
review executable. The original beta54.3 archive remains a historical portable
package; the corrected normal package is labelled beta54.3.1.
