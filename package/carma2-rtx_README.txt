Carmageddon 2 - RTX Remix compatibility proxy
=============================================

Makes Carmageddon II: Carpocalypse Now path-traceable with NVIDIA RTX Remix. The game's
hardware renderer reaches Direct3D 9 through nGlide; this d3d9.dll sits in front of RTX
Remix and hands it the game's 3D scene in a form it can ray trace.

No game files, no nGlide and no RTX Remix runtime are included. You need all three.


What is in this folder
----------------------

  d3d9.dll                the proxy
  remix-comp-proxy.ini    proxy settings, every option documented inside
  rtx.conf                RTX Remix settings and texture tags for this game
  .trex\bridge.conf       RTX Remix bridge settings (exposes the Remix API - required
                          for the headlights)
  rtx-remix\mods\carma2rtx\
                          RTX Remix mod: glowing fire and flame frames, and material
                          overrides for water, glass and chrome
  carma2-rtx_README.txt   this file (named so it cannot replace the game's ReadMe.txt)
  LICENSE, THIRD_PARTY_NOTICES.md


Requirements
------------

  - Carmageddon II: Carpocalypse Now. Developed against the GOG version, which ships
    with nGlide.
  - An RTX Remix runtime for x86 games: NVIDIA's RTX Remix, or Remix Plus 1.4 or newer.
    The proxy recognises which one it is running on (rtx_comp\console.log names it) and
    creates the headlights and the sun through it. On a runtime it does not recognise,
    it leaves the headlights and the sun off rather than risk a crash.
  - A GPU that can run RTX Remix.


Installing
----------

  1. Install the RTX Remix runtime into the game folder (the folder with CARMA2_HW.EXE).
  2. Rename the runtime's d3d9.dll in the game folder to d3d9_remix.dll.
     Leave .trex\d3d9.dll alone.
  3. Copy everything from this folder into the game folder. Let .trex\bridge.conf
     replace the runtime's copy, or add these lines to yours:

         client.forceWindowed = True
         client.DirectInput.disableExclusiveInput = True
         exposeRemixApi = True

  4. Run nglide_config.exe and set the resolution to your desktop resolution. RTX Remix
     only path-traces when the game renders at the resolution it is presented at.
  5. Start the hardware-accelerated game (CARMA2_HW.EXE).

If the game crashes on launch or the soundtrack does not play, rename CARMA2_HW.EXE to
CARMA2_HW0.EXE and start that instead. Some installs need it; the proxy works with
either name.

The game is known to fail on start-up now and then, with or without this proxy. Start it
again.

The depth-cue fog is switched off in remix-comp-proxy.ini (Fog, FogVolumetrics). That is
the tested configuration.

The proxy draws a sky built from each track's own horizon texture, and that sky is what
lights the track. If your RTX Remix runtime brings a sky of its own (a physical
atmosphere), set [Sky] Synthesize=0 in remix-comp-proxy.ini.


Keys
----

  F4       proxy menu: Headlights, Sun, and Effects (culling of crushed doors, additive
           car flames). Hold the right mouse button outside the menu to give input back
           to the game.
  H        headlights: off -> player car -> all cars -> off
  Alt+X    RTX Remix's own menu

Headlight settings are adjusted in the F4 menu and saved to carma2-headlights.ini in the
game folder with "Save to ini". The sun works the same way and saves to carma2-sun.ini.
It starts at the angle the game gives its own light: 60 degrees up, the same on every
track.


Troubleshooting
---------------

  rtx_comp\console.log in the game folder is the proxy's log.

  - "The Remix API is switched off": exposeRemixApi = True is missing from
    .trex\bridge.conf.
  - "Unrecognised Remix API table": this Remix runtime lays out its API in a way the
    proxy does not know. Everything but the headlights and the sun still works.
  - The picture is rasterized, not path-traced: nGlide's resolution does not match the
    desktop resolution.
  - The Headlights tab says why the player's car is dark when its lights are missing.


Uninstalling
------------

Delete d3d9.dll, remix-comp-proxy.ini, carma2-headlights.ini, carma2-sun.ini, rtx.conf,
carma2-rtx_README.txt, rtx-remix\mods\carma2rtx and the rtx_comp folder from the game
folder, then remove the RTX Remix runtime.
