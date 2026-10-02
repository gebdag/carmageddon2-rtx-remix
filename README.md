# Carmageddon 2 RTX Remix

An RTX Remix compatibility mod for Carmageddon 2: Carpocalypse Now (`CARMA2_HW.EXE`).

<img width="480" alt="c2_transp" src="https://github.com/user-attachments/assets/c9ff94bb-d63d-438c-bd4b-ef3309fd76d0" />

<img width="1920" alt="Carmageddon II(carma2_hw exe) Screenshot 2026 10 02 - 01 29 14 63" src="https://github.com/user-attachments/assets/465702d3-cec2-4f9f-8c31-e5204113f0f4" />
<img width="1920" alt="Carmageddon II(carma2_hw exe) Screenshot 2026 10 02 - 01 30 28 58" src="https://github.com/user-attachments/assets/df2c280b-42cc-4acb-8459-909d113eac74" />
<img width="1920" alt="Carmageddon II(carma2_hw exe) Screenshot 2026 10 02 - 01 31 17 61" src="https://github.com/user-attachments/assets/7b66170b-4a45-41f0-9471-55980380cb78" />
<img width="1920" alt="Carmageddon II(carma2_hw exe) Screenshot 2026 10 02 - 01 34 22 30" src="https://github.com/user-attachments/assets/ff2830b0-40b9-4ac0-83f9-55f3e45671fc" />
<img width="1920" alt="Carmageddon II(carma2_hw exe) Screenshot 2026 10 02 - 01 44 44 29" src="https://github.com/user-attachments/assets/d25feb57-d428-410a-a10e-b06b85850f58" />


Features:
- Full RTX Remix compatible rendering of the game
- Full sky handling (vanilla skybox or Numos via RTX Remix Plus)
- Emissive maps for some of the game's textures, shipped as a Remix mod
- Working car head, brake and reversing lights
- Illuminated street and traffic lights as well as checkpoints
- Path traced water and glass materials
- F4 in game settings panel for adjusting aspects of rendering
- Widescreen support
- Option to steer with arrow keys (camera on Numpad)

```
CARMA2_HW.EXE (BRender) -> Glide -> nGlide -> d3d9.dll (this mod) -> d3d9_remix.dll (RTX Remix)
```

All options are documented in `remix-comp-proxy.ini`.

## Requirements

- Carmageddon 2: Carpocalypse Now (developed against the GOG version; Steam version should also work)
- nGlide (included with the GOG & Steam version)
- RTX Remix runtime: NVIDIA's RTX Remix, or Remix Plus 1.4 or newer for dynamic Numos sky. The proxy recognises which one it is running on and choses accordingly. 

## Installing

1. Install the RTX Remix runtime into the game folder, then <b>rename its `d3d9.dll` to `d3d9_remix.dll`</b>.
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
src/comp/modules/sun.*              the game's sun as a Remix distant light, its menu and ini
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
