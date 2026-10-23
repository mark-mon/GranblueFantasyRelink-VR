# Notes for Claude

Native VR mod for Granblue Fantasy: Relink (D3D11 + OpenXR), shipped as a `dinput8.dll` proxy. Read `README.md`, then
`docs/stereo-pipeline-notes.md` (the game's frame pipeline, addresses, hooks) before changing rendering code.

- Build: `build.sh` (clang-cl + lld-link with an xwin CRT/SDK) or `build_msvc.cmd`. Output `out/dinput8.dll`.
- The default config is the `kDefaultConfig` string in `src/config.cpp`. The release `config.txt` is generated from
  it. Keep comments there user-facing. Debug and experimental keys (quad views, debug_*, cull=, render_size)
  are parsed but deliberately left out of the template and the menu.
- Never touch the game's DRM (SteamStub). Hooks are in memory only; no game files are modified.
- `vrkit/` is a copy of the shared settings-menu library. Port improvements back to the shared repo.
- Releases: bump `GBVR_VERSION` / `kModVersion` in `src/dllmain.cpp`, build, zip `dinput8.dll` + `config.txt` +
  `README.txt`, tag `vX.Y`, and attach the zip to a GitHub release.
