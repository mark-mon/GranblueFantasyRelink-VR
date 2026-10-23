# Technical notes

- [`stereo-pipeline-notes.md`](stereo-pipeline-notes.md): how Granblue Fantasy: Relink renders a frame (main loop,
  render-frame, deferred contexts, constant blocks, camera functions) and how stereo, HUD separation, render size and
  culling were added to it. Addresses are for the Steam build the mod was made against.
- [`vr-mod-toolkit.md`](vr-mod-toolkit.md): the game-independent techniques used here: command-list replay stereo,
  per-view temporal history, NVIDIA VRS foveation, eye gaze, shader-hash post-processing swaps, the settings menu.
  Also covers what each technique needs from a game and the pitfalls hit along the way.
