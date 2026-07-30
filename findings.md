# Carmageddon 2 — RTX Remix port: static analysis findings

Target: `<game dir>\CARMA2_HW.EXE` (x86 PE, base 0x00400000)
Ghidra project: `patches/Carmageddon2/ghidra/Carmageddon2.gpr` (CARMA2_HW.EXE, hardware_d3d.bdd, carma2.exe)
KB: `patches/Carmageddon2/kb.h`

## 1. Renderer selection

`carma2.exe` is only a launcher. It stores the choice under `Software\SCI\Carmageddon2`
(`Driver`, `Flags`, `DevGuid`, `DrvGuid`) and spawns one of:

```
carma2_sw.exe
carma2_hw.exe                          <- 3dfx / Glide (default; GOG ships nGlide)
carma2_hw.exe -d3d
carma2_hw.exe -d3d -afe
carma2_hw.exe -d3d -noblend
carma2_hw.exe -d3d -zombie ...
```

`WinMain` = **0x0051AAA0**. Argument handling (uppercased, `strcmp`):

| arg | effect |
|---|---|
| `-D3D` | `PTR_DAT_006621DC = "D3D"` (0x00662518) — BRender device name; default is `"3DFX_WIN"` (0x006622B8) |
| `-AFE` | `DAT_0074D3DC = 1` |
| `-ZOMBIE` | `FUN_004D6F50()` |
| `-ALIEN` | `FUN_004D6F70()` |
| `-NOCUTSCENE(S)` | `DAT_0068B894 = 1` |
| `-SCALEMOUSE` | `DAT_006AD468 = 1` |

`-noblend` has no matching string in the EXE — the launcher passes it but the game ignores it.

**Status on Windows 11:** default (Glide) launch works. `-d3d` crashes immediately with
`0xC000041D` (STATUS_FATAL_USER_CALLBACK_EXCEPTION) — an exception thrown inside a window
callback during DirectDraw/Direct3D init. The game's D3D backend is **Direct3D Immediate Mode
(pre-DX8)**: `DIRECT3D_HAL` / `DIRECT3D_RGB` / `DIRECT3D_RAMP` device strings, `DDRAW` device
name, `SSDXStop(): Releasing DDraw2 interface...`. There is no DX9 anywhere in this game.

## 2. Why the game's own D3D output is useless to RTX Remix

BRender does **all transform & lighting in software**. The device driver only ever receives
screen-space vertices. Proof from the sibling Glide driver: `hardware_3dfx.bdd` imports
`_grDrawTriangle@12` — Glide triangles are screen-space (x, y, ooz, oow, s, t). The D3D driver
is the same renderer with a `D3DTLVERTEX`/execute-buffer backend.

So even after wrapping DirectDraw to D3D9, Remix would only see `D3DFVF_XYZRHW` geometry with
no usable camera. The interception has to happen inside BRender, before T&L.

## 3. The BRender pipeline (all inside CARMA2_HW.EXE)

```
BrZbSceneRender              0x00522F30  (world, camera, colour_pmap, depth_pmap)
 └ BrZbSceneBeginFrameSetup  0x00522C80  viewport scale/offset from colour pixelmap
    └ SceneSetupCameraMatrices 0x00521C10
       ├ BrCameraToScreenMatrix4 0x0051E520 -> PROJECTION (br_matrix4, 64B)
       │    partSet(BRT_MATRIX 0x76, 0, 0xEE view_to_screen, &m4)
       │    partSet(BRT_MATRIX 0x76, 0, 0xF0 camera_type, 0xF1 persp | 0xF2 parallel)
       └ walks camera->parent chain -> WORLD->VIEW (br_matrix34, 48B)
            g_camera_matrix_stack (0x0079F07C), stride 56, entry [0] = final world_to_view
            partSet(BRT_MATRIX 0x76, 0, 0xEA model_to_view, &stack[0])
 └ BrZbActorRender            0x005221E0  recursive actor walk
    │  renderer->modelMul(+0xA4, actor->transform)   accumulates model_to_view
    │  renderer->boundsTest(+0xD0) -> 0x113/0x114/0x115
    └ BrZbModelRender          0x00521890
       └ g_pfnModelRenderStyleTable[style & 0xFF]   table at 0x00665090, 8 entries
          └ ModelRenderStyle_Faces 0x00525FC0   <<< HOOK HERE >>>
             ├ model->stored (+0x50) ? stored->_render(stored, g_pRenderer)      [vtbl +0x44]
             └ else  g_pGeometryV1Model->_render(geom, g_pRenderer,
                                                model->prepared (+0x4C),
                                                material, BRT_TYPE)              [vtbl +0x44]
                     ^ software T&L happens inside here
 └ BrZbBucketFlushAndSwap     0x00522EB0  bucket flush + renderer->flush(+0xFC)
```

Renderer object: `g_pRenderer` @ **0x0079EFEC**. Dispatch offsets:
`+0x80 partSet`, `+0x84 partQuery`, `+0x8C templateQuery`, `+0x90 queryMany`,
`+0xA4 modelMul`, `+0xB4 modelIdentity`, `+0xB8/+0xBC statePush/statePop`,
`+0xD0 boundsTest`, `+0xFC flush`.

Matrix tokens: `BRT_MATRIX = 0x76`, `model_to_view = 0xEA` (48B), `view_to_screen = 0xEE` (64B).

## 4. Where the untransformed vertices live

`BrModelUpdate` = **0x0051F950**. It builds `model->prepared` (+0x4C) from the authored
`model->vertices` / `model->faces`. Layouts (see `kb.h` for full structs):

* `br_vertex`, stride **0x28**: `p` @0x00 (model space), `map` @0x0C, RGB @0x15, normal @0x1C
* `br_face`, stride **0x28**: `vertices[3]` @0x00, `material` @0x08, normal @0x18, `d` @0x24
* `br_model`: `vertices` @0x08, `faces` @0x0C, `nvertices` @0x10, `nfaces` @0x12,
  `pivot` @0x14, `flags` @0x20, `radius` @0x30, `bounds` @0x34, `prepared` @0x4C, `stored` @0x50

`model->prepared` holds `ngroups` (@0x08) groups (@0x18, stride 0x24), each:

| off | field |
|---|---|
| 0x00 | material |
| 0x08 | face colours |
| 0x0C | face vertex-index array |
| 0x10 | **vertex array, stride 0x20** |
| 0x14 | vertex colours |
| 0x18 | vertex source-index array (u16) |
| 0x1C | nfaces |
| 0x1E | nvertices |

Prepared vertex (0x20 bytes) — **exactly a D3D9 FFP vertex**:

```
+0x00  float px, py, pz    model space, minus model->pivot
+0x0C  float u, v
+0x14  float nx, ny, nz
```

→ `D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1`, no conversion needed, one group = one draw call
with a single material.

**To confirm at runtime:** `BrModelUpdate` only builds `model->stored` when
`g_pRenderer && g_pGeometryV1Model && (model->flags & 0x800)`. If C2 models don't set 0x800,
`ModelRenderStyle_Faces` always takes the `prepared` branch. Trace 0x00525FC0 and check whether
`[model+0x50]` is ever non-null before committing to one path.

## 5. Open decision

The game has no D3D9 device of its own to proxy. Two ways to give Remix one:

* **A — DirectDraw→D3D9 wrapper** (DxWrapper `dd7to9`): makes `-d3d` boot on Win11 and puts a
  real `d3d9.dll` in the loader path. Adds a third-party layer; its draws are still `XYZRHW`,
  so our hook must replace them, not augment them.
* **B — self-contained injected DLL**: create our own `IDirect3DDevice9` on the game's HWND
  directly against Remix's `d3d9.dll`, keep the game on the (working) Glide backend, suppress
  its presentation, and drive Remix entirely from the BRender hook.
  Injection point: the EXE imports `IFORCE2.dll`, `winmm.dll` and `wsock32.dll`, all of which
  ship locally in the game folder — clean DLL-hijack targets.

Route B is fewer moving parts and does not depend on getting the broken DDraw path working.

---

## 6. Effects: tyre tracks, sparks, car rear lights (2026-07-30)

### 6.1 Render styles decide what BrZbModelRender draws

`g_pfnModelRenderStyleTable` @ **0x00665090**, indexed by `style & 0xFF`:

| style | value | thunk | primitive token |
|---|---|---|---|
| `BR_RSTYLE_DEFAULT` | 0 | 0x00525FC0 | 0x10B (triangles) |
| `BR_RSTYLE_NONE` | 1 | 0x005260E0 | bare `ret 0x18` |
| `BR_RSTYLE_POINTS` | 2 | 0x00526090 | 0xD1 |
| `BR_RSTYLE_EDGES` | 3 | 0x00526040 | 0x10A (lines) |
| `BR_RSTYLE_FACES` | 4 | 0x00525FC0 | 0x10B |
| bounding variants | 5-7 | 0x005260F0 / 0x00526270 / 0x005262D0 | |

`style` is BrZbModelRender's **5th** parameter, read at 0x00521A51 and masked with 0xFF.
Both recursive actor walkers (`BrZbActorRender` 0x005221FC and 0x00522510) return early when
the resolved style is NONE, so a hidden actor never reaches BrZbModelRender — the NONE thunk
is only a safety net. Scanning `mov byte [reg+0x20], imm` shows the game writes render_style
4 (45x), 1 (53x), 0 (11x), 3 (2x), 6 and 7 once each, and **never 2**.

### 6.2 Sparks are EDGES-style models — zero-area triangles

`InitLineAndSmokeStuff` @ **0x0047E610** builds one reusable line object:

* `g_line_model` @ 0x0074CAC8 — `BrModelAllocate("gLine_model", 2, 1)`, flags 0x12,
  `faces[0].vertices = {0, 0, 1}` — a **degenerate** face encoding one segment
* `g_line_material` @ 0x0074CA4C — flags 0x1007 (LIGHT|PRELIT|SMOOTH|0x1000), no texture
* `g_line_actor` @ 0x0074CA34 — `render_style = 3` (EDGES)

`DrawLine3D` @ **0x004F6B80** — when `g_lines_as_3d_models` (0x0074CF68) is set it writes the
two endpoints into `g_line_model->vertices`, calls `BrModelUpdate(model, 1)` and
`BrZbSceneRenderAdd(g_line_actor)`; otherwise it falls back to a 2D framebuffer blit. The
console log's `flat colour: 'gLine_model'` line proves the 3D path is the live one.

The spark emitter is at **0x004F7776**: a 33-entry array (base 0x006A9B80, stride 0x40,
gated by the bitmask at 0x006AA57C) whose per-particle colour is written straight into the
line model's vertex RGB by **0x004F7CB0**.

**Why they vanished:** BRender's edge renderer walks face edges, so `{0, 0, 1}` is a valid
line. Fed to D3D9 as a triangle it has zero area and rasterizes nothing. The proxy now routes
EDGES-style models into a segment list and expands each into a camera-facing quad, colouring
it from the authored vertex RGB.

### 6.3 Ground decals are co-planar quads, and translucent

`InitSpillsAndSkids` @ **0x004E9C40** creates a **ring of 100 actors** (`g_ground_decal_ring`
@ 0x006A27F0, stride 0x1C, `[0]` = `br_actor*`), each owning its own
`BrModelAllocate(NULL, 4, 2)` — a unit quad in the **XZ plane at y = 0**, UVs 0..1, model
flags |= 2 (KEEP_ORIGINAL). Actors start at `render_style = NONE`; the allocator at
**0x004EA1A0** sets `render_style = DEFAULT`, assigns the material and advances
`g_ground_decal_next` (0x006A27E8) modulo 100. This is what lays down tyre tracks, oil spills,
smears and car shadows. `InitImpactDecals` @ **0x004EA880** is the same idea for 50 XY-plane
quads with the "BANG!" material.

`MaterialNeedsAlpha` @ **0x0051F630** is BRender's own translucency test — colour_map type in
{0x0D, 0x0E, 0x12, 0x18, 0x19, 0x1A, 0x1F}, or `index_shade` set, or the `extra` token list
containing 0xBE/0xBF. `BrZbModelRender` calls it at **0x0052196D** to decide whether a model
goes into the depth-sorted translucent bucket instead of straight to the rasterizer, so it is
also the right authority for the proxy.

**Why they looked wrong:** the proxy guessed translucency from the pixelmap type alone, drew
everything in scene-walk order, and left `D3DRS_ZWRITEENABLE` on. A decal's fully transparent
texels therefore claimed depth, and the road behind them failed the test — the hole the user
saw. On top of that the quads are exactly co-planar with the road: BRender got away with it
through bucket ordering, but a path tracer has no draw order. Fixed by taking translucency
from `MaterialNeedsAlpha`, drawing opaque geometry first and translucent geometry afterwards
with depth writes off and alpha test on, and lifting translucent vertices along their normals
by `[Effects] DecalOffset`.

### 6.4 Car rear lights are a texture atlas selected by map_transform

`br_material::map_transform` is a **br_matrix23 at offset 0x24** (6 floats, row-vector 2x3
affine UV transform). Confirmed twice:

* **0x00445D76** writes m[0][0]=1, m[0][1]=0, m[1][0]=0, m[1][1]=1, m[2][0]=-scroll, m[2][1]=0
  then calls `BrMaterialUpdate(mat, 0x7FFF)` — a scrolling texture
* **0x00478930** compares a frame's br_matrix23 (frame table stride 0x18) against
  material+0x24/0x30/0x34/0x38, `BrMatrix23Copy`s it in on mismatch, then calls
  `BrMaterialUpdate(mat, 1)` — so `BR_MATU_MAP_TRANSFORM == 1`

The funkotronic spec proves the intent. From `EAGLE3.TXT` inside `DATA/CARS/eagle3.TWT`:

```
START OF FUNK
EARLITL          <- material: left rear light panel
constant
piss off
no fucking lighting bastards
frames
accurate
texturebits      <- frames are sub-rectangles, not separate pixelmaps
VB
4
EBACKALL,2,0,2,0 <- pixelmap, x-divisions, x-index, y-divisions, y-index
EBACKALL,2,1,2,0
EBACKALL,2,0,2,1
EBACKALL,2,1,2,1
END OF FUNK
```

`EBACKALL` is a **2x2 atlas holding all four states** (off, braking, reversing, both) and the
funk picks a quadrant by UV transform. The proxy ignored `map_transform` and explicitly set
`D3DTTFF_DISABLE`, so the whole atlas was mapped onto the light panel — all four states
visible at once. Fixed by folding `map_transform` into `D3DTS_TEXTURE0` with `D3DTTFF_COUNT2`,
refreshed every time the model is captured. Note D3D9 expands a 2-component texcoord to
(u, v, 1), so the br_matrix23 translation row maps to the D3D matrix's **third** row.

### 6.5 A spark's orange is a two-colour gradient, not a colour (2026-07-30)

`SetLineColour` @ **0x004F7CB0** takes a flag in `cl` and writes both of `gLine_model`'s
vertices, then calls `BrModelUpdate(model, 0x7FFF)`:

| | vertex[0] (+0x15) | vertex[1] (+0x3D) |
|---|---|---|
| `cl != 0` | `ff` `ff` `ff` | `ff` `ff` `ff` |
| `cl == 0` | `ff` `00` `00` | `ff` `ff` `00` |

So a spark is **red at one end and yellow at the other** — the orange is the gradient
between them, and neither endpoint alone is orange. The write offsets also confirm
`br_vertex` independently: `red` at +0x15 and a stride of 0x28 (`0x3D - 0x15`), matching
the struct the proxy already uses, and `br_model::vertices` at +0x08.

**Why they looked wrong:** the proxy sampled `colour_of(from)` only and shaded the whole
streak with it, so every spark came out flat `ff0000`. Both ends are now read and the pair
selects a streak texture that carries the gradient along its length. Brightness along the
length is deliberately left flat — an alpha taper would bias the very colours this is
reproducing. The falloff across the width stays, since a quad has hard edges where BRender
drew a one-pixel line.
