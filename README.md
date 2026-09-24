# Carmageddon 2 RTX Remix

An RTX Remix compatibility mod for Carmageddon 2: Carpocalypse Now (`CARMA2_HW.EXE`).

Carmageddon 2 transforms and lights everything on the CPU through BRender, so what reaches the
graphics driver is already in screen space, which Remix skips. This mod is a `d3d9.dll` proxy
that hooks BRender's model renderer (`BrZbModelRender`) while the vertices are still in model
space, and submits them to Remix with separate world, view and projection transforms.

```
CARMA2_HW.EXE (BRender) -> Glide -> nGlide -> d3d9.dll (this mod) -> d3d9_remix.dll (RTX Remix)
```

## Features

- Static scenery baked into per-texture chunks that Remix keeps between frames
- The game's own render of injected models suppressed, so it isn't drawn over the path-traced frame
- Frustum culling disabled and a longer far plane for path tracing
- Tyre tracks and other decals lifted off the road, with a separate translucent pass
- Sparks rebuilt as billboards
- Car rear light panels showing the right cell of their texture atlas
- Smoke and dust tinted by vertex colour and faded by material opacity
- The game's back-face culling handed to Remix, so car glass stays single-sided
- The race's depth cue passed on as fog and to Remix's volumetrics
- A patch for the game's own out-of-bounds crash in the tint-poly code when pressing ESC
- Headlights: two Remix spot lights per car. H cycles off, player car, all cars. Tuned from the F4 menu and saved to `carma2-headlights.ini`
- F4 debug overlay

All options are documented in `remix-comp-proxy.ini`.

## Requirements

- Carmageddon 2: Carpocalypse Now (developed against the GOG version; no game files are included)
- nGlide (included with the GOG version)
- RTX Remix runtime

## Installing

1. Install the RTX Remix runtime into the game folder, then rename its `d3d9.dll` to `d3d9_remix.dll`.
2. Copy everything from a release into the game folder: `d3d9.dll`, `remix-comp-proxy.ini`, `rtx.conf` and
   `.trex\bridge.conf`, which sets `exposeRemixApi = True` (the headlights are created through the Remix API).
   `carma2-rtx_README.txt` in the release has the details.
3. Start `CARMA2_HW.EXE`. If the game crashes on launch or the soundtrack does not play, rename it to `CARMA2_HW0.EXE` and start that; the proxy works with either name.

## Building

Needs Visual Studio 2022 with the C++ x86 toolset.

```bat
build.bat
package.bat
```

`build.bat` builds `build\bin\release\d3d9.dll`. `package.bat` builds and assembles `release\`, exactly what
a player copies into the game folder, from the DLL and the files in `package\`.

## Repository layout

```
src/comp/modules/brender_inject.*   BRender hooks and Remix submission
src/comp/modules/headlights.*       car headlights through the Remix API, their menu and ini
src/comp/game/                      Carmageddon 2 addresses, structures and the ESC crash patch
src/comp/, src/shared/              remix-comp-proxy framework
deps/                               vendored dependencies
package/                            release files: proxy settings, rtx.conf, .trex/bridge.conf, player README,
                                    and the Remix mod (rtx-remix/mods/carma2rtx: emissive maps, material overrides)
findings.md                         research notes
kb.h                                knowledge base for the reverse engineering tools
save-run.ps1                        saves a finished run's logs and settings into runs\<name>
```

## Credits

Built on the remix-comp-proxy framework by [xoxor4d](https://github.com/xoxor4d) and
[kim2091](https://github.com/kim2091), from the
[Vibe Reverse Engineering](https://github.com/Ekozmaster/Vibe-Reverse-Engineering) toolkit.
Third-party components are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## License

MIT, see [LICENSE](LICENSE).
