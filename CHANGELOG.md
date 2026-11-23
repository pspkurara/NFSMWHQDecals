# Changelog

## 1.0.1 - 2026-11-24

- Fixed: changing the screen mode while using REDUX could make the game's Reset fail, because the extra normal targets were not
  freed first. They are now freed before every Reset.
- Fixed: the normal targets are now kept per render target, not per size, so two targets of the same size no longer share one.
- Fixed: with `HotReload`, a key that you delete from the INI goes back to its default value.

## 1.0.0 - 2026-11-21

First release.

- Gives number decals and body, side and window decals a mild reflection and a soft highlight.
- Works with the game's own car shader (pixel shader 1.1) as it is.
- Works with the car shader of REDUX (pixel shader 3.0), unofficially. There the gloss also follows the shape of the panel under the
  decal. `Compensate` (on by default) makes the highlight look the same as with the game's own shader.
- Five looks in the INI for each decal group: original, gloss, metallic, chrome and matte. Switch by taking `;` off one look and
  putting it in front of the other.
- `HotReload` reads the INI again every second, in the garage and in a race.
- Writes no log file unless `Log=1`.
