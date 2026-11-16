# Pictures of the settings window

The right-hand column of every game's settings window (`Sources/KleptonSettingsWindow.swift`)
shows what a setting does with a picture. They are general, the same for every game: one scene,
shown with and without the setting. `gen_xcodeproj.py` copies these image sets into each app's
asset catalog.

- The scene (`tools/settings_help/scene-base.jpg`) was generated with ChatGPT's image generation
  for this window on 2026-11-16, on the owner's request: an original space-station observation
  deck, no game's art, no people, no logos.
- Every picture here is made from it by `tools/settings_help/make_pictures.swift`
  (`swiftc -O -o /tmp/mp tools/settings_help/make_pictures.swift && /tmp/mp tools/settings_help/scene-base.jpg <out>`):
  coarser and finer resampling, a coarse edge around a fine middle, nearest-neighbour stairs
  against smooth edges, a dark rim for a trimmed view, a portal over a blurred room, and drawn
  strips of pictures for frame rate and frame generation. They illustrate the idea; they are not
  screenshots of a game.
