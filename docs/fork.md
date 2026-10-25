# This fork

[darkphoenixfox/wipeout_pc](https://github.com/darkphoenixfox/wipeout_pc) is a fork of
[AbdelmoumenDeghbouche/wipeout_pc](https://github.com/AbdelmoumenDeghbouche/wipeout_pc)
("upstream"). It adds more render resolutions, HUD shape, 3D side by side and LeiaSR output
(README, *What this fork adds*), and takes upstream's fixes as they come.

## Keeping up with upstream

- **Automatically:** the *Sync upstream* workflow (`.github/workflows/sync-upstream.yml`) runs
  every day and can be started by hand (Actions > Sync upstream > Run workflow). When upstream
  has new commits it merges them into a branch `sync/upstream-<commit>` and opens a pull request
  into `main`. If they conflict, it opens an issue listing the files instead.
- **By hand** (also for a conflict), from a checkout of this fork's `main`:

  ```bash
  bash tools/sync_upstream.sh      # fetch, merge upstream/main, run the Python tests
  bash build.sh                    # MSYS2 CLANG64; then play
  git push fork HEAD:main
  ```

  The script adds the `fork` and `upstream` remotes when they are missing and turns on
  `git rerere`, so a conflict resolved once is replayed in later merges.

Rules that keep merges simple:

- **Merge, never rebase** `main`: its commits are public (pull requests, releases).
- **Fork values go in fork-only files.** The version and release page the launcher uses are in
  `launcher/fork_info.py` (`APP_VERSION`, `RELEASES_API`, `RELEASES_PAGE`); upstream's `VERSION`
  line and its tests stay as upstream writes them, so its version bumps merge by themselves.
- Sync often; the files both sides change are the launcher, `README.md`,
  `scripts/game_profiles.py`, `patches/WipEout_Omega_Collection.xml` and the Vulkan presenter.
  After a sync that touches the renderer, check 3D (side by side and LeiaSR) in a race.
- Features that suit upstream can be offered there as pull requests: the less this fork differs,
  the fewer conflicts.

## Releasing

1. Set the version in `launcher/fork_info.py` (`APP_VERSION`); release tags are
   `windows-v<APP_VERSION>`.
2. Build the LeiaSR bridge (README, *Building*; needs Visual Studio Build Tools and the LeiaSR
   SDK), then `bash build.sh` and `bash packaging/windows/package.sh` in MSYS2 CLANG64. The
   package script adds `gow3_leiasr.dll` when it was built. The *Windows zip* workflow cannot
   build the bridge (the SDK is not public), so its packages have side-by-side 3D only.
3. Check the package: `packaging/windows/smoke-test.ps1 dist/wipeout-windows.zip`.
4. Publish: `gh release create windows-v<version> dist/wipeout-windows.zip
   dist/wipeout-windows.zip.sha256 --repo darkphoenixfox/wipeout_pc` (the `.sha256` from
   `sha256sum -b wipeout-windows.zip`).

The launcher's update check looks at this fork's latest release (`launcher/fork_info.py`).
