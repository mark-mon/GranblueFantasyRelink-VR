# Notes for Claude

Native VR mod for Granblue Fantasy: Relink (D3D11 + OpenXR), shipped as a `dinput8.dll` proxy. Read `README.md`, then
`docs/stereo-pipeline-notes.md` (the game's frame pipeline, addresses, hooks) before changing rendering code.

- Build: `build.sh` (clang-cl + lld-link with an xwin CRT/SDK) or `build_msvc.cmd`. Output `out/dinput8.dll`.
- The default config is the `kDefaultConfig` string in `src/config.cpp`. The release `config.txt` is generated from
  it. Keep comments there user-facing. Debug and experimental keys (quad views, debug_* such as debug_dump_shaders / debug_skip_ps, cull_margin, render_size)
  are parsed but deliberately left out of the template and the menu.
- Never touch the game's DRM (SteamStub). Hooks are in memory only; no game files are modified.
- `vrkit/` is a copy of the shared settings-menu library. Port improvements back to the shared repo.
- Releases: bump `GBVR_VERSION` / `kModVersion` in `src/dllmain.cpp`, build, zip `dinput8.dll` + `config.txt` +
  `README.txt`, tag `vX.Y`, and attach the zip to a GitHub release.
- Test PC (shared with other projects): send commands through the shared agent, file
  `D:\vrconversions\sharedtools\agent\inbox\granbluevr__<NNN_name>.cmd.ps1` (new number every time); output in
  `D:\vrconversions\granbluevr\agent\outbox\<NNN_name>.out.txt`. Rules in `D:\vrconversions\sharedtools\agent\README.txt`.
  Each command dot-sources `D:\vrconversions\granbluevr\agent\gbvr.ps1`, installs with `GB-InstallTest`, and ends with
  `GB-Cleanup` in a `finally` (stops the game and removes the mod's files from the game folder; log and test config are
  kept in `granbluevr\test\gamefolder_backup`). Don't start the game while the user says the machine is in use.
