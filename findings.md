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

`EBACKALL` is a **2x2 atlas holding all four states** (frames 0-3: off, reverse, brake, both; section 45) and the
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

### 6.6 Decal lifting has to be scoped to the decal pools (2026-07-30)

The lift added in 6.3 keyed off `MaterialNeedsAlpha`, i.e. every translucent material. Glass
is translucent, so a car's windows were displaced along their own vertex normals too. On a
flat ground quad that is a pure translation; on a closed shell like a windscreen it is a
deformation, and where the normals face inward it contracts the shell — the glass pulls away
from the frame and gaps open all round it. Visible with RT on or off, since it is the
injected geometry that changes.

The game has exactly two decal pools, both built once at startup and recycled thereafter,
each entry beginning with a `br_actor*` whose `model` (+0x18) is the quad:

| pool | base | stride | count | built by |
|---|---|---|---|---|
| ground — tracks, spills, smears, shadows | `0x006A27F0` | `0x1C` | 100 | `InitSpillsAndSkids` @ 0x004E9C40 |
| impact — "BANG!" marks | `0x006A55D8` | `0x78` | 50 | `InitImpactDecals` @ 0x004EA880 |

Ranges confirmed from the loop bounds: the ground walker runs to `0x006A32E0`
(`0xAF0 / 0x1C` = 100) and the impact walker counts down from `0x32`.

Membership of these pools is what "is a decal" means in this game — nothing else lays a quad
flat onto another surface — so `is_decal_model` walks them directly and only those models are
lifted. Translucency still selects the blended pass; it just no longer moves anything.

---

## 7. Performance: what has been ruled out (2026-07-30)

Frame rate is ~20fps in a race. **The three effect fixes in section 6 are not the cause.**
Measured with `TranslucentPass=0 TextureTransform=0 Sparks=0`, which restores the proxy's
pre-fix behaviour on all three paths, and the frame rate did not recover. The startup line
records the switch states, so any log says what it was measuring.

Also already ruled out, from the earlier optimization pass: static batching, per-model
buffer caching and the culling bubble were all tried and shelved as ineffective or worse.
`MergeStaticGeometry` is off by default as a result.

### What the numbers say

A race scene reports roughly:

```
861 models, 2821 draws, 39206 verts, 0 segments | submit 2.56 ms | geometry cached 285
```

`submit_ms` is the proxy's own D3D9 submission and sits at 2.5-7 ms, which does not account
for a 50 ms frame on its own. The gap between that and `frame_ms` is where the time is going,
and nothing currently measures it.

### Leads worth taking first

* **2821 draws per scene, and every D3D9 call crosses the Remix bridge by IPC.** That is the
  one number far outside what the geometry justifies -- 861 models producing 2821 draws means
  the per-material split is fragmenting nearly every model into three or more runs. Draw count,
  not vertex count, is what an IPC boundary charges for.
* **`submit_ms` excludes `capture_model`**, which runs inside the game's scene walk for every
  model, every frame, and is not timed. Timing the capture side separately from the submit
  side would say which half the cost is in, and no current log distinguishes them.
* **`readable()` is a `VirtualQuery`, and it is called from hot paths.** `refresh_part_state`
  does one per part per model per frame when `TextureTransform=1`, and `is_decal_model` does
  up to 150 per model first seen. Neither explains the floor on its own -- the frame rate did
  not recover with the first switched off -- but both are real and worth removing regardless.

---

## 9. Performance rework: sealed static chunks (2026-07-31)

The per-phase timers (section 7's follow-up logging) split a heavy scene's 32.6 ms as:
`game 9.45` (BRender software T&L + nGlide raster of models Remix discards),
`submit 9.03` (4085 proxy draws across the bridge), `other ~12.8` (game logic, roughly
constant), with the Remix server visibly per-instance-bound on top (GPU 29% at 4849
instances vs 52% at 1106). Every scaling term is per-model-draw count, so the fix is to
stop paying per model:

* **Per-actor placement tracking.** The old merge keyed placements by `br_model`; instanced
  scenery (one lamppost model, dozens of actors) read as "the model moved" every frame and
  never merged. Records are now keyed by `br_actor`.
* **Placement identity is the transform chain, not a reconstructed matrix.** model_to_world
  is recovered as model_to_view x view^-1, and at city-scale translations the float error
  of that round trip exceeds any workable epsilon -- with a moving camera *nothing* passed
  a 1e-3 matrix compare, which also silently crippled the old merge. The record now stores
  an FNV hash over the actor's parent chain (node address + t_type + t bytes per node).
  Nothing writes a static actor's transform, so equality is exact; physics rewrites bytes
  and reparenting changes addresses, so both read as movement.
* **Sealed chunks instead of rebuilds.** Promoted scenery accumulates CPU-side into
  per-(texture, blend) chunks; when promotions go quiet for 30 scenes the chunks seal:
  uploaded once into managed buffers that are never modified again, so Remix hashes them
  once and keeps their BLAS. Demotion (actor moved / deformed / material became animated)
  punches the actor's index ranges out with degenerate triangles -- a narrow partial IB
  lock, not a rebuild. No hitches by construction.
* **Frustum culling disabled** (`DisableFrustum=1`): the bounds-test hook downgrades every
  OUTSIDE to PARTIAL, so the whole level is walked, tracked and baked from the first frames
  of a race -- no shadow pop-in and no light leak at any camera angle. The far plane and
  bubble stop mattering for geometry coverage; FarPlane only shapes the projection handed
  to Remix.
* **`SuppressGameRender=1` by default, scoped to chunk-covered geometry.** Two scoping
  bugs shaped this. First, the old flag suppressed models in the 3D HUD widget scenes,
  which are never submitted to Remix -- blanking them is why the flag was shelved. Second,
  suppressing every injected model blanked pedestrian limbs: not everything a model draws
  passes through the BrZbModelRender hook (ped limbs are drawn inside the ped's own render
  call), and Remix *composites* the game's rasterized stream, so that stream was the only
  thing carrying such geometry. Suppression therefore applies exactly to models drawn from
  a live sealed chunk; dynamics keep their game render (~1 ms for the handful of movers).
  `game` still drops from ~9.5 ms to ~1 and the nGlide draws for the static world vanish
  off the bridge. Models with a custom render callback (flags & 0x20) are additionally
  barred from baking, so callback-drawn extras can never be silenced.
* **Animated materials stay dynamic.** BrMaterialUpdate (0x00520E70, __cdecl, confirmed in
  section 8) is hooked; a map_transform update (bit 0) on a material learn_materials already
  knows marks it animated -- scrolling water, flashing signs -- and demotes anything baked
  with it. Load-time BrMaterialUpdate calls never trigger this because learn_materials only
  runs later, from BrModelUpdate.
* **Race changes reset the static world**: a different world actor at submit, or most
  tracked actors going unseen (with culling disabled, absence means gone), clears chunks
  and records.

---

## 8. Calling conventions for hookable BRender functions (2026-07-31)

All three are **`__cdecl` (caller cleans)**. Every `ret` in each function is a bare `C3` — there
is no `ret N` anywhere in any of them, and every call site cleans its own arguments. They are
safe to declare as `__cdecl` function pointers and to detour with a `__cdecl` trampoline.

### 8.1 `BrMaterialUpdate` @ `0x00520E70` — `void __cdecl (br_material *material, br_uint_16 flags)`

Function extent `0x00520E70 .. 0x0052145A`. **Exactly one exit**, and it is bare:

```
0x0052144F: 5B 5D 5E 5F           pop ebx / pop ebp / pop esi / pop edi
0x00521453: 81 C4 0C 01 00 00     add esp, 0x10c        ; local frame only
0x00521459: C3                    ret                   ; bare — no ret N
```

Argument slots (prologue is `sub esp,0x10c` then `push edi/esi/ebp/ebx`, so the stack-arg
offsets shift by 0x11C from entry):

```
0x00520EAD: 8B B4 24 28 01 00 00  mov esi, [esp+0x128]  ; arg0 = material
0x00520EE3: 8A 9C 24 24 01 00 00  mov bl,  [esp+0x124]  ; arg1 = flags (LOW BYTE ONLY)
```

Call-site cleanup — 88 of the 136 xrefs do `add esp, 8` on the instruction directly after the
call, the rest defer it by one or two scheduled instructions. No site cleans 0 bytes:

```
0x00445DF7: 52                    push edx              ; material (from global 0x67C4E0)
0x00445DF8: E8 73 B0 0D 00        call 0x520e70
0x00445DFD: 83 C4 08              add esp, 8            ; caller cleans 8 = 2 args

0x00478985: 57                    push edi              ; flags
0x00478986: 52                    push edx              ; material
0x00478987: E8 E4 84 0A 00        call 0x520e70
0x0047898C: 83 C4 08              add esp, 8

0x00406FDD: 68 FF 7F 00 00        push 0x7fff           ; BR_MATU_ALL
0x00406FE2: 57                    push edi
0x00406FE3: E8 88 9E 11 00        call 0x520e70
0x00406FE8: A1 2C 77 67 00        mov eax, [0x67772c]   ; cleanup scheduled one insn later
0x00406FED: 83 C4 08              add esp, 8
```

Callee-saved `ebx/esi/edi/ebp` are all pushed and popped, so a detour only has to preserve
its own state.

#### Flags bits actually tested

`flags` is only ever read as a **byte**, at `[esp+0x124]` / `[esp+0x138]`. Bits 8-15 of the
`0x7FFF` that callers pass are ignored. Seven bits are tested, in ascending order:

| bit | test site | what it pushes to the renderer |
|-----|-----------|-------------------------------|
| `0x01` | `0x00520EEA` | `lea edx,[esi+0x24]` -> `&material->map_transform`, token `0xC8`, via `partSet` (`[disp+0x80]`) |
| `0x02` | `0x00520F0C` | colour `+0x08`, opacity `+0x0C`, flags `+0x20` -> token list (`0x0E`,`0xBF`,`0x86`,`0x85`,...) |
| `0x04` | `0x005211CE` | same fields re-encoded through `fild`/float — lighting coefficients |
| `0x08` | `0x005212DB` | `[esi+0x40]` colour_map -> its driver object `[+0x40]`, token `0xA5` |
| `0x10` | `0x0052136E` | `[esi+0x44]`, token `0xA8` |
| `0x20` | `0x005213AF` | `[esi+0x54]` |
| `0x40` | `0x005213DD` | `[esi+0x58]` (`extra` token-value list) |

**Bit 0 is confirmed as the map_transform update** — its branch is the only one that touches
`material + 0x24`, which `kb.h` already records as `br_matrix23 map_transform`:

```
0x00520EEA: test bl, 1
0x00520EED: je   0x520f0c
0x00520EEF: mov  eax, [0x79efec]        ; renderer
0x00520EF4: lea  edx, [esi + 0x24]      ; &material->map_transform
0x00520EF7: push edx
0x00520EF8: push 0xc8                   ; token
0x00520EFD: mov  ecx, [eax]
0x00520EFF: push edi                    ; 0
0x00520F00: push 0x75                   ; part
0x00520F02: push eax
0x00520F03: call dword ptr [ecx + 0x80] ; RD_partSet
0x00520F09: add  esp, 0x14
```

So `BrMaterialUpdate(mat, 1)` is the minimal call that republishes only the map transform.

### 8.2 `BrTransformToMatrix34` @ `0x00531870` — `void __cdecl (br_matrix34 *dest, br_transform *src)`

Function extent `0x00531870 .. 0x00531B41`, followed by the 7-entry jump table at `0x00531B44`
(targets `0x53188E, 0x53188E, 0x5318A2, 0x5318CB, 0x531913, 0x5318F4, 0x531B2F`). `funcinfo`
also reports a ret at `0x00531B90`; that one belongs to the *next* function at `0x00531B60`,
not to this one.

**Six exits, all bare `C3`**, each preceded only by `pop edi / pop esi / add esp, 0x24`:

```
0x005318A1  0x005318CA  0x005318F3  0x00531912  0x00531B2E  0x00531B41
0x005318EE: 5F 5E 83 C4 24 C3     pop edi / pop esi / add esp,0x24 / ret
```

Argument order — `src` is loaded first but it is the **second** slot:

```
0x00531870: sub esp, 0x24
0x00531875: push esi
0x00531876: 8B 74 24 30           mov esi, [esp+0x30]   ; arg1 = src (br_transform*)
0x0053187B: 66 8B 06              mov ax, word ptr [esi]; src->type
0x00531887: FF 24 85 44 1B 53 00  jmp [eax*4 + 0x531b44]
0x0053188E: 8B 7C 24 30           mov edi, [esp+0x30]   ; arg0 = dest (after push edi)
0x00531892: 83 C6 04              add esi, 4            ; src matrix payload starts at +4
0x00531895: B9 0C 00 00 00        mov ecx, 0xc
0x0053189A: F3 A5                 rep movsd             ; 12 dwords = 48 bytes
```

This also pins the `br_transform` layout: `u16 type` at `+0x00`, payload from `+0x04`, and
for the rotation/quat/look-up variants the translation vector at `+0x28..+0x30`. It matches
`br_actor.t_type @ 0x28` / `br_actor.t @ 0x2C` in `kb.h` — i.e. `&actor->t_type` is a valid
`br_transform*`.

Call sites (18 xrefs) push `src` then `dest` and clean 8:

```
0x0040C7A1: 8D 75 28              lea esi, [ebp+0x28]   ; &actor->t_type
0x0040C7A4: 8D 54 24 4C           lea edx, [esp+0x4c]   ; dest
0x0040C7A8: 56                    push esi              ; arg1 = src
0x0040C7A9: 52                    push edx              ; arg0 = dest
0x0040C7AA: E8 C1 50 12 00        call 0x531870
0x0040C7AF: 83 C4 08              add esp, 8

0x0045CBE2: 52                    push edx              ; &actor->t_type
0x0045CBE3: 50                    push eax              ; dest
0x0045CBE4: E8 87 4C 0D 00        call 0x531870
0x0045CBE9: 83 C4 08              add esp, 8

0x00531B72: 50                    push eax              ; (neighbour fn at 0x531B60)
0x00531B73: E8 F8 FC FF FF        call 0x531870
0x00531B78: 8B 54 24 3C           mov edx, [esp+0x3c]
0x00531B7C: 83 C4 08              add esp, 8
```

Preserves `esi`/`edi`.

### 8.3 `BrMatrix34Mul` @ `0x00532620` — `void __cdecl (br_matrix34 *dest, const br_matrix34 *a, const br_matrix34 *b)`

Function extent `0x00532620 .. 0x005327B4`. No prologue, no callee-saved registers touched
(only `eax`/`ecx`/`edx` and the x87 stack). **One exit, bare:**

```
0x005327AD: D8 40 2C              fadd  dword ptr [eax+0x2c]
0x005327B0: D9 5A 2C              fstp  dword ptr [edx+0x2c]
0x005327B3: C3                    ret                   ; bare — no ret N
```

Arguments are read straight off the stack with no frame:

```
0x00532620: 8B 4C 24 08           mov ecx, [esp+8]      ; arg1 = a
0x00532624: 8B 44 24 0C           mov eax, [esp+0xc]    ; arg2 = b
0x00532628: 8B 54 24 04           mov edx, [esp+4]      ; arg0 = dest
```

Call sites (79 xrefs, 53 of them cleaning on the very next instruction) clean 12 bytes:

```
0x00407D20: 8D 8E 8C 00 00 00     lea ecx, [esi+0x8c]   ; b
0x00407D26: 51                    push ecx              ; arg2
0x00407D27: 53                    push ebx              ; arg1
0x00407D28: 57                    push edi              ; arg0 = dest
0x00407D29: E8 F2 A8 12 00        call 0x532620
0x00407D2E: 83 C4 0C              add esp, 0xc          ; 3 args

0x004093A4: 57                    push edi
0x004093A5: 51                    push ecx
0x004093A6: 52                    push edx
0x004093A7: E8 74 92 12 00        call 0x532620
0x004093AC: 83 C4 0C              add esp, 0xc
```

Semantics: row-vector `dest = a * b`, i.e. `dest.row[i] = a.row[i].x*b.row0 +
a.row[i].y*b.row1 + a.row[i].z*b.row2`, with `b.row3` added for `i == 3`
(`fadd [eax+0x24/0x28/0x2c]` immediately before the last three stores).

#### `dest` MUST NOT alias `a` or `b`

The function writes `dest` progressively — the twelve `fstp dword ptr [edx+...]` stores at
`0x532648, 0x532669, 0x53268B, 0x5326AD, 0x5326CC, 0x5326EB, 0x53270D, 0x53272C, 0x53274B,
0x53276C, 0x53278E, 0x5327B0` all happen while later rows are still reading the inputs:

```
0x00532648: fstp dword ptr [edx]       ; dest[0][0] written here
0x00532658: fmul dword ptr [ecx]       ; ...but a[0][0] is still needed
0x0053269C: fmul dword ptr [eax]       ; ...and so is b[0][0]
```

So both `dest == a` and `dest == b` corrupt the result. Any hook or wrapper that wants an
in-place multiply must multiply into a scratch `br_matrix34` and copy back.

### 8.4 Ready-to-use declarations

```c
typedef void (__cdecl *BrMaterialUpdate_t)(br_material *material, unsigned short flags);
typedef void (__cdecl *BrTransformToMatrix34_t)(br_matrix34 *dest, const br_transform *src);
typedef void (__cdecl *BrMatrix34Mul_t)(br_matrix34 *dest, const br_matrix34 *a, const br_matrix34 *b);

static const BrMaterialUpdate_t      BrMaterialUpdate      = (BrMaterialUpdate_t)0x00520E70;
static const BrTransformToMatrix34_t BrTransformToMatrix34 = (BrTransformToMatrix34_t)0x00531870;
static const BrMatrix34Mul_t         BrMatrix34Mul         = (BrMatrix34Mul_t)0x00532620;
```

---

## 10. Powerup instance pool (2026-07-31)

Goal: let a d3d9 proxy answer "is this `br_actor` / `br_model` one of the in-race powerup
pickups?". Answer: **there is no array of live pickups.** Pickups are ordinary actors that
already exist inside the track's actor hierarchy; the loader only re-skins them. The only
fixed global array is the *collected-and-waiting-to-respawn* list, which by definition holds
the pickups you can **not** see. The usable classifier is the shared powerup `br_model`
triple (10.2) or the actor name prefix (10.1).

### 10.1 Pickups are `&£NN` actors in the track hierarchy

`SpecialActorEnumCallback` @ **0x0040D1F0** (recursive via `BrActorEnum` 0x0051DED0, callback
re-registered at 0x0040D513) walks every actor of the loaded track and switches on the actor
name (`br_actor + 0x14`, i.e. `identifier`):

```
0x0040D232  cmp byte [ecx], 0x26        ; '&'
0x0040D23E  cmp al, '0' / '9'           ; name[1] digit  -> indexed "&NNNNNN" object
0x0040D4C5  cmp byte [ecx+1], 0xA3      ; name[1] == '£' -> POWERUP PICKUP
0x0040D4D5  movsx ecx, byte [eax+2]
0x0040D4D9  movsx edx, byte [eax+3]
0x0040D4DD  lea   ecx, [ecx+ecx*4]
0x0040D4E0  lea   edi, [edx+ecx*2-0x210]   ; edi = (name[2]-'0')*10 + (name[3]-'0')
```

`edi` is the **powerup index** into the POWERUP.TXT table (10.5). Visual treatment is then
dispatched purely on that index:

| index  | handler | effect |
|--------|---------|--------|
| 66..85 | `PowerupActorSetupSpin` @ **0x004DF570** | keeps the track's own model, adds Y-spin, `model->custom = 0x004DF650` |
| 86..87 | `PowerupActorSetupIcon` @ **0x004DF6C0** | replaces `actor->model` with the shared icon model, `model->custom = 0x004DFE10` |

Both are `__thiscall`, `ecx = br_actor*`. `0x004DF6C0` ends at 0x004DFDD0:

```
0x004DFDDD  call 0x00533DC0            ; BrMatrix34RotateY(&actor->t, 0.1f)
0x004DFDEA  mov  [ebp+0x18], eax       ; actor->model = g_powerup_model_arm
0x004DFDF6  mov  [ecx+0x24], 0x4DFE10  ; model->custom = PowerupModelCustomCB
```

Note `br_actor + 0x14 = identifier` (missing from kb.h before this section); confirmed by the
`'&'` compares above and by 0x004F11EC / 0x004F5003.

### 10.2 The three shared powerup icon models -- the practical classifier

`PowerupActorSetupIcon` @ 0x004DF6C0 clones three `.ACT` models once (lazily, guarded by the
globals being NULL) and caches them:

| global | built from | `BrModelAllocate` name | first write |
|--------|-----------|------------------------|-------------|
| **0x006A0AE0** | `&68powerup1.ACT` | `"PowArm"` (0x0065ECF8) | 0x004DF7A4 / 0x004DFAB2 |
| **0x006A0AE4** | `&70powerup1.ACT` | `"PowPow"` (0x0065ECE0) | 0x004DFC25 |
| **0x006A0AE8** | `&69powerup1.ACT` | `"PowOff"` (0x0065ECC8) | 0x004DF946 / 0x004DFD99 |

(fallback path reads them out of `POWRSHIT.TXT` / `&77powerup.ACT`, 0x004DF977.)

`PowerupModelCustomCB` @ **0x004DFE10** (`br_model_custom_cbfn`, arg0 = `br_actor*`) is the
only consumer: every 16 timer ticks it rotates the icon through the three models and spins the
actor.

```
0x004DFE46  mov edi, [0x6A0AE0]
0x004DFE4C  cmp edx, edi          ; actor->model == PowArm ?
0x004DFE56  mov [esi+0x18], edx   ;   -> PowPow
0x004DFE5B  cmp edx, [0x6A0AE4]   ; == PowPow ?
0x004DFE69  mov [esi+0x18], edx   ;   -> PowOff
0x004DFE6E  mov [esi+0x18], edi   ; else -> PowArm
0x004DFE88  call 0x00533960       ; BrMatrix34PostRotateY(&actor->t, angle)
```

**Proxy test (recommended):**

```c
br_model *m = actor->model;
bool is_powerup_icon = m && (m == *(br_model**)0x006A0AE0 ||
                             m == *(br_model**)0x006A0AE4 ||
                             m == *(br_model**)0x006A0AE8);
```

or, model-only and pointer-free: `m->custom == (void*)0x004DFE10`, or
`m->identifier` in {`"PowArm"`, `"PowPow"`, `"PowOff"`}.

This covers index 86/87 pickups (the ubiquitous floating icons). Index 66..85 pickups keep
their track-authored model; for those the only test is
`actor->identifier[1] == '\xA3'` -- which is exactly the test the game itself uses at runtime
(0x004F11EC).

### 10.3 Collection: pending-hit queue, then `render_style = BR_RSTYLE_NONE`

Car-vs-actor collision response (0x004F11C6 onward) filters candidates the same way:

```
0x004F11E1  mov eax, [edx+0x14]         ; actor->identifier
0x004F11EC  cmp byte [eax+1], 0xA3      ; '£' -> it is a pickup
0x004F11F6  cmp byte [edx+0x20], 1      ; render_style == BR_RSTYLE_NONE ?
0x004F11FA  je  0x004F1306              ;   already collected -> ignore
0x004F1217  call 0x004F1030             ; QueueSpecialActorHit(ecx=car, edx=index, actor)
```

`QueueSpecialActorHit` @ **0x004F1030** appends to a fixed queue:

```
$ 0x006A4430  g_pickup_hit_queue   ; stride 0x0C, 50 entries (limit checked at 0x004F1067/0x004F10DB)
              [0x00] owner  (car spec* / 0x0075BC2C)
              [0x04] powerup index (the £NN code; 0x0E for the ped-special case)
              [0x08] br_actor*
$ 0x006A55BC  g_pickup_hit_count   ; reset to 0 at 0x004F578A and 0x004ED299
```

Queue drain, once per physics step, `0x004ED268 .. 0x004ED299`:

```
0x004ED273  mov esi, 0x6A4434
0x004ED27D  call 0x004D8D30      ; ApplyPowerupToCar(ecx=owner, edx=index)
0x004ED287  call 0x004E0750      ; RegisterCollectedPickup(ecx=index, edx=actor)
0x004ED292  add esi, 0xC
0x004ED299  mov [0x006A55BC], 0
```

The **hide** itself is done by the shared "special actor reaction" handler
`SpecialActorReact` @ **0x004F4E20** (called at 0x004F1268 / 0x004F12C9 with the hit actor):

```
0x004F4FDB  cmp  byte [ebx+0x20], 1     ; already hidden?
0x004F4FDF  je   0x004F5037
0x004F4FEF  mov  byte [ebx+0x20], 1     ; <<< actor->render_style = BR_RSTYLE_NONE
0x004F4FF3  call 0x004C8960             ; net broadcast msg 0x21 (hide actor)
```

There is a second hide at **0x004F4FA2** for the actor's children (`actor->children` chain,
0x004F4F97..0x004F4FAA). The actor is **never** unlinked -- `BrActorRemove` is not involved.
Both recursive renderers bail on `BR_RSTYLE_NONE` (see kb.h), so the pickup simply stops
drawing.

### 10.4 Respawn: the one fixed global array

`RegisterCollectedPickup` @ **0x004E0750** (`ecx` = powerup index, `edx` = `br_actor*`):

```
0x004E0755  mov eax, [0x006A0A50]       ; per-skill "powerup enabled" byte table
0x004E075A  cmp byte [ecx+eax], 0       ; index not enabled -> no respawn at all
0x004E0777  mov eax, 0x006A0458         ; linear scan for a free slot
0x004E077C  cmp dword [eax], 0
0x004E0781  add eax, 0x0C
0x004E0785  cmp eax, 0x006A0908
0x004E0794  mov [esi+0x006A0458], edx   ; slot[0x00] = br_actor*
0x004E079A  mov [esi+0x006A045C], ecx   ; slot[0x04] = powerup index
0x004E07BB  mov [esi+0x006A0460], eax   ; slot[0x08] = respawn deadline (ms)
```

**`g_pickup_respawn_slots` @ 0x006A0458 -- stride 0x0C, (0x6A0908-0x6A0458)/0xC = 100 entries.**
Slot free iff `[0x00] == NULL`. Deadline = `now + g_pickup_respawn_base + g_pickup_respawn_range/2`
(0x004E07A0..0x004E07BB), with `g_pickup_respawn_base` @ **0x007447D8** and
`g_pickup_respawn_range` @ **0x007447E8**, both parsed from a settings TXT at 0x00487BF1 /
0x00487C07 (`value * 1000`).

Tick, `RespawnDuePickups` @ **0x004DB880** (called from 0x00493A24 and 0x004E69A3):

```
0x004DB883  call 0x00514C30            ; now = timer ms
0x004DB891  mov  esi, 0x006A0458
0x004DB898  mov  eax, [esi]            ; actor
0x004DB89E  cmp  edi, [esi+8]          ; now < deadline -> skip
0x004DB8A8  mov  byte [eax+0x20], 4    ; <<< actor->render_style = BR_RSTYLE_FACES
0x004DB8B0  call 0x004C8F90            ; net broadcast msg 0x41 (show actor)
0x004DB8B7  call 0x004ECEA0            ; sparkle FX at actor->t.translate (actor+0x50)
0x004DB8BC  mov  dword [esi], 0        ; free the slot
0x004DB8C2  add  esi, 0x0C
0x004DB8C5  cmp  esi, 0x006A0908
```

So: **the same `br_actor` is reused**; only `render_style` toggles 1 <-> 4. Race start clears
the whole array at 0x004DA67D..0x004DA68C.

Network mirrors of the same two writes (message-table thunks at 0x0065D6EC / 0x0065D6F4):

```
0x004C9D40  ...  jmp 0x004E07D0   ; ShowActor:  mov byte [ecx+0x20], 4 ; ret
0x004CA3B0  ...  jmp 0x004E07E0   ; HideActor:  mov byte [ecx+0x20], 1 ; ret
```

### 10.5 The powerup *definition* table (not instances)

For completeness, the array that `kMem_powerup_array` (tag 0xC5) names is the POWERUP.TXT
type table, not an instance pool:

```
0x004D987C  mov [0x006A0AD0], eax          ; g_powerup_count  (entries in POWERUP.TXT)
0x004D9895  call 0x005275C0                ; BrMemAllocate(count * 0xAC, 0xC5)
0x004D98A5  mov [0x006A0A54], eax          ; g_powerup_defs   (stride 0xAC)
```

Indexing is `g_powerup_defs + index * 0xAC` (0x004DED0A..0x004DED1E). Entry fields seen:
`+0x14` name, `+0x38` flags, `+0x3C` / `+0x40` apply / remove `__thiscall` handlers,
`+0x50`/`+0x54` timers. Per-car active state lives at `car + 0x1710 + index*4`
(0x004DED61, 0x004DEDC4). `LoadPowerups` @ **0x004D96C0** (POWERUP.TXT / ZOMPOWERUP.TXT /
ALPOWERUP.TXT chosen at 0x004E0C02..0x004E0C32).

### 10.6 Naming prefix fallback

`DATA/ACTORS/&Gpowerup.act`, `&Lpowerup2.act`, `&Lpowerup3.act` carry internal identifiers
`&73powerup.act`, `&68powerup2.act`, `&68powerup3.act`; the EXE additionally names
`&68powerup1.ACT`, `&69powerup1.ACT`, `&70powerup1.ACT`, `&77powerup.ACT`. So the source
models all match `*powerup*.act` (case-insensitively). **But** the models actually bound to a
pickup actor at render time are the *clones*, whose identifiers are `"PowArm"`, `"PowPow"`,
`"PowOff"` -- match the `"Pow"` prefix, or compare against the three globals in 10.2.

## 12. Sprites: prelit vertex colour and animated opacity (2026-08-02)

Symptom: smoke, dust and exhaust render as flat white puffs at full strength, and other
translucent effects never thin out. The guess was palettized textures; it is not — a full
race's log carries **no** `unhandled pixelmap type`, `untextured` or `skipped` line, so every
colour_map that reached the injection uploaded fine. The colour and the fade never lived in
the texture at all.

### 12.1 The smoke system

`InitSmokeStuff` @ **0x004F9FC0** builds the whole thing once:

| what | global | built by |
|---|---|---|
| `gBlend_model` — 4 vertices, 2 faces (a quad) | `0x0074CF30` | `BrModelAllocate` @0x004F9FD7 |
| `gBlend_model2` — 6 vertices, 4 faces | `0x0074CF94` | `BrModelAllocate` @0x004F9FED |
| `gBlend_actor` | `0x0074CAAC` | `BrActorAllocate` @0x004F9FFF |
| 35 materials all named `"some smoke"` | `0x006A880C`, stride 0x24 | `BrMaterialFind` loop @0x004FA013 |

Both models get `flags |= BR_MODF_KEEP_ORIGINAL` (0x004FA2F0), so their authored
`br_vertex` array stays alive. Every one of the 35 materials is then given the *same*
three fields (0x004FA355..0x004FA376):

```
[mat+0x20] = 0x27          ; LIGHT|PRELIT|SMOOTH|PERSPECTIVE
[mat+0x58] = 0x00660148    ; extra = { BLEND_B 1 }, { OPACITY_X 0x004B0000 }, { 0, 0 }
[mat+0x40] = SMOKE.PIX     ; 64x64 BR_PMT_RGBA_4444, a white blob with a 4-bit alpha ramp
```

`DrawSmokeParticles` @ **0x004FB1B0** depth-sorts the live particles and draws them one at
a time through the one shared actor. The particle record is 0x24 bytes at `0x006A87F0`
(`+0x00` world position, `+0x0C` size, `+0x10` alpha, `+0x14` RGB from the 16-entry type
table at `0x006B7840`, `+0x18` aspect, `+0x1C` the slot's material, `+0x20` the model):

```
0x004FB1FA  actor->t.translate  = record[0x00..0x08]
0x004FB21C  actor->t.m[0][0]    = record[0x0C]
0x004FB236  actor->material     = record[0x1C]
0x004FB23C  fld [rec+0x10] / fmul 150.0 / fmul 65536.0 / ftol
0x004FB258  mov [material->extra + 0x0C], eax     ; OPACITY_X value
0x004FB25F  BrMaterialUpdate(material, 0x40)      ; BR_MATU_EXTRA
0x004FB289  vertex[i].red/green/blue = record[0x14]   for every vertex
0x004FB2AF  BrModelUpdate(model, 2)               ; BR_MODU_VERTEX_COLOURS
0x004FB2C7  BrZbSceneRenderAdd(gBlend_actor)
```

**So a smoke puff's entire colour is the model's vertex RGB and its entire fade is one
token in the material's extra list.** The material never changes, the texture is white, and
both channels the game does use were being dropped. Everything the proxy submitted was
therefore a fully opaque white quad — exactly the reported symptom.

### 12.2 Opacity is a 0..255 byte, whichever token spells it

`MaterialNeedsAlpha` @ 0x0051F630 tests `extra` for tokens **0xBE** and **0xBF**. The
BRender token-name table (records of `{char* name, ?, token, type}` at 0x00668000..0x0066A400)
resolves those to `OPACITY_X` (fixed) and `OPACITY_F` (float); `0x85` is `BLEND_B`. It also
corrects `kb.h`: `material+0x4C` is **index_blend**, not index_shade (0x44 screendoor,
0x48 index_shade).

`BrMaterialUpdate`'s BR_MATU_MATERIAL branch pins the scale (0x00520F0C onward):

```
0x00520F18  token 0x0E COLOUR_RGB   = material->colour
0x00520F29  token 0xBF OPACITY_F    = (float)material->opacity * 0.003922   ; 1/255
0x00520F48  token 0x85 BLEND_B      = (material->opacity != 0xFF)
```

so `br_material::opacity` is a 0..255 byte and blending follows from it. The two runtime
writers of `OPACITY_X` agree the fixed form carries that same byte in its integer part:
smoke writes `alpha * 150` (peaking at 150/255 ≈ 59%) and `'Acc Poly Mat'` writes
`0x00800000` = 128 at 0x0045AAB4. The `BLEND25/50/75.TAB` shade tables in `DATA/SHADETAB`
name the same quantity.

Note `'Acc Poly Mat'`'s list at 0x005962F8 is **not** zero-terminated — the next object's
bytes follow it — so any walk of `extra` has to bound itself.

### 12.3 Prelit is the flag that says "the vertex colour is the colour"

`BrModelUpdate` writes the prepared group's colour array at 0x0051FB66:

```
vertex_colours[v] = (vertex.index << 24) | (red << 16) | (green << 8) | blue
```

guarded by the update flags read at 0x0051FAF7: `1` positions, `2` vertex colours, `4` UVs,
`8` normals, `0x20` face colours — i.e. BRender's `BR_MODU_*`. The top byte is the palette
index, not alpha.

Whether that colour is the surface colour is `BR_MATF_PRELIT` (0x2). Without it BRender
lights the model at render time and the prepared colours are just the authored ones, so
carrying them into Remix would double up with Remix's own lighting. With it — smoke
(flags 0x27) and `gLine_material` (flags 0x1007) — they are final. Track materials sampled
from `DATA/MATERIAL/*.MAT` carry flags 0 or `BR_MATF_LIGHT`, so gating on PRELIT leaves the
world untouched.

### 12.4 What the proxy does now

* `ffp_vertex` carries a `D3DCOLOR` diffuse, filled from `v1_group::vertex_colours` for
  prelit materials and left white otherwise. Stage 0 becomes
  `COLOROP = MODULATE(TEXTURE, DIFFUSE)`, which is identity for everything else.
* `material_opacity()` resolves `br_material::opacity` and any OPACITY token in `extra`
  into a 0..255 byte, re-read every capture (the smoke rewrites it between particles).
  It rides in `D3DRS_TEXTUREFACTOR`'s alpha with
  `ALPHAOP = MODULATE(TEXTURE, TFACTOR)`, and anything below 255 moves into the
  translucent pass — which is what `BLEND_B` does in BRender.
* Opacity joins `chunk_key`, since a sealed chunk draws under one texture factor.
* `on_material_update` now treats BR_MATU_MATERIAL and BR_MATU_EXTRA as animation too, so a
  material whose opacity moves between scenes is demoted out of the static world.
* `upload_pixelmap` handles `BR_PMT_INDEX_8` through `br_pixelmap::map` (offset **0x10**,
  confirmed at 0x004FA340 where SMOKE.PIX is handed the DRRENDER.PAL pixelmap). No shipped
  texture has needed it so far; it closes the gap that would otherwise show as white.

### 12.5 One model, many instances: transient geometry

The smoke draws 35 particles from one 4-vertex quad, calling `BrModelUpdate` between each.
`m_geometry` is keyed by `br_model`, so all 35 queued draws pointed at one entry and would
have shown the last particle's colours. That was invisible while the buffers held only
positions — identical for every particle — and became a real defect the moment the vertex
colour went in.

`geometry_for` now detects a rebuild of a model the current scene has already queued and
builds into a pooled `model_geometry` that lives for that scene (`m_transient`, a deque so
the queue's pointers stay valid, recycled rather than reallocated each scene).

New switches, both default on: `[Effects] VertexColour` and `[Effects] MaterialOpacity`.
The log names every material either one touches (`shaded material: '<name>' - ...`).

---

## 11. ESC-menu crash: `TintPolyHide(-1)` (2026-07-31)

Crash dump: `%LOCALAPPDATA%\CrashDumps\CARMA2_HW.EXE.27856.dmp`
`0xC0000005` writing `0x3E95C08F`, faulting PC **0x004D826E**.
**Root cause is a stock-game bug, not the proxy.** It is latent on small maps and
deterministic on the city.

### 11.1 The faulting function: `TintPolyHide` @ 0x004D8250

```
0x004D8250  lea  eax, [ecx+ecx*4]        ; __fastcall, ecx = slot index
0x004D8253  shl  eax, 6
0x004D8256  add  eax, ecx
0x004D8258  xor  ecx, ecx                ;   eax = index * 0x6450   (25680)
0x004D825A  lea  eax, [eax+eax*4]
0x004D825D  shl  eax, 4
0x004D8260  cmp  dword [eax+0x705CB0], ecx   ; slot->in_use == 0 ?  <-- ONLY guard
0x004D8266  je   0x004D828A                  ; ...returns if 0
0x004D8268  mov  edx, dword [eax+0x705C80]   ; slot->actor
0x004D826E  mov  byte [edx+0x20], 1          ; actor->render_style = BR_RSTYLE_NONE  <-- FAULT
0x004D8272  mov  dword [eax+0x705C8C], ecx
0x004D8278  mov  dword [eax+0x705C90], ecx
0x004D827E  mov  dword [eax+0x705C94], ecx
0x004D8284  mov  dword [eax+0x705CB4], ecx   ; slot->visible = 0
0x004D828A  ret
```

There is **no index bounds check at all**. The only guard is the `in_use` flag read at
`base + index*0x6450 + 0x30`, which for a negative index reads memory *below* the table.

### 11.2 The table: the "tinted poly" pool

* **Base `0x00705C80`**, **stride `0x6450` (25680)**, **capacity 10**, limit `0x007447D0`.
* Zero-initialised wholesale by `TintPolyInit` @ **0x004D7040**:
  `mov ecx,0xFAC8 / mov edi,0x705C80 / rep stosd` -> 0xFAC8*4 = 0x3EB20 = 10 * 0x6450.
* Slot fields confirmed: `+0x00` `br_actor*` (render_style at actor+0x20), `+0x0C`,
  `+0x10`, `+0x14` (cleared on hide), `+0x30` `in_use`, `+0x34` `visible`,
  `+0x38` subclass/type, `+0x40` `br_material*`.
* Identity from the strings the pool's constructor uses: `"Tint Poly Mat"` (0x0065E8D8),
  `"tinted_poly_camera"` (0x0065E87C), `"Invalid Pulse Poly subclass"` (0x0065E894).
  This is the full-screen tint / pulse-overlay system (screen flashes, damage/powerup tints).
* **Allocator** `TintPolyCreate` @ **0x004D70C0**: linear scan
  `for (eax = 0x705CB0; eax < 0x7447D0; eax += 0x6450)` for the first slot with
  `in_use == 0`; **returns -1 when the pool is full** (`or eax, ebp` at 0x004D70FF).
  Bounded correctly -- it can never write past slot 9.
* All 22 code references to `0x705C80` live in `0x004D7000..0x004D8700`. Nothing in the
  render path (`BrZbActorRender` 0x005221E0, `BrZbModelRender` 0x00521890, the style
  thunks at `g_pfnModelRenderStyleTable` 0x00665090) touches this table. **The table is
  not corrupted and its count cannot be corrupted by frustum-culling or render changes.**

### 11.3 Where the -1 comes from

Three globals hold tint-poly slot handles; **all three are statically initialised to -1**:

| global | written by | dump value |
|---|---|---|
| `0x00655E48` | `0x0047E00A` (result of `TintPolyCreate` @0x0047DFF1) | `0` |
| `0x00655E4C` | `0x0047E01B` (result of `TintPolyCreate` @0x0047E00F) | `1` |
| `0x00655E50` | **never written -- 1 reference in the whole image, a read at 0x0046D91C** | `0xFFFFFFFF` |

Byte-pattern search for the displacement `50 5E 65 00` returns exactly one hit
(`0x0046D91E`, the disp32 of `mov ecx,[0x655E50]`). `0x00655E50` is a dead third handle
that stays -1 for the entire process lifetime.

`FrontendEnterFromRace` @ **0x0046D8E0** hides all three:

```
0x0046D900  mov ecx,[0x655E48] ; 0x0046D90C call 0x004D8250
0x0046D911  mov ecx,[0x655E4C] ; 0x0046D917 call 0x004D8250
0x0046D91C  mov ecx,[0x655E50] ; 0x0046D922 call 0x004D8250   <-- ecx = -1
0x0046D927  mov ecx, esi       ; 0x0046D929 call 0x0046D1C0   (Frontend_Setup)
```

Register check: `eax = 0xFFFF9BB0 = -0x6450 = -1 * 25680`. Exact match, index = -1.

Note the *other* pause path, `RaceLoopPauseHideTintPolys` @ **0x00504230**, hides only
`0x655E48` and `0x655E4C` and is safe. Only the frontend-entry path hits the dead handle.

### 11.4 Why it only crashes on the city map

`TintPolyHide(-1)` reads `slot[-1].in_use` at `0x00705C80 - 0x6450 + 0x30 = 0x006FF860`
and `slot[-1].actor` at `0x006FF830`. Both lie in the zero-initialised tail of `.data`
(`.data` = 0x0058F000..0x007A18A4, raw size 0xE7A00 so file-initialised only to
0x00676A00). Dump values:

```
0x006FF830: 6F C0 95 3E ...   -> edx = 0x3E95C06F  (a float, ~0.2925)
0x006FF860: 70 A8 B0 42       -> in_use != 0  -> guard passes -> AV
```

That memory belongs to a large track-geometry / bounds-tree pool of 0x40-byte records
(bounding floats plus three child pointers, `0x4FFF0000` used as an infinite-bound
sentinel) that occupies `~0x006B8000..0x00704000`, ending only ~0x1C80 short of the tint
table. In this dump it is populated well past 0x006FF860. On smaller tracks the pool never
grows that far, `slot[-1].in_use` stays 0, and `TintPolyHide(-1)` returns harmlessly.
**The city is simply the first map big enough to fill that far.**

### 11.5 Call stack

| return addr | function | role |
|---|---|---|
| 0x004D826E | `TintPolyHide` 0x004D8250 | faulting write |
| 0x0046D927 | `FrontendEnterFromRace` **0x0046D8E0** | hides the 3 tint polys, then `Frontend_Setup` 0x0046D1C0 (`"START OF FRONTEND_Setup"` @0x006559B4) |
| 0x004945AB | **0x00494570** | pause/ESC handler; `mov ecx,1 / call 0x0046D8E0`, on 0 result sets `[0x0075BC24] = 6` |
| 0x00493BF2 | **0x004939EA** | per-frame race tick; brackets the menu with `call 0x00504230` (hide) / `call 0x005042A0` (restore) around `call 0x00494570` |
| 0x00503FD6 | **0x00503C50** | race main loop |
| 0x00492534 | 0x00503C50's caller (main/`WinMain` path, ret to 0x00492534 from `call 0x00503C50` @0x0049252F) | |
| 0x0051AF0E | CRT/WinMain | |

### 11.6 Answer to "does the proxy cause this?"

No. Neither the boundsTest override, the skipped `BrZbModelRender` body, nor the yon_z
rewrite writes anywhere near `0x006FF830` or into the tint-poly table. They change *how
much* of the track pool is touched per frame but not *where* it lives. The proxy makes the
crash easier to hit only insofar as it keeps the user on the biggest map; the same call
happens on stock.

### 11.7 Minimal fix

Preferred: add the missing bounds check in place. `TintPolyHide` is 0x004D8250..0x004D828B
with 5 padding NOPs after; replacing the 16-byte index-scaling prologue with
`imul` frees exactly the 5 bytes needed:

```
; 0x004D8250, 16 bytes, ends exactly at 0x004D8260 (unchanged tail)
83 F9 0A              cmp  ecx, 10               ; capacity
73 35                 jae  short 0x004D828A      ; -> the existing ret
69 C0 50 64 00 00     imul eax, ecx, 0x6450
33 C9                 xor  ecx, ecx
90 90 90              nop
```

Apply the identical guard to `TintPolyShow` @ **0x004D8220** (same shape, 14 bytes
0x004D8220..0x004D822E, `jae short 0x004D824C` = `73 27` from 0x004D8225, then
`imul eax, ecx, 0x6450` + 3 NOPs). `TintPolyIsVisible` @ 0x004D8CF0 also lacks a check but
only reads, so it is harmless.

Alternative one-liner if the proxy prefers not to touch shared code: NOP the dead third
call in `FrontendEnterFromRace` -- 11 bytes at **0x0046D91C** (`8B 0D 50 5E 65 00` +
`E8 <rel32>`) -> `90 x11`. Removes the bogus `TintPolyHide(-1)` outright. It is safe
because `0x00655E50` is provably never assigned.

Do **not** "fix" this by writing a value into `0x00655E50` -- any non-negative value there
would make the frontend hide a live tint poly slot.

---

## 13. The static world decays over a race (2026-08-02)

Section 9's chunks build correctly and then bleed. A full city race with the 2026-08-02
build seals 848 actors into 341 chunks and immediately runs at 58-62 fps, then loses 303
of them over the race:

```
scene  600: 848 baked, 341 chunks,  94 demoted, 2106 draws (+ 848 glide), game 3.42 submit 2.91 -> 58.1 fps
scene 1800: 676 baked, 341 chunks, 266 demoted, 3044 draws (+ 905 glide), game 5.08 submit 6.12 -> 39.2 fps
scene 4800: 669 baked, 343 chunks, 303 demoted, 3972 draws (+2326 glide), game 10.40 submit 8.44 -> 32.9 fps
```

`other` (game logic) holds at 9.5-13.3 ms throughout, so the whole 62 -> 33 fps swing is
`game` + `submit`. Every eviction costs twice: the actor goes back to a per-model draw
*and* regains its BRender T&L and nGlide render, because `SuppressGameRender` is scoped to
chunk-covered geometry. Scene 3600 still reaches 62.1 fps, which is the proof that the
chunks themselves are fine -- what varies is how much geometry is left outside them.

### 13.1 Why it only ever shrinks

`demote_actor` inserted into `m_moving_actors`, and `capture_model` tested that set before
tracking an actor at all. Nothing removed from it. In a game whose entire premise is
demolishing the scenery, a one-way blacklist means the static world can only decay until
the race ends. That is why the rework measured well when tested and not after a full race.

### 13.2 Reparenting was being read as movement

`placement_fingerprint` mixed the parent chain's **node addresses** into the same hash as
its transform bytes, so a relink read as a move. The city regroups scenery as it streams,
which relinks whole instanced sets at once -- the log shows `&02lamp.act`, `&03traffic.act`,
`&09citbarrier.act`, `&25citree2.act` and `&01citree1` all evicted as `chain relinked`, and
those five model names cover the ~172 actors lost between scenes 600 and 1800.

Placement and parentage answer different questions. The hash is now split: `where` is the
transform bytes up the chain, `parent` is the node addresses. A change in `where` is a
move; a change in `parent` alone is absorbed and counted. Nothing is lost by dropping the
addresses from the placement -- a reparent that actually relocates the actor changes some
ancestor's transform bytes, so it still reads as a move.

### 13.3 Eviction is not instrumented

`on_material_update` demotes every baked actor sharing a newly animated material in one
sweep and logged nothing at all, so a mass eviction was indistinguishable from gradual
drift in the single `demoted` counter. `8932aac` widened its trigger from
`BR_MATU_MAP_TRANSFORM` to `| BR_MATU_MATERIAL | BR_MATU_EXTRA` **unconditionally**, while
gating the rest of that commit behind `[Effects] MaterialOpacity` -- so the one change most
able to shrink the static world was the one that could not be switched off for a
measurement.

### 13.4 What changed

* **`unbake_actor` replaces `demote_actor`.** The record survives an eviction with its
  bake taken back out and its sighting count restarted, so scenery that comes to rest
  rejoins the static world. A knocked lamppost lying still is scenery again. The
  `m_moving_actors` blacklist is gone.
* **Split fingerprint** (13.2), with `relinks absorbed` counted so the rate is visible.
* **Per-reason demotion tally** in the scene report: `moved`, `swapped`, `deformed`,
  `animated`, plus `relinks absorbed` and `rebaked`. Every eviction lands in exactly one
  bucket.
* **`on_material_update` names the material** and how many baked actors it took, and the
  `BR_MATU_MATERIAL | BR_MATU_EXTRA` widening now follows the `MaterialOpacity` switch:
  without opacity being rendered, a fading material looks no different baked.
* **Bakeability is decided once, from the model** (`classify_actor`), and unbakeable actors
  skip the chain walk entirely rather than being fingerprinted every frame.

### 13.5 Why the baker cannot whitelist "truly static" geometry instead

The immovable set in this game is small. `4939c72` excluded every `&`-named model and only
243 of ~1400 actors baked, costing 25 fps (`0ea4eaf`): in a Carmageddon city the buildings
and road surface are one track mesh, and almost everything that gives the level its density
-- trees, lamps, rails, bins, barriers -- is a noncar, individually knockable, with its own
`DATA/NONCARS/*.txt` spec carrying `mass attached` / `mass unattached` and a torque
threshold. There is no large class of geometry that is static by construction.

So the test cannot be *what is this*, it has to be *is the game writing its placement* --
which is what the fingerprint already measures, and now measures without the false
positives. Cars are excluded by that test rather than by name: their transforms are
rewritten every frame, including on the grid, so they never survive the three-scene
probation.

The one exclusion that cannot be derived from movement is deletion: a pickup vanishes
without ever moving, and a chunk cannot give geometry back. That stays a positive test
(`vanishes_outright`) over the pickup naming rule and the two decal pools.

---

## 14. Two things the tally caught that guesswork did not (2026-08-02)

The per-reason counters from section 13 were run against a full city race. They refuted
one of that section's own conclusions and found the real costs.

### 14.1 Reparenting was never the problem

`relinks absorbed` is **0** for the entire race. Every eviction logged as `chain relinked`
had a changed transform as well; the label only reflected which test the diagnostic
reported first. Section 13.2's split fingerprint is still correct -- placement and
parentage are different questions -- but it fixed nothing, and the ~172 actors lost between
scenes 600 and 1800 were lost to something else.

### 14.2 Probation counted draws, not scenes

`demoted 32662 (moved 32389 ... 32204 rebaked)` over 6000 scenes, from **229 distinct
model names**. That is not decay, it is a bake/unbake oscillation running about five times
a scene.

`sightings` incremented once per `capture_model` call, and the game draws many instances
through a single actor: the smoke quad, the spark emitter and both decal pools re-place one
actor between draws. Three draws in one frame satisfied a three-scene probation, so a
particle baked into the world, and the next draw's transform unbaked it. Under the old
permanent blacklist the first unbake ended it; making demotion recoverable turned it into a
loop.

Two consequences, both visible in the log. Chunk vertices grew from 59k at the first seal
to **283k** -- `punch_out` zeroes indices but never reclaims vertices, so every rebake left
its predecessor behind as dead geometry. And chunk count climbed 298 -> 405 across **9 seal
passes**, each one handing Remix a new set of buffers to hash.

An actor drawn more than once in one scene is now classified as an instancing stencil and
barred from baking -- a positive test on behaviour, no names involved. Probation counts
scenes via `last_seen_scene`. A demoted actor must hold still for `STATIC_REBAKE_SIGHTINGS`
(120 scenes, ~2 s) rather than 3, and `STATIC_MAX_BAKES` caps any actor at three bakes for
the race, which bounds the dead vertices a recovery can leave behind.

### 14.3 The widened animation rule was evicting the road

`ROAD` (18 actors), `SMLRD` (16), `0RDSDTOP` (5), `0claybot` (9), `0SHRxxx?xxxx.MAT` (13)
and 468 other materials were marked animated. Those are road and terrain -- most of a
level's surface area.

`8932aac` treated any `BR_MATU_MATERIAL` or `BR_MATU_EXTRA` update as animation. The game
re-publishes materials for reasons the injection does not render; `BR_MATU_MATERIAL` rides
along with lighting and index-range changes. Gating the rule behind `MaterialOpacity` did
not help, because that switch has to stay on for smoke.

The flag is no longer the signal. `on_material_update` now keeps the last opacity it
resolved per material and only counts an update as animation when the value actually moved.
`BR_MATU_MAP_TRANSFORM` is unchanged -- the funk system's UV animation needs no such test.

---

## 15. The dips are the dynamic population, and nothing measured it (2026-08-02)

The scene-probation build holds the static world steady -- demotions fell from 32662 to
487, rebakes to 52, chunk vertices to 60-97k -- and the sampled scenes run at 60-63 fps.
The dips remained, and the log could not see them.

### 15.1 Why no dip ever appeared in the log

Two separate blind spots. The periodic line samples one scene in 600, so a dip lasting a
couple of seconds falls entirely between two samples. The `<-- new worst` line was supposed
to catch exactly that, but it tracked an **all-time** worst, and scene 2 of every run is a
load stall of 100-400 ms that holds the record for the rest of the session -- so it fired
once, at startup, and never again.

The worst frame is now tracked per reporting window and logged next to the sample, so every
600 scenes the log carries both a typical frame and the window's worst with the same
breakdown.

### 15.2 What the sampled numbers do say

Comparing the best and worst sampled scenes of one race:

```
scene 3000: 62.3 fps |  941 models (602 baked) | 1008 draws (+ 95 glide) | game 0.98 submit 0.45 other 14.19
scene 3600: 47.4 fps | 1439 models (598 baked) | 2403 draws (+986 glide) | game 4.71 submit 4.31 other 10.52
```

The baked count is identical. What changes is the queue: 339 dynamic models becomes 841,
and that alone is worth 9 ms of `game` plus `submit`. `other` -- game logic, physics, AI --
sits at 10-14 ms in every sample and is a hard floor of roughly 70 fps that no amount of
render work removes.

So the remaining question is entirely *what those 841 models are*, and `models` minus
`baked` could not answer it. Each queued model now records why the chunks could not carry
it -- overlay, callback, vanishing, instanced, unbakeable, moving, probation, unsealed --
and the counts ride in the report.

### 15.3 SuppressDynamics

`game` is BRender's software T&L plus the nGlide raster of models Remix discards, and the
glide draws are those same models crossing the bridge. At the dip that is 4.7 ms and 986
draws for geometry the injection has already sent in model space.

Suppression has been scoped to chunk-covered geometry since section 9, because coverage is
only provable there: what passes through BrZbModelRender is injected, and whatever a model
draws outside that hook -- pedestrian limbs are the known case -- exists solely in the
game's rasterized stream. `[Optimization] SuppressDynamics` makes that trade available and
measurable rather than assumed. It is off by default; turning it on should take `game` to
roughly 1 ms and the glide draws to near zero, and the peds are what to watch.

---

## 16. Track changes: recycled pointers, not stale ones (2026-08-02)

Three symptoms, one cause. On the second and later races of a session: performance
degrades, textures come out misaligned, and geometry from the previous track appears in the
new one. The first race of a session is always fine.

### 16.1 Every cache is keyed on a pointer the game frees

`reset_static_world` cleared the chunks, the actor records and the material state. It did
not clear anything else, and nothing else was ever cleared for the life of the process:

| cache | key | what a track change does to it |
|---|---|---|
| `m_textures` | `br_pixelmap*` | the new track's pixelmaps land on freed addresses -> **the previous track's texture is returned** |
| `m_geometry` | `br_model*` | same, per model -> **the previous track's mesh is drawn** (only evicted after 900 unused scenes) |
| `m_materials` | `br_material::stored` and the material pointer | resolves to a dead material -> wrong atlas cell, wrong opacity |

These are not stale entries in the harmless sense. They are confident cache *hits* that
return the wrong object, because the allocator hands the new track the addresses the old
one just released. That is the misalignment, the carried-over geometry, and -- once the
baker starts chunking geometry built from mismatched pairs -- the performance loss too.

The colour and spark swatches are keyed on colour rather than on a game pointer, so they
are the one thing that survives a track change intact.

### 16.2 The old race-change signal could not work

Two triggers existed and neither is a track change. The world actor pointer never changes,
because the game reuses one across races. The population heuristic -- a whole scene of
never-seen actors while none of the live ones are walked -- is inference over the scene
walk, tuned against the city's zone streaming, and it fires late, or mid-race, or not at
all. Both are gone.

`Frontend_Setup` @ **0x0046D1C0** is the game's own answer. Everything that leaves a race
enters it: finishing, and the pause menu via `FrontendEnterFromRace` (0x0046D8E0). It is
`__thiscall` with no stack arguments, so a `__fastcall` detour taking `this` in ecx and an
unused edx has an identical calling sequence.

### 16.3 Why the flush runs at frontend entry, not at the next race

It has to happen before the new track loads. `learn_materials` is driven from
`BrModelUpdate`, which the level loader calls for every model, so the `br_material::stored`
tokens for a track are learned during its load. Flushing at the first race scene of the new
track would discard exactly that, and every model would fall back to its fallback material.

`reset_for_new_track` therefore runs from the `Frontend_Setup` detour and drops the static
world, the queue, the transient pool, all model geometry, all uploaded textures, the
material table and the per-run diagnostic sets. It also clears `m_submitted_world` and
`m_race_camera`, so the next race re-establishes both from scratch.

**Known consequence:** the pause menu reaches `Frontend_Setup` too, so pausing and resuming
mid-race flushes and rebuilds. Nothing is wrong afterwards -- the world re-bakes within a
second or so -- but it is a hitch and a `track state flushed` line where none is needed.
Narrowing that needs a way to tell an abandoned race from a paused one, which the argument
to `FrontendEnterFromRace` may carry.

---

## 17. Pause is not a track change (2026-08-02)

Section 16's flush fixed the track transition and broke the pause menu: pausing and
resuming left the textures wrong. `Frontend_Setup` fires for both, and flushing a track
that is still loaded is not recoverable -- `m_materials` is taught the `br_material::stored`
tokens by the level loader, and nothing re-teaches them once a race is running, so every
model came back resolving to its fallback material.

Two separate mistakes were folded into one flush, and they are now separate.

### 17.1 A cache entry has to prove it still describes its key

The real defect behind the misaligned textures was never the lifecycle. It was that
`m_textures` and `m_geometry` trusted a raw game pointer as an identity, and the allocator
reuses those addresses. Each entry now carries what it was built from and re-checks it on
every lookup:

* `model_identity`: `prepared`, `vertices`, `faces`, `nvertices`, `nfaces`.
* `pixelmap_identity`: `pixels`, `map` (the palette), `row_bytes`, `width`, `height`, `type`.

A mismatch rebuilds the geometry or re-uploads the texture. This holds whatever the
lifecycle detection decides, and it covers recycling that happens for reasons nobody has
enumerated -- which is the only honest position to hold about an allocator.

### 17.2 A track load rebuilds models; a pause does not

That leaves the static world, which is the only genuinely track-scoped state: sealed chunks
hold geometry copied out of the old track, and actors that never get walked again would
never punch themselves out.

`Frontend_Setup` starts a count instead of a flush. At the first race scene back,
`BrModelUpdate` calls made while away decide what happened: loading a level runs it over
every model it reads, in the hundreds; the frontend's rotating car previews rebuild a
handful; resuming from pause rebuilds none. `TRACK_LOAD_MODEL_REBUILDS` is 100 and both
outcomes log the count, so the margin is visible in any run rather than assumed.

On a load the static world is dropped and the previous track's geometry and textures are
released -- not for correctness, which 17.1 already owns, but so a session does not
accumulate every track's uploads. `m_materials` is deliberately kept: the loader filled it
on the way in.

---

## 18. A dropped submit froze the scene counter (2026-08-02)

The pause menu stopped damaging textures and started destroying the bake instead: after a
pause the static world was gone and did not come back, and geometry popped out of
existence. Nothing in section 17 touches the chunks on a pause, so the cause was older.

`m_scenes_submitted` advances on the **last line of a completed submit**. `submit` returns
early whenever `build_projection` fails -- no camera, or a camera whose yon is not past its
hither, which is what the pause menu leaves behind. The scene walk in front of it runs in
full regardless: `BrZbSceneRenderBegin`, every `BrZbModelRender`, `capture_model` for each.

So a dropped submit leaves two consecutive walks carrying the same scene number, and
section 14.2's stencil test -- an actor drawn twice in one scene is an instancing stencil
and may never bake -- fires on **every actor in the level at once**. `bakeable` is cleared
permanently, so the static world is not merely dropped, it can never rebuild for the rest
of that track. One frame with no usable projection was enough.

The stencil test is right; it was counting the wrong thing. `m_scene_walks` is incremented
in `begin_scene`, unconditionally, and everything that means "this scene" -- an actor's
`last_seen_scene`, a geometry entry's `queued_scene` -- now compares against that.
`m_scenes_submitted` keeps its own meaning for the report, the seal quiet window and
geometry eviction.

### 18.1 What else a track load was carrying over

`m_race_camera` survived the flush. A race scene is recognised by comparing the current
camera against it, so the previous track's camera pointer -- freed, and a candidate for
recycling into anything -- was still the reference on the way into a new level. It is
cleared when the frontend comes up, and the first submit of the next race establishes the
real one. `m_submitted_world`, the frame clock and the window-worst sample are reset with
the rest, so a load does not report itself as one enormous scene.

Raster geometry appearing for a second or two after a load is the rebuild, not a bug: with
no sealed chunks nothing is suppressed, so the game's own rasterized stream is all Remix
has to composite until the chunks seal. It should stop once the first seal lands, and if it
keeps recurring mid-race the demotion tally in the report says which bucket is churning.

---

## 19. The pause was crossing the load threshold (2026-08-02)

The run log settles why a pause still killed performance: real track loads rebuild 3139,
3181 and 2141 models -- and the pause rebuilt **152**, comfortably over section 17's
threshold of 100, so the pause was flushed as if a track had loaded.

The raw call count was the wrong measurement, not just the wrong threshold. The frontend
re-rebuilds the same few preview models every frame, so the count grows with time spent in
the menu, and any threshold loses eventually. **Distinct models** measure what was read in:
a load runs BrModelUpdate over every model of the track, a menu over the same handful again
and again. `m_frontend_models` is now a set, the threshold is 400 distinct, and both
decisions still log their number.

Also fixed from the same log: the per-reason dynamic counters were never reset per scene
("vanishing 905918" is a session total, not a scene's), so they now clear in `begin_scene`
alongside the rest.

### 19.1 The erosion that remains is road materials marked "animated"

After each genuine load the world seals fine (55-60 fps at scenes 1800/2400), then decays:
`road1` animates (67 baked actors out), `slab1l` (61), `sidexxx?xxxx.MAT` (50), `concwalll`
(28), `2rokgrx` (31) -- by scene 3000 the `animated` tally is 110 and the fps is 41.
Roads are the biggest triangle counts in the level, so these evictions carry real cost, and
every one also re-enables the game's own render of that geometry.

Whether they are genuine funk animation (several tracks do animate surface textures) or a
misread -- BR_MATU_MATERIAL rides along with non-visual updates, and the opacity byte is
resolved through a token list some materials share -- is exactly what the log could not
say, because the eviction line did not name its trigger. It now does: `UV transform`,
`opacity A -> B`, or both. If the next run shows roads evicted on `opacity 255 -> 128`
style transitions, the tint overlay's shared token list is the suspect; if `UV transform`,
the funk system genuinely owns those surfaces and the cost is the game's design.

---

## 20. `opacity 0 -> 255`: the erosion was an uninitialized baseline (2026-08-02)

Section 19's trigger detail ran for one session and convicted the opacity path outright.
Every single road-class eviction reads `animates (opacity 0 -> 255)` -- road1 (67 actors),
road2 (33), 1rok642 (18), concwalll (14), 2rokgrx (18) -- and 0 is not a value the game
ever writes. It is `material_state`'s default.

The entry for a material is created by whichever update arrives first. When that update
carries only `BR_MATU_MAP_TRANSFORM` -- the funk system's per-frame UV tick, bit 0 alone --
the opacity branch never runs and the baseline stays 0. The next ordinary
`BR_MATU_MATERIAL` update reads the true 255, compares it against 0, and calls it a fade:
the material is marked animated, everything baked with it is evicted for the race, and
roads are the biggest geometry in the level. The session log shows `animated` climbing to
141 actors on the revisited track with `game` at 9.4 ms and 1871 glide draws -- the evicted
roads back on the game's own render -- against `animated 0` and a locked 62-64 fps on the
first track. That asymmetry is track content, not session state: the first track's
materials never take both update kinds, the desert's and city's roads do.

The fix is one move: the baseline is read from the material at entry creation, whatever
flags the creating update carries. A genuine fade still registers -- smoke's first sighting
mid-fade differs from its next value -- and a material whose opacity never moves can no
longer manufacture a transition out of the default.

The two `UV transform` evictions in the same log (`room2`, `gDefault_track_material`, at
the frontend boundary) are the genuine article and cost 7 actors between them.

---

## 21. Fog / depth cue (2026-08-02)

### Summary

C2 has no global fog state in the renderer. Fog is **per-`br_material`**: the race TXT
carries one "depth cue" block, the loader stores it in a small block of globals, and
`ApplyDepthCueToMaterial` (0x004451A0) bakes it into `flags` / `fog_min` / `fog_max` /
`fog_colour` of each material that needs it. `BrMaterialUpdate` (0x00520E70) then publishes
those four fields to the renderer as `BRT_FOG_T / FOG_MIN_F / FOG_MAX_F / FOG_RGB` on the
`BRT_PRIMITIVE` state part. A d3d9 proxy hooking `ModelRenderStyle_Faces` already has the
material pointer, so **fog can be read straight off the material** -- no token interception
needed -- with the level globals available as a cross-check.

### Key Addresses

| Address | Description |
|---------|-------------|
| 0x00504BF0 | Race/level TXT loader; depth-cue block parsed at 0x00505E6B..0x00505EC7 |
| 0x00660E90 | Depth-cue mode keyword table: `{"dark","fog","colour"}` (index 0,1,2; no match = -1) |
| 0x0048FA70 | ParseEnumFromList(ecx=file, edx=table, count) -> index |
| 0x0048FDC0 | ParseTwoInts(ecx=file, edx=&a, [b]) -- `"%d"`, separators `"\t ,/"` |
| 0x0048FE30 | ParseThreeInts(ecx=file, edx=&r, [g],[b]) |
| 0x00481C29 | Level start: `SetDepthCue(level block..., apply=1)` |
| 0x00445340 | **SetDepthCue** -- stores live state, then walks material lists applying it |
| 0x004451A0 | **ApplyDepthCueToMaterial(ecx = br_material\*)** -- writes flags/fog_min/fog_max/fog_colour |
| 0x00447220 | CommitLevelDepthCue -- copies the level block into the live block |
| 0x00446CC0 | Debug key: cycles depth-effect mode ("Fog mode" / "Colour Fog mode" / "Darkness mode" / "Depth effects disabled") |
| 0x00520E70 | `BrMaterialUpdate(material, parts)`; fog tokens emitted at 0x005210FE..0x00521150 |
| 0x00521181 | `g_pRenderer->vtbl+0x84` (partSetMany) with part `BRT_PRIMITIVE` (0x7C) |
| 0x00445620 | Loads DEPTHCUE.TAB / FOG.TAB / ACIDFOG.TAB / BLUEGIT.TAB shade tables + HORIZON.MAT |

### 21.1 `br_material` fog fields (BRender struct-reflection table at 0x006637F0)

The file-format reflection table for `br_material` (struct size 0x9C) names them explicitly:

```
type=0x05 off=0x20  flags
type=0x0a off=0x5c  fog_min          (br_scalar -> float in this build)
type=0x0a off=0x60  fog_max          (br_scalar -> float)
type=0x12 off=0x64  fog_colour       (br_colour)
```

* `flags & 0x00080000` = **`BR_MATF_FOG_LOCAL`** -- fog enable for this material.
  (Confirmed by the neighbouring bits in `BrMaterialUpdate`: 0x10000 -> `MAP_ANTIALIASING_T`,
  0x20000 -> `MAP_INTERPOLATION_T`, 0x40000 -> `MIP_INTERPOLATION_T`, 0x80000 -> `FOG_T`.)
* `fog_min` / `fog_max` are **IEEE floats**, not 16.16 fixed. This build is the float BRender:
  `BrMaterialUpdate` emits `FOG_MIN_F` (0x97) / `FOG_MAX_F` (0x99), never the `_X` fixed-point
  tokens 0x98 / 0x9A.
* `fog_colour` is `BR_COLOUR` = **0x00RRGGBB** (packed at 0x004452F8: `edi = ((R<<8)|G)<<8 | B`).

### 21.2 The level depth-cue globals

Parsed straight out of the race TXT at 0x00505E6B (all `int`):

| Address | Field |
|---------|-------|
| 0x0075D744 | `g_depthCueType` -- -1 none, **0 dark**, **1 fog**, **2 colour** (keyword table 0x00660E90) |
| 0x0075D748 | `g_depthCueP1` -- fog-start exponent (decibel-ish, see formula) |
| 0x0075D74C | `g_depthCueP2` -- fog-end exponent |
| 0x0075D750 | `g_depthCueR` (0..255) |
| 0x0075D754 | `g_depthCueG` |
| 0x0075D758 | `g_depthCueB` |
| 0x0075D75C | `g_depthCueShadeTable` -- `br_pixelmap*` (DEPTHCUE/FOG/ACIDFOG/BLUEGIT .TAB) |

`SetDepthCue` mirrors that block into the **live** set, which is what actually drives the
materials -- this is the set a proxy should read every frame:

| Address | Field |
|---------|-------|
| 0x0075D760 | `g_fogType` (live) |
| 0x0075D764 | `g_fogP1` (live) |
| 0x0075D768 | `g_fogP2` (live) |
| 0x0075D76C | `g_fogR` (live) |
| 0x0075D770 | `g_fogG` (live) |
| 0x0075D774 | `g_fogB` (live) |
| 0x0075D778 | `g_fogShadeTable` (live `br_pixelmap*`) |
| 0x0074CAA8 / 0x0074CF2C / 0x0074CAD0 | duplicate copies of level R / G / B (written at 0x00505EB6) |
| 0x00761F4C | `g_yon` -- float view-depth / far-distance scale, default **5.0** (0x40A00000), tweakable by debug keys; also written into camera `+0xC` (yon_z) at 0x0047DA0B and 0x0047E405 |

Right after parsing, 0x00505EDF has a conditional override: if the mode is not already 1 it is
forced to `type=1 (fog), p1=7, p2=0, RGB=(0xF8,0xF8,0xF8)` -- a hard-coded white-fog fallback.

### 21.3 Value formats -- how the TXT ints become fog distances

`ApplyDepthCueToMaterial` (0x004451A0), for every mode except "off":

```
material->fog_min   = g_yon * pow(10.0, -g_fogP1 * 0.1)      ; 0x004451EF..0x00445226
material->fog_max   = g_yon * pow(10.0,  g_fogP2 * 0.1)      ; 0x00445210..0x00445245
material->flags    |= 0x00080000                              ; BR_MATF_FOG_LOCAL
BrMaterialUpdate(material, 0x7FFF)                            ; 0x00445310
```

Constants: 10.0 @0x00589CA8 (double), 0.1 @0x00589CB0 (double), `pow` @0x005769A0,
`g_yon` @0x00761F4C.

So `fog_min`/`fog_max` are **BRender world units, the same units as the camera hither/yon**,
expressed relative to `g_yon`. With the debug default `p1=10, p2=0` and `g_yon=5.0` that gives
`fog_min = 0.5`, `fog_max = 5.0`.

Per-mode `fog_colour` (jump table at 0x00445328, index = `type + 1`):

| type | mode | code | fog_colour |
|------|------|------|-----------|
| -1 | off | 0x004451E3 | `flags &= ~0x00080000` (fog disabled, nothing else written) |
| 0 | dark | 0x004451EF | `0x00000000` (black -- depth-darkening) |
| 1 | fog | 0x00445250 | `0x00F8F8F8` (near-white) |
| 2 | colour | 0x004452AE | `(R<<16)|(G<<8)|B` from the live globals |

### 21.4 How it reaches the renderer / driver

`BrMaterialUpdate` 0x00520E70 builds a `{token, value}` pair array on the stack and submits it
in one call. The fog part (0x005210FE):

```
edx = material->flags & 0x00080000
pairs += { BRT_FOG_T (0x95), edx ? BRT_LINEAR (0x93) : BRT_NONE (0x01) }
if (edx) {
    pairs += { BRT_FOG_MIN_F (0x97), material->fog_min  }   ; [esi+0x5C]
    pairs += { BRT_FOG_MAX_F (0x99), material->fog_max  }   ; [esi+0x60]
    pairs += { BRT_FOG_RGB   (0x96), material->fog_colour } ; [esi+0x64]
}
...
g_pRenderer->vtbl[0x84](g_pRenderer, BRT_PRIMITIVE /*0x7C*/, 0, pairs, &out)   ; 0x00521181
```

Token values resolved from the BRender token-name table (records are 0x18 bytes,
`{char* name, type, token, part}`, aligned on 0x00668C58):

| Token | Value | Data type |
|-------|-------|-----------|
| `FOG_T` | 0x95 | enum (`NONE`=0x01 / `LINEAR`=0x93) |
| `FOG_RGB` | 0x96 | br_colour |
| `FOG_MIN_F` | 0x97 | float |
| `FOG_MAX_F` | 0x99 | float |
| `FOG_MIN_X` | 0x98 | br_fixed (unused by this build) |
| `FOG_MAX_X` | 0x9A | br_fixed (unused) |
| `FOG_TL` | 0x12F | table/pixelmap variant, unused here |
| `PRIMITIVE` | 0x7C | state part these live on |

**Glide driver.** `hardware_3dfx.bdd` imports exactly three fog entry points from `glide2x.dll`:

```
_grFogMode@4         IAT 0x1000B24C   thunk 0x1000598C
_grFogColorValue@4   IAT 0x1000B250   thunk 0x10005986
_grFogTable@4        IAT 0x1000B254   thunk 0x10005980
```

All consumed by one state-apply function at **0x10001100** (driver-local fog state struct):

```
+0x2C fog type   -> 1 (NONE) ? grFogMode(0) : grFogMode(2 /*GR_FOG_WITH_TABLE_ON_Q*/)
+0x30 fog colour -> grFogColorValue()          (cached in 0x10011718)
+0x34 fog_min    \  cached in 0x1000D034 / 0x1001171C; log/exp-mapped into a 64-entry
+0x38 fog_max    /  byte table at 0x100116D8 -> grFogTable()  (0x100011FF)
```

That is the proof the four material fields reach the hardware unchanged.
`hardware_d3d.bdd` contains no fog strings and no matching state block -- the D3D backend
does not implement fog at all.

### 21.5 Per-material exclusions (what is *not* fogged)

Fog is opt-in per material via `BR_MATF_FOG_LOCAL`. Two sources set it:

1. **Authored** -- track/world materials come from `.MAT` files with the flag already set
   (or not). Nothing at load time forces it on for the whole scene.
2. **Patched at runtime** by `SetDepthCue` 0x00445340, which walks a *specific* list of
   engine-generated materials and calls `ApplyDepthCueToMaterial` on each:
   `[0x0075BB60]`, `[0x007634B8]`, the per-car material arrays at `0x00763090`
   (`[0x00762430]` entries, each iterating `[car+0xE14]` materials at `[car+0xE0C]`),
   `BrMaterialFind("GIBSLICK")`, `BrMaterialFind("PEDSMEAR")`, `0x0074CEE8..0x0074CEF0`,
   `0x007632CC..0x00763484` (stride 0x28), and `[0x0074B74C]`.
   Additional call sites: 0x004EA799 / 0x004EA7F2 (0x006A3340 material array, count
   `[0x006A6D38]`) and 0x004F2BE4.

**The sky/horizon is deliberately excluded.** `HORIZON.MAT` is held in `[0x0067C4E0]` and is
never passed to `ApplyDepthCueToMaterial`; instead `SetDepthCue` writes the shade-table
pixelmap into `horizonMat->colour_map` (`+0x40`) and calls `BrMaterialUpdate(mat, 0x7FFF)`
(0x00445361..0x00445376). The horizon gets its depth cue from the indexed shade table, not
from `BR_MATF_FOG_LOCAL`, so a proxy must not apply scene fog to it.

Related pixelmap globals loaded at 0x00445620:
`0x0079EC20` DEPTHCUE.TAB, `0x0079EC38` FOG.TAB, `0x0079EC24` ACIDFOG.TAB,
`0x0079EC28` BLUEGIT.TAB, `0x0067C4E0` HORIZON.MAT (`br_material*`),
`0x0067C4A0` SHADETAB (`br_pixelmap*`).

### Suggested Live Verification

* Read `[0x0075D760]` (type) and `[0x0075D764..0x0075D774]` after a race loads on a foggy
  level and confirm they match the TXT.
* Breakpoint 0x004451A0 during level load; dump `ecx` and the resulting
  `[ecx+0x20] & 0x80000`, `[ecx+0x5C]`, `[ecx+0x60]`, `[ecx+0x64]` to confirm the formula
  and the 0x00RRGGBB packing.
* In the proxy's `ModelRenderStyle_Faces` hook, log `material->flags & 0x80000` per draw and
  check that the sky/horizon group comes through with the bit clear.
* `mem write` `[0x00761F4C]` (g_yon) and confirm fog distances move with it -- that decides
  whether fog range should be re-derived from the proxy's own far plane.

---

## 22. Fog restored through Remix's legacy fog remapping (2026-08-02)

Section 21's depth cue is now re-published by the proxy. dxvk-remix's `setFogState`
(d3d9_rtx_utils.cpp) captures the plain fixed-function fog render states off every draw --
`D3DRS_FOGENABLE / FOGCOLOR / FOGSTART / FOGEND / FOGTABLEMODE / FOGVERTEXMODE` -- and its
scene manager renders "the first unreplaced fog" of the frame. With
`rtx.volumetrics.enableFogRemap` on, a `D3DFOG_LINEAR` state's colour and end distance drive
the volumetric transmittance colour and measurement distance
(rtx_global_volumetrics.cpp:472).

`apply_fog` in `submit()` therefore sets exactly those states before the injected passes,
and the state-block restore keeps them off nGlide's stream. Values come from
`game::read_scene_fog()`, which mirrors `ApplyDepthCueToMaterial` from the live globals
each scene: type (`[0x0075D760]`, -1/0/1/2) picks the colour (black / 0xF8F8F8 / level RGB),
and `fog_min/max = g_yon * 10^(∓p/10)` with the exponents from `[0x0075D764/68]` and g_yon
from `[0x00761F4C]`. Always `D3DFOG_LINEAR` -- the game's BrMaterialUpdate never emits any
other fog type (0x005210FE). Reading the globals rather than sampling baked materials means
the fog survives the static world: a sealed chunk's materials are never re-read, but the
globals are live every scene, and the debug keys that cycle the depth-cue mode mid-race
(0x00446CC0) are reflected immediately. The change is logged once per state change as
`depth cue: fog colour RRGGBB, min to max world units`.

Switch: `[Effects] Fog`, default on. Remix side: `rtx.conf` now carries
`rtx.volumetrics.enableFogRemap = True` and `enableFogColorRemap = True` (colour remap is
off by default in Remix and without it a track's red haze would stay grey);
`enableFogMaxDistanceRemap` already defaults on. The remap only takes effect while Remix's
volumetric lighting is enabled. The sky needs no exclusion on our side -- HORIZON.MAT is
fogged through a shade table, never through `BR_MATF_FOG_LOCAL`, and the horizon never
passes through the injection; `rtx.fogIgnoreSky` exists as a further guard if it ever does.

Not carried over: the per-material `fog_min/max/colour` fields (all materials share the
level values in practice -- Remix supports one fog per frame anyway) and the indexed shade
tables (DEPTHCUE/FOG/ACIDFOG/BLUEGIT.TAB), which only matter to the software renderer's
palette path.

---

## 23. Dark fog colours must not become the transmittance colour (2026-08-02)

Enabling volumetrics with the section 22 config blacked the world out on New City 3, with
only a faint red glow left, and no distance slider recovered it. The failure is exact and
slider-proof: Remix derives the medium's attenuation as `sigma = -ln(transmittance)/D` per
channel (rtx_global_volumetrics.cpp:544) and attenuates every infinitely-distant light --
the sky, this port's only light source -- over a hard cap of `5 * D`
(`maxAttenuationDistanceForNoAtmosphere`, :625). D cancels: **surviving sky light =
transmittance^5, whatever the distances say.** With `enableFogColorRemap` on, a track's fog
colour is that transmittance; 0x500000 gives 0.08^5 in red and (1/255)^5 in the clamped
green/blue. Any dark or saturated depth cue -- "dark" mode outright, the red tracks -- is a
guaranteed blackout, while white-fog tracks survive (0.94^5 = 0.73), which is why the first
test looked fine.

The game's depth cue is a *display* fade toward a colour, not a physical medium colour, and
the world it fades is lit by a sky the physical reading would occlude. So the colour rides
in the fog's own glow instead: `enableFogColorRemap = False` (transmittance stays the
neutral 0.93 preset -> sky keeps 0.93^5 = 70%), and the track colour enters through
`fogRemapColorMultiscatteringScale = 0.35`, which scales `fogState.color` into the fog's
ambient in-scatter (:539). Distance remapping stays on, so fog extent still follows each
track's `FOGEND`.

The camera-position diagnostic also settled the atmosphere question for later: the race
camera sits at world y = 8.5 on New City 3, so the world is Y-up at the origin and Remix's
y=0-anchored atmosphere shell fits -- `enableAtmosphere` should work with a raised
`atmosphereHeightMeters` (the default 30 is marginal for city verticality), and
`atmosphereInverted` is not needed. Retest atmosphere only after the transmittance fix,
since transmittance^(path) kills it identically through the long horizontal paths a finite
shell produces.

---

## 24. The proxy decomposes the fade colour into medium terms (2026-08-02)

Section 23's config kept the world lit but lost the red: with colour remap off, the only
hue left was the multiscattering ambient, and a red *transmittance* could never have
supplied it anyway -- a physically red medium scatters the complementary hue, so it glows
cyan and merely reddens what is behind it. The game's authored value is a display fade
target; Remix has no single parameter with that meaning, so the proxy now splits it into
the parameters Remix does have, pushed over the bridge API (`SetConfigVariable`) once per
depth-cue change from `push_fog_to_remix`:

| game colour becomes | Remix parameter | why there |
|---|---|---|
| saturation (brightness-normalized hue, `0.25 + 0.70 * hue`) | `rtx.volumetrics.singleScatteringAlbedo` | the colour of the fog's own glow; an albedo cannot darken anything |
| a whisper of hue (`0.97 - FogTint * (1 - hue)`, FogTint 0.08) | `rtx.volumetrics.transmittanceColor` | distance reddening; bounded because sky survival is transmittance^5 |
| the raw value | `D3DRS_FOGCOLOR` (unchanged) | the multiscattering term reads the fog state directly |

"dark" mode (colour 0x000000) has no hue and gets a dim neutral medium instead: albedo
0.35, transmittance 0.90. Depth cue off restores the rtx.conf presets (0.95 / 0.93). The
first push can beat the bridge to the first submit, so it retries each scene until the
bridge is up (`m_remix_fog_synced`).

Switches: `[Effects] FogVolumetrics` (default on) gates the API pushes;
`[Effects] FogTint` is the transmittance-hue budget, clamped to 0.5 -- the ^5 law makes
even 0.2 cost the weakest channel ~96% of the sky. `enableFogColorRemap` must stay off in
rtx.conf: it would overwrite the pushed transmittance with the raw fade colour and
reintroduce the section 23 blackout.


---

## 25. Sprite particles: one model, many textures (2026-09-02)

Three symptoms, one cause: **every sprite system in this game keeps its geometry
constant and animates by rewriting `br_material::colour_map`**. A cache that resolves a
group's texture once, when the geometry for a `br_model` is built, therefore freezes the
first frame it ever saw and hands it to every later draw of that model.

None of the systems below calls `BrModelUpdate`. None of them touches `map_transform`.
None of them rewrites pixel data. The only per-particle appearance channels are
`material->colour_map`, `actor->material`, and (smoke only) `actor->model` and the vertex
colours. Everything else -- size, spin, billboard orientation -- is in the actor transform.

### 25.1 The shared 50-slot sprite pool: explosion fire, sparkle, blood

`InitSpriteParticlePool` @ **0x004EA880** (documented in section 6.6 as
`InitImpactDecals`, which is wrong -- it is not a decal pool) builds **50 slots** at
`g_sprite_particles` **0x006A55C8**, stride `0x78`. Each slot gets its **own**
`br_actor` (type MODEL, `render_style = FACES`), its **own** `BrModelAllocate(NULL, 4, 2)`
-- a unit XY quad, `x,y in -0.5..0.5` -- and its **own** `BrMaterialAllocate("BANG!")`
whose `colour_map` starts as `g_bang_pixelmap` (0x0074D360) and whose flags are
`&= ~BR_MATF_LIGHT` then `|= 0x800`. `slot[0x0A] = 1` marks the slot free.

Every data-driven sprite effect in the game draws from this one pool:

| effect | emitter list | frames | spawned from |
|---|---|---|---|
| car "wasted" explosion | `0x006A550C` (block `0x006A52D0` + 0x23C) | `ex00001..ex00007` | GENERAL.TXT, "Wasted explosion settings" |
| powerup collect | `0x006A7F1C` | `BING1..6`, `TWINK1..4` | GENERAL.TXT, "Powerup connotations" |
| powerup respawn | block `0x006A3660` | `TWINK1..4` | GENERAL.TXT, "Powerup respawn connotations" |
| ped blood clouds | `0x00694478`, `0x0069BC28` | `BIGBL01..05` | PEDS/SETTINGS.TXT, "SMALL/MED/LARGE BLOOD CLOUD SPEC" |
| car impacts | `0x007620F8` | per-track | track TXT |

`ParseSpriteEmitterList` @ **0x004EE780** reads one "explosion group" block into a
`0x44`-byte `br_sprite_emitter`. Frames are a heap array of `{float, br_pixelmap*}`
pairs at `emitter+0x40`, `emitter+0x04` frames long:

```
0x004EE8C9  frames = BrMemAllocate(nframes * 8, 0xFC)
0x004EE8D3  emitter->frames = frames
0x004EE8DE  ParseFloat()                            ; the authored opacity ("50", "75", "100")
0x004EE8E8  fstp [frames + i*8]
0x004EE8F2  mov  dword [frames + i*8], 0x42C80000   ; ... immediately overwritten with 100.0f
0x004EE902  frames[i].map = BrMapFind(name)         ; error 0x77 "can't find pixelmap"
```

so **the per-frame opacity in the data files is dead** -- only the pixelmap pointer
survives. The frames are separate `br_pixelmap` objects found by name, never one
pixelmap whose pixels get rewritten.

`SpawnSpriteParticles` @ **0x004EAD00** (`ecx` = `{count, br_sprite_emitter*}`,
`edx` = owner, arg0 = `&model->bmin`, arg1 = optional world origin) claims a free slot by
linear scan, and if none is free steals the next one round-robin from
`g_sprite_particle_next` (0x006A82A0). It then **copies the emitter's whole frame table
into the slot** at `slot+0x28` (`nframes * 2` dwords) and stores `nframes` at `slot+0x09`.
The owning effect is only remembered as an opaque pointer at `slot+0x0C`.

`AnimateSpriteParticles` @ **0x004EAAF0** (called once per frame from 0x00493A4E) is
where the appearance changes:

```
0x004EABA6  if (frame_index != (char)slot[0x08])                 ; frame changed this tick
0x004EABB2      actor->material->colour_map = slot->frames[frame_index].map
0x004EABBF      BrMaterialUpdate(actor->material, 0x7FFF)
0x004EABE1  memcpy(&actor->t, &g_effects_camera_actor->t, 48)    ; billboard
0x004EAC1B  actor->t.translate = owner->t.translate + offset
0x004EAC71  scale(&actor->t, map->width * seed / 128, map->height * seed / 128, 1)
0x004EAC85  rotateZ(&actor->t, slot->angle)
```

`frame_index = (now - slot->death_time) / slot->frame_period`; when it reaches
`slot[0x09]` the actor is `BrActorRemove`d and the slot is freed. The actor is
`BrActorAdd`ed to `g_effects_parent_actor` (0x007634B8) and rendered by the ordinary
scene walk -- **not** `BrZbSceneRenderAdd`.

**Why blood shows the sparkle texture.** A slot's `br_model` is allocated once at startup
and never freed, but its `br_material->colour_map` is rewritten by whatever effect owns
the slot at the moment. A cache keyed on `br_model` that resolves the texture at build
time captures the pixelmap that slot happened to be showing the first time it was drawn --
`TWINK3`, say -- and keeps handing it back after the slot is recycled for a blood cloud.
The 50 quads are also geometrically identical, so nothing in the geometry distinguishes
them.

### 25.2 Car flames: 30 sprites, one model

`InitFlames` @ **0x004FC3A0**:

```
0x004FC3CF  g_flame_model = BrModelAllocate("Lollipop", 4, 2)             ; 0x006AA380
0x004FC421  LoadPixelmapMany("FLAMES.PIX", g_flame_pixelmaps, 20) -> 20   ; 0x006A8638
0x004FC442  BrMapAddMany(g_flame_pixelmaps, 20)
            for slot in 0..9:                                             ; 0x006A96AC, stride 0x7C
0x004FC453    slot->root = BrActorAllocate(BR_ACTOR_NONE, NULL)
              for child in 0..2:
0x004FC469      child = BrActorAllocate(BR_ACTOR_MODEL, NULL)
0x004FC475      mat   = BrMaterialAllocate(NULL)
0x004FC492      child->model    = g_flame_model                           ; SHARED
0x004FC499      child->material = mat                                     ; per child
0x004FC4A5      mat->flags &= ~BR_MATF_LIGHT;  mat->flags |= 0x800
0x004FC4C5      mat->colour_map = g_flame_pixelmaps[0]
```

The quad is `x -0.5..0.5, y 0..1` with UVs 0..1 -- anchored at the bottom edge, so a flame
grows upward from its origin. `FLAMES.PIX` is a container holding `FLM01..FLM20`
(DATA/COMMON/flames/PIX16); a byte table of the 20 source sizes lives at
`g_flame_frame_size` **0x00660118** (`{20,25} {24,32} {24,34} {28,44} ...`).

`UpdateFlameSlot` @ **0x004FBDD0** advances all three children each tick:

```
0x004FBDF6  root->t.translate = pos
0x004FC020  frame = ++slot->frame[i]        ; re-randomised when it passes 19
0x004FC059  child->material->colour_map = g_flame_pixelmaps[frame]
0x004FC066  BrMaterialUpdate(child->material, 0x7FFF)
0x004FC08x  scale(&child->t, size_w * sx, size_h * sy, 1.0)
0x004FC0Ax  child->t.translate.x = ...;  child->t.translate.z = ...
0x004FC0B4  child = child->next
```

**Why every explosion flame shows the same frame.** All 30 flame sprites (10 burning cars
x 3) point at the one `"Lollipop"` `br_model`. A per-`br_model` geometry cache has exactly
one entry for them and therefore exactly one texture, so the whole fire animates as a
single frame instead of 30 independent ones. This is the same class of bug section 12.5
already fixed for the smoke quad -- but there the tell was vertex colour; here it is the
texture, which the cache still resolves only once.

`StartCarFire` @ 0x004FCAB0, `IsCarOnFire` @ 0x004FED90, `RemoveAllFlameActors` @
0x004FC9E0, `ShutdownFlames` @ 0x004FC2E0. `g_flame_slot_mask` (0x006AA59C) has a bit per
live slot.

### 25.3 Splashes: one model, one material per frame, fixed per actor

`InitSplashes` @ **0x004FDDE0** builds `g_splash_model` (`"Splash"`, 0x006A8758) -- the
same quad as Lollipop -- loads up to 20 pixelmaps (`SPLSHBLU.PIX` by default, or a name
list when `ecx != 0`), and then does something the other systems do not: it allocates
**one `br_material` per animation frame** into `g_splash_frame_materials` (0x006A9130,
count in 0x006AA5A4), each with `colour_map` fixed to that frame's pixelmap. 32 actors at
`g_splash_slots` (0x006A82B8, stride 0x1C) each get `model = g_splash_model` and
`material = g_splash_frame_materials[rand()]` -- **chosen once at startup and never
changed**.

`SpawnSplash` @ **0x004FD530** only sets position, size and the alive bit and
`BrActorAdd`s the actor; `EffectsTick` @ **0x004F9790** only writes the transform. So a
splash never animates: its texture is whatever material it drew at init. Same shared model
for all 32, so a per-`br_model` cache collapses them onto one texture.

### 25.4 Smoke: the one system that swaps the model too

Correcting and completing section 12.1 -- `DrawSmokeParticles` @ 0x004FB1B0 changes three
things per particle, and the model is one of them:

```
0x004FB236  gBlend_actor->material = record[0x1C]          ; one of the 35 "some smoke"
0x004FB258  material->extra[1].value = alpha*150*65536     ; OPACITY_X
0x004FB25F  BrMaterialUpdate(material, 0x40)               ; BR_MATU_EXTRA
0x004FB26F  esi = record[0x20]                             ; gBlend_model OR gBlend_model2
0x004FB285  vertex[i].red/green/blue = record[0x14]
0x004FB2AF  BrModelUpdate(esi, 2)                          ; BR_MODU_VERTEX_COLOURS
0x004FB2BD  gBlend_actor->model = esi                      ; model swapped per particle
0x004FB2C7  BrZbSceneRenderAdd(gBlend_actor)
```

`gBlend_model2` (6 verts / 4 faces, 0x0074CF94) is picked at spawn time by the emitters at
0x004FAA89 / 0x004FAC08 / 0x004FACA8 / 0x004FAF70 / 0x004FB010. Smoke is the only sprite
system that uses `BrZbSceneRenderAdd` and the only one that calls `BrModelUpdate`.

### 25.5 What BrZbModelRender actually receives

`BrZbActorRender` @ 0x005221E0 resolves the model/material/env before dispatching:

```
0x005221FE  mat   = actor->material;  if (mat == NULL)   mat   = inherited
0x00522209  model = actor->model;     if (model == NULL) model = inherited
```

and passes `mat` as `BrZbModelRender`'s **3rd argument**. Every sprite model above is
built with `BrModelAllocate` and its faces' `material` field left NULL, so the material a
hook sees is always `actor->material` -- the object whose `colour_map` the game mutates.

### 25.6 What varies per particle

| system | model | actor->material | material->colour_map | pixel contents | map_transform | vertex colours |
|---|---|---|---|---|---|---|
| sprite pool (explosion fire, sparkle, blood, BANG) | per slot, constant | per slot, constant | **rewritten every frame** | never | never | never |
| car flames | **one shared model for all 30** | per child, constant | **rewritten every tick** | never | never | never |
| splashes | one shared model for all 32 | per actor, fixed at init (random frame) | never after init | never | never | never |
| smoke | **swapped per particle** (2 models) | **swapped per particle** (35 materials) | never | never | never | **rewritten per particle** |
| sparks (sections 6.2 / 6.5) | one shared line model | constant | n/a (untextured) | never | never | **rewritten per spark** |

For the proxy the consequence is the same in every row: **`br_model` alone is not a cache
identity for sprite geometry.** The identity has to include the resolved texture -- i.e.
`material->colour_map` -- and it has to be re-read per draw rather than at build time,
because `BrMaterialUpdate` is called with `0x7FFF` (which includes `BR_MATU_COLOUR_MAP`)
between draws of the same model. Treating a `BrMaterialUpdate` that carries
`BR_MATU_COLOUR_MAP` the way section 12.5 treats a mid-scene `BrModelUpdate` -- demote to
a transient per-scene entry -- covers all five systems, since a sprite model is never
static.

---

## 26. Sprites, glass and the raster twin: three fixes in the proxy (2026-09-02)

Three reported symptoms -- sprite textures mixed up (fire showing one frame for every
particle, blood wearing the pickup sparkle), car windows "painting" instead of reflecting
once given a translucent material, and raster gameplay bleeding through -- come down to
two defects in the proxy.

### 26.1 Material state was cached with the geometry

Section 25 established that every sprite system draws one shared quad model per particle
and animates it by rewriting `actor->material->colour_map`, which reaches the hook as
BrZbModelRender's material argument. The proxy resolved a group's texture once, when the
`br_model`'s geometry was built, and `refresh_part_state` re-read only `map_transform` and
opacity into that same shared entry. Two consequences: the texture froze at whatever the
model first drew with (blood in a slot that last showed `TWINK3`), and every capture of the
same model in one scene overwrote the state the earlier captures were queued against, so
the last particle's frame was drawn thirty times (the flames).

What a draw looks like is now separate from what it is made of. `geometry_part` keeps only
the index range, the material the run was built against and whether that run inherits the
render call's material (`material_token == 0`, i.e. faces authored without one, which is
every sprite quad). `resolve_draw_state` runs per capture and appends one `draw_state`
per part to a per-scene pool: the material re-resolved through the inheritance, the
texture re-read from that material's current `colour_map` (`texture_for` already
re-checks the pixelmap identity), `MaterialNeedsAlpha`, the UV transform and the opacity.
`queued_model` records where its states start; `draw_pass` reads them from there. The
static chunks keep using the build-time part state, which is what they bake.

### 26.2 The game's raster twin of every dynamic model was composited over Remix

dxvk-remix path-traces what it has been given up to its injection point and composites
every draw after that point on top of the result. It picks the point itself: the first
draw that `isRenderingUI()` accepts -- a bound texture tagged in `rtx.uiTextures`, or an
orthographic projection with depth writes off (d3d9_rtx.cpp `makeDrawCallType`). nGlide's
pre-transformed draws are otherwise `Rasterized, false`: drawn into the back buffer, then
overwritten wholesale when `injectRTX` blits the path-traced image over the target.

With `SuppressDynamics=0` every dynamic model was rendered twice: once in model space by
the proxy, once by BRender + nGlide in screen space. Whenever the game's draws landed after
the injection point -- nGlide holds its last batch until the next state change, and the
translucent bucket BRender draws last is exactly the car windows and sprites -- that twin
was painted flat over the path-traced frame. For a window that twin is BRender's
env-mapped glass, a static painted reflection: the "paints rather than reflects" look.
Water never showed it because it is chunk-covered and already suppressed.

Two changes:

* **Suppression rewrites the render style instead of skipping the call.** `hk_model_render`
  forwards `BR_RSTYLE_NONE` for a captured model, so BrZbModelRender still publishes its
  state and still dispatches a custom callback, but the style thunk it lands on is a bare
  return: no software T&L, nothing to nGlide. Skipping the call was why SuppressDynamics
  could not be defaulted on -- it also skipped `model->custom`, which is how the powerup
  icons draw (PowerupModelCustomCB ends in BrZbModelRender via 0x00523070). A model with a
  callback is now left to that nested call for its capture, and captured after the fact only
  if the callback rendered nothing through the hook. `SuppressDynamics` defaults on and is
  scoped to the race camera; the 3D HUD widgets keep their game render.
* **The injection point is pinned.** `trigger_injection` issues one triangle outside the
  clip volume under an orthographic projection with depth writes off at the end of every
  race submit. Remix classifies it as UI and injects there, so the path-traced frame is
  complete when it is composited and everything nGlide draws afterwards -- the HUD, and any
  batch it still held -- lands on top. `[Remix] TriggerInjection` switches it off.

### 26.3 The 50-slot pool at 0x006A55C8 is sprites, not decals

Section 6.6 called it the impact decal pool. Section 25.1 shows it is the shared sprite
billboard pool (fire, sparkle, blood, "BANG!"). The proxy no longer lifts those quads along
their normals; they stay in `vanishes_outright`, since they are re-placed rather than moved.

---

## 27. The race frame in order, and how it reaches Glide (2026-09-02)

Static only (Ghidra project, program `CARMA2_HW.EXE`; the file on disk is now
`CARMA2_HW0.EXE`, byte-identical, renamed by the Remix launcher). Confidence: **[C]**
confirmed from disassembly, **[H]** one inference, **[?]** unresolved.

### 27.1 Where the Glide code lives

**[C]** The EXE has no `glide2x.dll` import and no `gr*` thunk. Every Glide entry point is
imported by **`3dfx_win.bdd`** (base 0x10000000, single export `BrDrv1Begin`), which BRender
maps with its **own PE loader** (`BrDLLLoad` 0x0052FFA0 -> `FUN_00530E70`: checks MZ/PE,
maps sections, resolves imports itself, falls back to `LoadLibraryA`). Its
`BRCORE1/BRHOST1/BRPMAP1` imports resolve against the statically linked BRender; only its
`glide2x.dll` import goes through the OS loader, which is how nGlide enters the process.
`PDAllocateScreenAndBack` @ **0x0051C300** then `strcmp`s the screen pixelmap identifier
against `"Voodoo Graphics"`.

Pixelmaps it creates:

| global | what |
|---|---|
| `0x0074D3E0` | screen pixelmap, 640x480x16 (`BrDevBeginVar("3DFX_WIN", ...)`) |
| `0x0074D360` | **back buffer** (`screen->_match(screen, 0)`) |
| `0x0068B8A4` | **depth buffer** (`_match(back, 1)` @ 0x004E4940) |
| `0x00762128` | **race view colour target**, a sub-pixelmap of the back buffer (0x004E49B7) |
| `0x0068B8A8` | second-view (mirror) colour target, another sub-pixelmap; shares the depth buffer |
| `0x006A22BC` | 64x64 reflection render target (0x004E4B28) |

Glide pixelmap dispatch (bdd, table at 0x1000E480): `_fill` 0x100026C0 -> `grBufferClear`;
`_doubleBuffer` 0x100028F0 -> `grBufferNumPending` x2 + **`grBufferSwap`** (the only swap
site, 0x10002932); `_rectangleCopy` 0x10002A30 -> `grLfbWriteRegion`; `_rectangleFill`
0x10002580 -> `grLfbLock`/write/`grLfbUnlock`; `_line`, `_text`, `_copyBits` are **stubs**.
Triangles: 8 `grDrawTriangle` sites (0x10003BFE..0x10004901) with `grTexSource`,
`grTexCombineFunction`, `guColorCombineFunction`, `grConstantColorValue4`, `grHints` per
batch. Textures go up via `grTexDownloadMipMap` (0x100010E4).

**[C] `grSstIdle` is never called and not even imported** (nor `grSstIdleN`, `grFinish`,
`grFlush`). The only synchronisation is the `grBufferNumPending` pair before the swap.
**There is no flush between the 3D scene and the overlay.**

### 27.2 One race frame

`RaceFrameTick` 0x004939EA -> `RenderAFrame` = **FUN_004E4E40** (called at 0x00493AEA):

| # | address | what | Glide |
|---|---|---|---|
| 1 | 0x004E4E7x | frees last frame's HUD glyph actors (`[0x0074CAE0]`) | -- |
| 2-3 | 0x004E50AB / 0x004E51CE | camera shake | -- |
| 4 | **0x004E52A4..533F** | four letterbox `_rectangleFill`s on the **back buffer**, colour 0, only when `[0x0068BE38]!=0 && [0x0075B9A4]!=2` | `grLfbLock`/write/`grLfbUnlock`, before any 3D |
| 5 | **0x004E5371** | `RenderView(cam=[0x74D35C], colour=[0x762128], depth=[0x68B8A4])` ECX=0 -- **the main race view** (27.3) | `grBufferClear` + `grDrawTriangle` |
| 6 | 0x004E5383 | restores the shaken camera | -- |
| 7 | **0x004E53B4** | `RenderView(cam=[0x75B940], colour=[0x68B8A8], depth=[0x75B93C])` ECX=1, only if `[0x00704E40]!=0` -- **second view (mirror/PiP)**, no reflections/shadows/particles | same |
| 8 | **0x004E53CD** | `TintPolySceneRender` 0x004D8290: `BrZbSceneRender(world=cam=[0x006A0430], colour=[0x0074D360] back buffer, depth)` | `grDrawTriangle` |
| 9 | 0x004E53DA | HUD quad via `FUN_0047CAD0`: rebuild `[0x0074CA70]`, `BrModelUpdate`, own `BrZbSceneRender(world=cam=[0x0074CAC4], colour=back, depth)` @ 0x0047CB9C | `grDrawTriangle` |
| 10 | 0x004E53E5 | CPU sprite blit (`FUN_0047BA80`) into a memory pixelmap | -- |
| 11 | 0x004E53FE | race info text (`FUN_00464E40`, 27.4) | `grDrawTriangle` |
| 12 | 0x004E5405 | dashboard/cockpit: `BrPixelmapRectangleCopy` + CPU blits into memory pixelmap `[0x0074CA1C]`; map via 11x `BrPixelmapLine` (memory) | -- |
| 13-17 | 0x004E541E..5455 | more HUD; `FUN_0044B6A0` uploads the composited 2D pixelmaps as textures (`FUN_00523160` x2 + `BrMaterialUpdate`) then queues 20+ HUD actors (`HudQueueActor` 0x004E5AD0) | `grTexDownloadMipMap`, `grDrawTriangle` |
| 18 | 0x004E546F/5486 | replay overlays (`[0x00676914]!=0`) | -- |
| 19 | **0x004E548B** | **`HudFlush` 0x004E5B00**: adds glyph root `[0x0074CF10]` + queued HUD actors `[0x00704E60]` to HUD root `[0x0074CA00]`, then `BrZbSceneAddActorIncremental(world=[0x0074CA00], camera=[0x0074CF74], colour=[0x00762128], depth)` | `grDrawTriangle` |
| 20 | 0x004E54A0/5499 | frame limiter | -- |
| 21 | **0x004E54D1** | `FUN_0051C520` -> `_doubleBuffer(screen, back)` -- **the buffer swap** | `grBufferNumPending` x2, `grBufferSwap` |
| 22 | 0x004E54DF | replay-only tail | -- |

### 27.3 `RenderView` = FUN_004E54F0 and `RenderScene` = FUN_004E5680

```c
if (view == 0)                                       // reflections first, into the 64x64 texture
  for (i = 0; i < [0x006A22C0]; i++) {               // queue filled by FUN_004E5CC0, reset by FUN_004E5CB0
     RenderScene([0x006A22BC], [0x0068B8A4], 1.0f, 0, 0, 0);   // a full scene, own Begin/End
     BrMaterialUpdate(entry.material, BR_MATU_COLOUR_MAP);
  }
RenderScene(colour, depth, 1.0f, view==0, view==0, view==0); // the view itself
```

`RenderScene` (EDX = camera, ECX = owning car, `ret 0x18`):

1. `camera->yon_z *= yon_scale`.
2. **depth clear**: `_fill(depth, 0xFFFFFFFF)` -> `grBufferClear`.
3. *(non-default mode only, `[0x0074D3DC]==0 && [0x0068B918]!=0 && [0x0074B784+0x74] in {6,7}`)* car bodies drawn in their own Begin/Add/End with a tint poly between -- **[?]** mode unidentified, does not run in a plain race.
4. Colour clear **or** sky, never both (0x004E5882): with `g_fogShadeTable` (`[0x0075D778]`) zero -> `_fill(target, colour)`; otherwise **`BrZbSceneRenderBegin` @ 0x004E5919, `DrawHorizon` (FUN_00445CB0) @ 0x004E592B, `BrZbSceneRenderEnd` @ 0x004E5930** -- **the horizon is its own one-model scene, immediately before the race scene, and no colour clear happens.** `DrawHorizon` rotates the horizon actor to the camera yaw, pins it to the camera position, writes a scrolling `map_transform` into HORIZON.MAT (`m[2][0] = -(yaw / fov)`, 0x00445D76..), `BrMaterialUpdate(mat, 0x7FFF)`, sets `render_style = FACES`, `BrZbSceneRenderAdd`, restores `render_style = NONE`.
5. depth-cue bookkeeping (`FUN_00446340`), shadow/skid geometry (`FUN_004E74D0`, actors not draws).
6. **`BrZbSceneRenderBegin([0x0074D44C], camera, colour, depth)` @ 0x004E5961** -- the race scene: four backdrop actors `[0x0074D650]`, `[0x0074D64C]`, `[0x0074D644]` each under a screen-space depth bias from `FUN_00540560(n)` (table `{0, -1.5, -3, ...}` at 0x00670530, written to `[0x0079FEB4]`); `FUN_00506E50`; **`BrZbSceneRenderAdd([0x007634B8])` = the track/world content** @ 0x004E59BE; the "Limbs_actor" pool (`FUN_004D3610`); particles (`FUN_004F7450`, `FUN_004FA910`, `FUN_004D5D60`) when `do_particles`; **`BrZbSceneRenderEnd` @ 0x004E5A1B** -- the bucket sort and rasterization, where nearly all `grDrawTriangle` traffic originates.

### 27.4 How the 2D overlay reaches the card

**[C] Almost the entire HUD is textured triangles, not LFB writes.**

* Text: `HudDrawText3D` FUN_00464E40 takes glyph actors from the pool `[0x0074CAE0]`, sets model/material, `BrActorAdd`s to `[0x0074CF10]`, and (when flushed) forces the colour pixelmap's base/origin to 0 and size to 640x480, then `BrZbSceneAddActorIncremental(world=[0x0074CA00], camera=[0x0074CF74], colour, depth)`.
* **`BrZbSceneAddActorIncremental` @ 0x005226D0 is not an add**: it is a complete miniature scene render -- publishes the colour/depth pixelmaps, computes screen scale/offset, `SceneSetupCameraMatrices`, walks the world's children through `BrZbActorRender` (so through the BrZbModelRender hook), then `renderer->flush`. Four args `(world, camera, colour, depth)`. It never calls `BrZbSceneRenderBegin`, so the proxy's capture flag is off while it runs.
* HUD widgets: `HudQueueActor` (max 128, "Not enough HUD actor storage") and `HudFlush` render the queue with one `BrZbSceneAddActorIncremental` into the race view pixelmap.
* Dashboard, cockpit and map are composited CPU-side into memory pixelmaps (`FUN_0047BA80` 16-bit blitter, `BrPixelmapLine`), uploaded as textures by `FUN_00523160`, drawn as quads.

The only per-frame 2D ops touching the Glide framebuffer are the letterbox fills (start of frame) and the `_fill` clears. `BrPixelmapText`/`BrPixelmapLine` are stubs on this device. **`grLfbWriteRegion` should not appear in a normal race frame at all.**

### 27.5 What this means for the proxy

* The race view is the scene opened on colour pixelmap `[0x00762128]`. The horizon scene, the mirror view (`[0x0068B8A8]`) and the reflection passes (`[0x006A22BC]`) all come through the same Begin/End hooks; the proxy currently tells them apart by model count and race camera, and now logs each distinct (camera, colour target) pair once (`scene target:` line) so a run shows whether a second camera ever reaches the submit.
* Nothing at the Glide layer separates 3D from 2D: no idle, no flush, same triangle path. The BRender hooks are the only clean boundary, which is why the injection point is pinned from the proxy (section 26.2) rather than inferred from the draw stream.
* The horizon actor is captured in its own scene (one model, never submitted) and, with `SuppressDynamics`, is no longer rasterized by the game. That raster was pre-injection and overwritten by Remix's blit in any case; the sky Remix shows comes from its own sky handling.
* kb.h corrected: the `.bdd` drivers are loadable (by BRender's loader); `BrZbSceneAddActorIncremental` takes four arguments; `0x0074D360` is the back buffer, not a "BANG!" pixelmap.

---

## 28. Glass: what carries a surface's history in Remix (2026-09-05)

Source read of dxvk-remix (dxvk-remix). This section records the
mechanism; the proxy-side change and the verdict are in the section that follows it.

### 28.1 A draw keeps its temporal history through DrawCallTracker

`DrawCallTracker::findOrCreateReplacementInstance` (rtx_draw_call_tracker.cpp:170-270)
matches this frame's draw against a previous frame's `ReplacementInstance` in three levels.
The key is built at :271:

```cpp
const ReplacementInstance::LookupKey key {
  computeIdentityHash(drawCallState, overrideMaterialData),   // L1
  hashes.getHashForRule<rules::TopologicalHash>(),            // spatialMapHash: the L2 bucket
  drawCallState.getMaterialData().getHash(),                  // materialHash
  hashes[HashComponents::VertexPosition],
  drawCallState.getGeometryData().boundingBox.getTransformedCentroid(objectToWorld),
  objectToWorld, ...
};
```

* **L1** -- exact `identityHash`, which covers the transform, the material and the vertex
  hashes. A surface that has not moved and has not changed matches here every frame.
* **L2** -- within the topological-hash bucket: first an exact transform + vertex-position
  match, then a **nearest-neighbour search bounded by `rtx.uniqueObjectDistance`**, filtered
  to candidates that were not already matched this frame and whose `materialHash` is equal.
* **L3** -- no match: a new instance, with no history at all.

`LegacyMaterialData::computeIdentityHash` (rtx_materials.cpp) covers the colour texture
hashes, the sampler hashes, the alpha test op and reference, **`tFactor`**, the whole blend
mode, and the texture stage colour/alpha argument sources and operations. So anything the
injection changes per draw -- the texture factor carrying material opacity, the texture
itself -- is part of the identity, and changing it breaks the L2 filter as well.

### 28.2 Static geometry matches at L1; anything that moves does not

This is the asymmetry behind "the water is fine and the car windows are not". The water is
a sealed chunk: identity transform, immutable buffers, unchanged material, so it matches at
L1 on every frame and its reflection accumulates cleanly. A car moves, so its glass fails
L1 by construction and is re-associated through the L2 nearest-neighbour search every
frame. Every frame that search misses is a frame the surface starts from nothing.

`rtx.uniqueObjectDistance` defaults to **300** game units and is not set in this port's
`rtx.conf`. The whole visible world here is 250 units (`[Culling] FarPlane`), track pieces
sit ~60 units apart and a car is a few units long, so the search radius spans the entire
level: every instance sharing a topology and a material is a matching candidate for every
other one. Repeated parts -- four wheels off one mesh, two opponents in the same car --
can take each other's history.

### 28.3 Blended draws are forced double-sided

`rtx_instance_manager.cpp:91`:

```cpp
if (drawCall.getMaterialData().blendMode.enableBlending && !surface.alphaState.isDecal
    && !drawCall.getGeometryData().forceCullBit)
  flags |= VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
```

so for any alpha-blended draw the game's cull mode is discarded and the geometry goes into
the BLAS double-sided. `forceCullBit` is only ever set from a USD replacement
(rtx_mod_usd.cpp:1576). The injection submits every draw with `D3DCULL_NONE` anyway, so
the whole world is currently double-sided regardless.

### 28.4 A translucent replacement bypasses the decal and particle classification

`InstanceManager::calculateAlphaState` (rtx_instance_manager.cpp:658) returns immediately
for `MaterialDataType::Translucent`, leaving `isParticle`, `isDecal` and `emissiveBlend`
false and `isBlendingDisabled` true. The instance then falls through the mask ladder at
:1205-1250 to `OBJECT_MASK_TRANSLUCENT` in the primary TLAS. So glass with a translucent
replacement is *not* at risk of being shunted into the unordered TLAS, whatever the game's
blend state or the decal texture tags say -- that hypothesis is dead.

### 28.5 What the mod authors

`rtxmod/mod.usda` binds `AperturePBR_Translucent` to three materials: one thick
(`mat_207656C26F8A30D3`, ior 1.01, a subsurface transmittance texture, measurement
distance 0.88) and two thin-walled (`mat_A5D9906FE099EB7F` ior 1.02, `mat_A625862AD1FEF1CD`)
-- the car glass. Neither of the thin-walled ones sets `doubleSided`, so nothing on the mod
side restores culling either.

---

## 29. PSR does not know the mirror moved, and the glass was double-sided (2026-09-05)

Source read of dxvk-remix (dxvk-remix), following section 28.

### 29.1 The root cause: virtual motion vectors ignore the reflector's own motion

A translucent primary surface is replaced by what is seen in or through it -- Primary
Surface Replacement -- and the denoiser then works on that *virtual* surface. Its
reprojection is built in `geometry_resolver.slangh:264-266` and `:311-338`:

```
virtualHitPosition = camera ray evaluated at the accumulated hit distance
virtualMotion      = quaternionTransformVector(accumulatedRotation, surfaceInteraction.motion)
prevWorldPosition  = virtualHitPosition + virtualMotion
```

`accumulatedRotation` is composed only of this frame's reflection and refraction
quaternions (`:117` identity, `:2214`, `:2822`, and `getReflectionQuaternion` in
`translucent_surface_material_interaction.slangh:868`), and `surfaceInteraction.motion` is
the motion of the surface being *reflected*. **No term anywhere accounts for the reflector
itself having moved or rotated.** The model is a virtual image in a static mirror.

That is precisely the difference between the two surfaces this port has:

* **Water** -- a static reflector. The virtual motion is correct, history reprojects, the
  reflection converges. It also matches at L1 every frame (section 28.2), so nothing
  disturbs it. This is why it always looked right.
* **A car window** -- the reflector translates and rotates every frame. The virtual image
  should sweep across the screen as the car turns; Remix reports that it barely moved, so
  the denoiser fetches history from the wrong pixels and keeps it. That is "the
  reflections paint rather than reflect", and the retained wrong history is the white
  noisy residue that builds up and smears when the camera turns.

Two details make it worse. DLSS-RR's motion-vector fix-up covers *transmission* PSR only
(`geometry_resolver.slangh:2874`), so reflection PSR gets the uncorrected virtual vector.
And `rtx.psrrNormalDetailThreshold` defaults to 0, which flags every glass pixel for NRD's
relaxed disocclusion threshold (0.1 instead of 0.01, `rtx_nrd_settings.cpp:206-210`) --
ten times more willing to keep history that is wrong. That knob is inert while DLSS-RR is
on, which it is by default.

**Nothing in the proxy can fix this.** What the proxy can do is stop feeding it the two
conditions that make it far worse.

### 29.2 The glass was double-sided, twice over

`determineInstanceFlags` (`rtx_instance_manager.cpp:66-119`) has two independent paths to
`VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR`, and the injection tripped
both: `D3DCULL_NONE` on every draw (`:101-104`) and alpha blending on the translucent pass
(`:91-92`). That bit **overrides `RAY_FLAG_CULL_BACK_FACING_TRIANGLES`**, which the
translucent path relies on in all three of its trace sites -- each carrying a comment that
back faces are only to be counted when the ray is inside a medium
(`geometry_resolver.slangh:3077`, `:3016`, `integrator_indirect.slangh:1449`).

With the invariant void, every wall of a car's glass shell is hit from both sides. The
shading normal is unconditionally flipped toward the viewer first
(`surface_interaction.slangh:360-362`), so a back-face hit is indistinguishable from a
front-face hit and each crossing reads as a fresh entry into the medium. Each one burns a
PSR bounce out of `psrrMaxBounces`, and at grazing angles the thin-walled geometric series
`1 / (1 - insideFresnel^2)` (`brdf.slangh:536-539`), guarded only by an exact float
equality against 1.0, reaches thousands. That is the white speckle.

The Glide driver never culled either -- it calls `grCullMode(GR_CULL_DISABLE)` once at
init -- so all backface rejection in this game is BRender's software T&L, which the
injection bypasses entirely. Remix was being handed both faces of every surface in the
game, not just the glass.

### 29.3 What the proxy does now

* **`[Effects] BackfaceCulling`** -- submits a real cull mode. Which screen-space winding
  is a back face is measured, not assumed: `sample_winding` compares the normal implied by
  the order indices are emitted in against the authored vertex normals over the first 4096
  triangles, and `resolve_cull_mode` names the mode from the majority. The projection is
  right-handed with the camera down -Z, so a front face comes out counter-clockwise in NDC
  and clockwise on screen after the viewport's Y flip; culling CCW is therefore correct
  when the emitted order is the outward one, and the measurement says when it is not. The
  choice is logged once either way, even when the switch is off. Materials flagged
  `BR_MATF_ALWAYS_VISIBLE` / `BR_MATF_TWO_SIDED` and the spark billboards stay
  double-sided.
* **`[Effects] SolidTranslucency`** -- solid translucent surfaces are submitted as
  ordinary geometry with an alpha test instead of as blended draws. Both switches are
  needed: either alone still leaves the geometry double-sided. Sprites, decals, and
  anything the game fades through its opacity byte keep their blending -- nothing replaces
  those, and an alpha test cannot express a uniform fade. A replaced translucent material
  owns how much light passes through the surface, so the blending buys glass nothing.

The alternative was mod-side: `forceCullBit` is set only by a USD **mesh** replacement
authoring `doubleSided` (`rtx_mod_usd.cpp:1571-1579`, and `usd_mesh_importer.cpp:169-174`
requires the value to be authored, not defaulted). A material-only replacement cannot do
it, and a mesh replacement per car is not practical here.

### 29.4 Ruled out

* **The unordered TLAS.** `calculateAlphaState` returns early for a translucent material
  (`rtx_instance_manager.cpp:658`), so `isDecal` / `isParticle` / `emissiveBlend` are all
  false and the instance takes `OBJECT_MASK_TRANSLUCENT` in the primary TLAS. Decal and
  particle texture tags cannot divert replaced glass.
* **The Neural Radiance Cache.** It is the default indirect integrator and trains on
  unclamped radiance, which would fit "junk that grows over time" -- but this machine's
  run log carries `Neural Radiance Cache failed to get initialized. Switching to
  importance sampled indirect illumination mode`, so it is not running here.
* **Vertex colour, texture factor opacity, alpha test state.** All ignored for a
  translucent material (`translucent_surface_material_interaction.slangh:46-190`).

### 29.5 Residual mitigations, in the order worth trying

These address the section 29.1 limitation, which no code change removes:

1. `rtx.fireflyFilteringLuminanceThreshold = 30` (default 1000) -- the global luminance
   clamp in `sanitizeRadianceHitDistance`, the most direct suppressor of white speckle.
2. `rtx.secondarySpecularFireflyFilteringThreshold = 50` (default 1000) -- clamps the
   non-selected PSR surface specifically.
3. `rtx.psrrMaxBounces = 2` (default 10) -- limits how far virtual reprojection error
   compounds.
4. Diagnostics that attribute the residual rather than fix it: `rtx.enablePSRR = False`
   (if the speckle goes and reflections turn blurry, 29.1 is confirmed),
   `rtx.useDenoiser = False` (separates accumulation from the raw signal), and
   `rtx.uniqueObjectDistance` tuned to this game's scale -- the default 300 units is wider
   than the entire visible world (section 28.2).

---

## 30. BRender's facing rule, confirmed from the rasterizer (2026-09-05)

Section 29 shipped a cull mode on a measurement. The engine's own rule is now read out of
the binary, and it agrees.

### 30.1 There is exactly one facing test, and the hardware path does not skip it

`ModelRenderStyle_Faces` (0x00525FC0) dispatches through `g_pGeometryV1Model`'s vtable
(0x0058BE98, `+0x44` render / `+0x4C` renderOnScreen) into **0x00542960**, which builds a
per-group pipeline of stage function pointers and runs it. The `.bdd` device drivers export
only `BrDrv1Begin` and register a *primitive* library ("3Dfx-Primitives"), not a geometry
object: the geometry always comes from the EXE's own "Default-Renderer-Float", so the
Glide and software paths run the same cull. The Glide driver itself calls
`grCullMode(GR_CULL_DISABLE)` once at init and never culls.

The cull stage is chosen at **0x00542BAD** on `renderer + 0x18`, which `BrMaterialUpdate`
publishes as `BRP_CULL` (token 0x74) straight from the material flags:

```c
uVar4 = 0xad;                                                /* BRT_ONE_SIDED */
if ((material->flags & 0x800)  != 0) uVar4 = 1;              /* BRT_NONE      */
if ((material->flags & 0x1000) != 0) uVar4 = 0xae;           /* BRT_TWO_SIDED */
partSet(renderer, 0x74, 0, 0xac, uVar4);
```

* `0x0800` = **BR_MATF_ALWAYS_VISIBLE** -> `BRT_NONE` -> 0x00543110 marks every face
  visible and runs no test at all.
* `0x1000` = **BR_MATF_TWO_SIDED** -> `BRT_TWO_SIDED` -> 0x005435A0 runs the same test but
  never culls; it records front/back as flag 4/5 and a sign used to flip the normal for
  lighting. It wins over 0x0800.
* Neither -> `BRT_ONE_SIDED` -> 0x005432B0, the cull.

So both bits mean "do not cull", which is exactly the mask `material_is_two_sided` uses.
`0x2000` is `FORCE_FRONT_B` and is lighting only.

### 30.2 The test is a model-space plane test

0x005432DB..0x0054330C, per face, over the prepared "online" faces (`v1_group + 0x04`,
stride 0x1C, normal at +0x0C, `d` at +0x18):

```
keep  <=>  dot(face->n, eye_in_model_space) >= face->d        (fcomp at 0x00543304)
```

`d = dot(n, v0)`, so this is `dot(n, eye - v0) >= 0` -- keep when the eye is on the
normal's side; equality is kept. The eye is the view-space origin pushed back into model
space by `ComputeCullEye` (0x00543A80) into 0x0079FAF4. The parallel-camera variant
(0x00543380) compares `dot(n, viewdir)` against 0. There is no screen-space area test
anywhere, and the primitive emitters downstream contain no second cull.

The planes are **rebuilt** for the online faces by `BuildOnlineFacePlanes` (0x0051F6A0,
called from BrModelUpdate at 0x0051FCC5) from the pivot-relative online vertices, through
`BrPlaneEquation` (0x00536FB0):

```
n = normalize((v1 - v0) x (v2 - v0))
d = +dot(n, v0)
```

### 30.3 Which D3D cull mode that makes

`n = (v1 - v0) x (v2 - v0)` pointing at the eye means a front face is wound
counter-clockwise **as seen from the eye**, in right-handed model space.

D3D9 assumes the opposite. Its own projection is left-handed, under which a front face is
clockwise from the eye, and `D3DCULL_CCW` -- the API default -- is the mode that keeps
those. The injection hands Remix a **right-handed** projection
(`D3DXMatrixPerspectiveFovRH`, camera down -Z), which puts BRender's counter-clockwise
front faces on exactly the side `D3DCULL_CCW` throws away.

**`D3DCULL_CW` culls back faces here** -- the standard consequence of driving a
right-handed projection through an API built around a left-handed one. The measurement is
kept as the check on the one step the engine does not settle -- that the injection emits
indices in the order BRender took its normal from -- and it logs its verdict either way.

This was got wrong once, and the symptom is worth recording because it is unmistakable:
with the mode inverted every front face is culled, only the far interior walls of objects
survive, and the whole world reads as transparent. `dxvk`'s `frontFace =
VK_FRONT_FACE_CLOCKWISE` (d3d9_rtx.cpp:637) and `D3DCULL_CCW -> VK_CULL_MODE_BACK_BIT`
(d3d9_util.cpp:271-277) describe D3D9's convention faithfully; they say nothing about which
side a right-handed projection puts this game's geometry on, and using them as if they did
is what produced the error.

### 30.4 A note for anything that hooks lower

The cull runs *inside* 0x00542960, downstream of `ModelRenderStyle_Faces`. A hook at the
render style, which is where this injection taps, therefore sees complete unculled
model-space geometry -- which is why the facing has to be reconstructed here at all. A
hook at the primitive emitters would see post-cull screen-space data instead.

---

## 31. Roughness is the way off the PSR path (2026-09-05)

The in-game symptom confirms section 29.1 exactly: the streaks smeared across a car's
windscreen are the car's own cream bodywork. That reflection is rigidly attached to the
car and should be motionless relative to a chase camera, but PSR reprojects it with the
reflected surface's **world** motion and no term for the mirror moving, so it drags a
little further every frame and never converges. Hence "after a while of driving".

### 31.1 A translucent material can never escape PSR

`translucentSurfaceMaterialInteractionGetLobeInformation`
(`translucent_surface_material_interaction.slangh:294-309`) hardcodes
`specularReflectionPresent` and `specularReflectionDirac` to true. Every translucent
surface is a perfect mirror to the resolver, so reflection PSR is always eligible and
there is no material parameter that opts out -- the translucent material has no roughness
input at all.

### 31.2 An opaque material with any roughness is refused PSR

`opaqueSurfaceMaterialInteractionCalcPSRReflectionSample`
(`opaque_surface_material_interaction.slangh:1619-1631`) refuses PSR outright when a
diffuse lobe is present or the specular lobe is not Dirac:

```cpp
if (lobeInformation.diffuseReflectionPresent ||
    !lobeInformation.specularReflectionPresent ||
    !lobeInformation.specularReflectionDirac || ...)
{ materialPSRSample.performPSR = false; return materialPSRSample; }
```

and Dirac is `isotropicRoughness < 0.001f` (`:1034-1035`, threshold at `:34`). So an
opaque material with even slight roughness is **not** replaced: its reflection is computed
on the glass surface itself and denoised against that surface's own motion vectors, which
are correct for a moving car.

**For car glass that is the trade worth making.** Binding `AperturePBR_Opacity` with a
small roughness (~0.05-0.1), low opacity and zero metallic instead of
`AperturePBR_Translucent` gives up true refraction -- worth very little through a thin
pane -- and gets back reflections that track the world. The water should stay
`AperturePBR_Translucent`: it is a static reflector, PSR reprojects it correctly, and it
is the one surface here that genuinely benefits from refraction.

### 31.3 Applied meanwhile

`rtx.conf` now carries the section 29.5 clamps: `fireflyFilteringLuminanceThreshold = 30`,
`secondarySpecularFireflyFilteringThreshold = 50`, `psrrMaxBounces = 2`. They reduce how
bright the smear gets; they do not stop it. `rtx.enablePSRR = False` is the global version
of 31.2 -- it takes *every* mirror off the PSR path, water included, so it is the decisive
A/B rather than the setting to keep.

### 31.4 The mod change, as applied (2026-09-05)

`rtxmod/mod.usda` (backup `mod.usda.pre-opacity-bak`). The two thin-walled glass materials,
`mat_A5D9906FE099EB7F` and `mat_A625862AD1FEF1CD`, now reference
`AperturePBR_Opacity` instead of `AperturePBR_Translucent`:

```
custom bool  inputs:use_legacy_alpha_state = 0
custom bool  inputs:blend_enabled = 1
custom float inputs:opacity_constant = 0.18
custom float inputs:reflection_roughness_constant = 0.07
custom float inputs:metallic_constant = 0
```

`reflection_roughness_constant` only has to clear 0.001 to escape PSR; 0.07 is a plausible
car window and leaves room to tune. `blend_enabled` has to be set explicitly because the
opaque material defaults it to false, and `use_legacy_alpha_state = 0` is required now that
`[Effects] SolidTranslucency` submits these draws unblended -- the transparency has to come
from the material. That same unblended draw is what keeps the runtime from forcing the
geometry double-sided, so the two changes depend on each other.

Traced through `calculateAlphaState` and the mask ladder, such an instance lands on
"alpha-blended geometry goes to the primary TLAS as non-opaque geometry with no duplicate
hits" (rtx_instance_manager.cpp:1228-1232) -- properly ray traced, single-sided, and off
the PSR path.

`mat_207656C26F8A30D3` is left translucent: it is thick, carries a subsurface transmittance
texture and a measurement distance, and is the water.

### 31.5 Tried and reverted: the opacity material looks worse (2026-09-05)

The 31.4 swap was tested in game and rejected on looks -- reverted to
`AperturePBR_Translucent` with `thin_walled = 1`. So the trade is not worth taking as
stated: escaping PSR costs more in appearance than the smearing costs, at least at
`opacity_constant = 0.18` / `reflection_roughness_constant = 0.07`.

Worth noting what this does **not** rule out. The swap changed three things at once --
material model, opacity source, and roughness -- so "looks bad" does not say which. A
narrower experiment would keep the translucent material and attack the smear from the
runtime side instead (`rtx.enablePSRR = False` is the whole-scene version of the same
idea), or keep the opacity material and tune it, since 0.18 opacity with a 0.2 default
albedo is a fairly milky starting point for glass.

### 31.6 The clamps did not help either (2026-09-05)

`rtx.fireflyFilteringLuminanceThreshold = 30`,
`rtx.secondarySpecularFireflyFilteringThreshold = 50` and `rtx.psrrMaxBounces = 2` were
tested and made no visible difference; `rtx.conf` is back to its previous 30 lines.

That is itself informative. Clamping bright outliers and shortening the PSR chain both
attack *how bright* the artefact is, and neither touched it -- which points away from
fireflies and back at the reprojection itself: the smear is history being dragged to the
wrong pixels, not a few over-bright samples being accumulated. The untried lever that
addresses that directly is `rtx.enablePSRR = False`, which stops the reflection being
replaced at all.

---

## 32. Emissive: the engine has none, but it has two exact stand-ins (2026-09-06)

BRender has **no emissive, self-illumination or glow channel**. Its struct reflection table
for `br_material` (17 entries at 0x006637F0, struct size 0x9C) lists exactly: `identifier`,
`colour`, `opacity`, `ka`, `kd`, `ks`, `power`, `flags`, `map_transform.m[0..2]`,
`index_base`, `index_range`, `fog_min`, `fog_max`, `fog_colour`, `subdivide_tolerance`.
The SURFACE-part publish in `BrMaterialUpdate` (0x005211D7) emits only `COLOUR_RGB`,
`OPACITY_F`, `AMBIENT_F`, `DIFFUSE_F`, `SPECULAR_F`, `SPECULAR_POWER_F`, `LIGHTING_B`,
`FORCE_FRONT_B`, `COLOUR_SOURCE_T`, `MAPPING_SOURCE_T`. The 461-record token table
(0x00667F50, stride 0x18) contains no EMISSIVE / GLOW / LUMINANCE / SELF_* token.

This also resolves the last unmapped bytes of the struct, from BrMaterialUpdate rather than
the table (which only covers serialized fields): **+0x50 `index_fog`** (`BRT_INDEX_FOG_O`
0x18A), **+0x54 `extra_surf`** (SURFACE part), **+0x58 `extra_prim`** (PRIMITIVE part --
what earlier sections call `extra`), **+0x68 `mode`** (texture wrap/mirror/clamp and
antialias bits), +0x98 `stored`.

### 32.1 The two signals that are exact

**Flags exactly `BR_MATF_ALWAYS_VISIBLE` (0x0800).** *(Wrong at runtime: see 34.5. A preset call right after this sets ka 1.0 / kd 0 and turns LIGHT back on.)* `InitSpriteParticlePool` (0x004EAA4E,
0x004EAA5F) and `InitFlames` (0x004FC4A5, 0x004FC4B2) both clear LIGHT and set 0x800 on a
material `BrMaterialAllocate` had defaulted to flags 1, so the result is 0x0800 exactly:
no LIGHT, no PRELIT. BrMaterialUpdate then publishes `LIGHTING_B = 0` with the colour taken
from the surface (white), i.e. a full-bright unshaded texture. That is explosion fire
(`ex00001..7`), the powerup sparkle (`TWINK1..4`, `BING1..6`), blood (`BIGBL01..05`), the
`BANG!` marks and the car flames (`FLM01..FLM20`).

**`ka >= 1.0`.** `ka` is the ambient coefficient, and at 1.0 the surface renders at full
texture brightness whatever the scene light does -- the engine's stand-in for
self-illumination. Of the 4373 material definitions the game ships, 4282 are at BRender's
0.1 default, 60 at 0.2, 12 at 0.0, and exactly **19 at 1.0**: `lamp2` (Airport1); `TUNLI`,
`tunli2`, `TUNCEL1`, `TUNFLR1`, `ROKMER`, `ROKSHAD`, `SHAD`, `FENCE` (desert1); `light1`,
`cinema01`, `ceiling1`, `vaultconsole`, `vaultwall2`, `room2`, `slab2`, `slab3`, `road6`,
`road7` (newcity1). Nothing writes `ka` at runtime.

Both are now reported by the injection as `unshaded surface: material '...' texture '...'`,
once per material, naming the texture to tag.

### 32.2 What is only recognisable by name

Brake and reverse lights are **byte-identical to ordinary body panels** on disk -- `EARLITL`
/ `EARLITR` ship as flags 0x0001, ka 0.1, colour 0xFFFFFF, differing only in `colour_map`
(`ebacklig`) and in being funked. The lit state is a texture region and nothing else: the
2x2 `EBACKALL` atlas quadrant chosen by `map_transform` (section 6.4). Headlights
(`EALITL` / `EALITR`, `eheadlig`) are not funked at all. Track neon, signs, TV screens and
lamps carry no signal whatsoever -- every one is ka 0.1 with flags 0x0001 or 0x0021.
For all of these, a pixelmap-name allowlist is the only route.

### 32.3 PRELIT is "textured", not "glowing"

**Zero of the 4373 shipped materials have PRELIT set on disk.** It is forced on at load by
`LoadCarMaterials` (0x00450150), whose only condition is having a texture:

```
0x0045020D  mov ebp, 3                  ; LIGHT|PRELIT
0x004502D9  if (material->colour_map != NULL) {
0x004502E8      material->flags |= ebp;  material->flags |= 4 /*SMOOTH*/;
0x00450307      BrMaterialUpdate(material, 0x7FFF); }
```

which is why `scrn`, `hawingy`, `es2wgblu` and every other car body panel logs as prelit.
`g_texture_detail_mode` (0x00591374) does the same wholesale across both material stores.
So PRELIT cannot distinguish a glowing surface from a baked-lit one and must never be used
as an emissive gate.

### 32.4 The injection cannot make a draw emissive

Remix ignores the per-draw `D3DMATERIAL9`: `LegacyMaterialData::createDefault` takes
emission from the global `rtx.legacyMaterial.emissiveIntensity` / `emissiveColorConstant` /
`enableEmission` options (rtx_materials.cpp:133-142), and the only field of the stored
`D3DMATERIAL9` read anywhere is `Diffuse` (rtx_scene_manager.cpp:864). Emission therefore
has to come from a replacement keyed on the texture hash, or from `rtx.lightConverter`
(rtx_options.h:240), which turns a tagged surface into an actual light rather than a bright
texture -- the better fit for head and brake lights, which should illuminate the road.

## 33. Headlights: Remix lights on the car's master actor (2026-09-21)

Dark stretches path-trace to black because the sky is the port's only light. The proxy
now puts two Remix spot lights on each car. H cycles off -> player car -> all cars -> off;
the F4 menu's Headlights tab has every value on a slider and saves them to
`carma2-headlights.ini` beside the game EXE.

### 33.1 Where the cars are

All from disassembly of `CARMA2_HW0.EXE`; the offsets are in `kb.h`.

- The player's `tCar_spec` is a global struct at `0x0075BC2C`, not a pointer.
  `GetCarSpec(0, 0)` returns the constant (`0x004AE7EC`), and `BuildCarShadows` loads it
  directly (`0x004E752A`).
- Opponents: `tOpponent_spec[30]` at `0x0075D8A0`, stride `0x1A4`, `tCar_spec*` at `+0x08`,
  count at `0x0075D7A0`. Cops: same layout at `0x007609D8`, count at `0x00691744`. The
  lookup loops at `0x004A9CEC` and `0x004A9D27` prove base, stride and offset.
- `car + 0x10` is the master actor; its matrix is car-to-world. `car + 0xE0C` is the loaded
  `.ACT`, added under the master with an identity transform (`0x0048A342`).
- A car faces down local -Z with +Y up: `0x0041410C` writes `-(row 2)` into
  `car->direction`. The car data agrees -- `copcar`'s front bumper sits at z = -0.53.
- `car + 0x1D4` is set by `KnackerThisCar` (`0x0043F5F0`): the car is wasted.
- `0x0075BBA8` is `gProgram_state.racing`: 1 inside `MainGameLoop`, 0 in the pause frontend.
- The proxy reads the arrays itself instead of calling `GetCarCount` / `GetCarSpec`; net
  players (category 1) are not read.

### 33.2 Scale

A normal car is about 0.4 wide, 0.8-1.0 long and 0.25-0.3 high in BRender units; `bigdump`
is 1.2 wide and its shell reaches z = 1.17. The constant 6.9 (Carmageddon 1's WORLD_SCALE)
appears in `.rdata`, so one unit is about 6.9 m. No fixed mounting point fits both, which
is why a lamp is placed relative to the box around everything the car's actor tree draws:
a fraction of the half width, a fraction of the height, and a fixed distance ahead of the
front face so the body does not shadow its own lamp. The box is re-measured every 120
frames, because damage reshapes the models.

### 33.3 The Remix side

- The lights are sphere lights with cone shaping, created through the Remix API from the
  race-view submit, where world space is BRender's own world space.
- The API has no move call. Describing a light again under the same hash overwrites it;
  destroying it first would blink it out for a frame.
- A light is in the scene only in frames where `DrawLightInstance` is called for it.
- The menu's brightness is radiance times emitter area, so the emitter radius changes how
  soft the shadows are and nothing else.
- **The Remix API was not initializing before this** (`Failed to initialize the remixApi -
  Code: 11`, NOT_INITIALIZED): the game's `.trex/bridge.conf` lacked `exposeRemixApi = True`.
  That also means section 24's `SetConfigVariable` pushes never reached Remix on that
  install. The headlights module retries the initialization a few times from the race
  frame, because the bridge is not necessarily up when the proxy first asks.

### 33.4 The API headers have to be the runtime's own

With the API exposed, the game died on boot: `FAST_FAIL_STACK_COOKIE_CHECK_FAILURE` in
`remix_api::initialize`. The vendored headers were API 0.5.2, whose `remixapi_Interface` has
21 entries. The deployed bridge client (`d3d9_remix.dll`, `remixapi_InitializeLibrary` at
RVA 0x5E470) zeroes 0xA4 bytes and copies 0x29 dwords into the caller's struct without
asking its size -- 41 entries, straight over the stack cookie.

A bigger buffer would not have been a fix. Entries are inserted mid-table between versions
(`CreateMeshBatched` at 4, `CreateLightBatched` at 9, four texture calls at 13), so under
the old header every call past `CreateMesh` would have landed on the wrong function. The
slots the client fills -- 1, 2, 3, 5, 7, 8, 10, 11, 12, 17, 18, 30, 31, 33, 34, 36, 40 --
match the 0.1000.0 header of the runtime's own source tree exactly, including its
`GetUIState` / `SetUIState` pair at 30 and 31. `deps/bridge_api` now holds that header set,
and `initialize` carries a `static_assert` on the table size. Replace the headers only
together with the runtime. `PFN_remixapi_BridgeCallback` became `__stdcall` in that version.

### 33.5 The API may not be initialized before the device exists

With matching headers the initialization succeeded -- and the Remix server then closed the
game ("The RTX Remix Runtime has encountered an unexpected issue"). `bridge32.log` names
it: the client's device queue held four `RemixApi_CreateMaterial` commands ahead of the
handshake's `Syn`, and `bridge64.log` ends on `Timeout. Application failed to give
go-ahead (CONTINUE)`. `comp::main` initialized the API from the proxy's start-up thread,
before the game had created a device, and `remix_api::initialize` immediately created the
framework's four debug-line materials. API calls ride the device queue, so they arrived
where the server expected the handshake. The stale headers had hidden this for the whole
life of the port: initialization always failed, so nothing was ever sent.

The API is now initialized by `remix_api::ensure_initialized` from the first race-view
submit -- render thread, device live -- with a few spaced retries, and the debug-line
materials are created on first use instead of at initialization.

Verified on 2026-09-21: the game boots, `Initialized RemixApi` is logged on the first race
frame, H lights the player car and then every car, and nothing faults.

### 33.6 The overlay has to be drawn into the back buffer (2026-09-21)

The F4 menu was only visible on the black frames between menu screens. A two-frame trace
shows why: nGlide never renders to the back buffer. It sets an offscreen target, draws the
whole frame into it, calls `EndScene`, then `StretchRect`s that target onto the back buffer
and presents. The framework drew ImGui from the `EndScene` hook, so it went into the
offscreen target -- the same image Remix writes the path-traced frame into
(`RtxContext::injectRTX` targets the bound render target) -- and never reached the screen
over a live frame.

`D3D9Device::draw_overlay` now draws ImGui from the `Present` hook instead: back buffer
bound, a scene of its own, after the copy. Verified over the intro video, the frontend and
a path-traced race.

### 33.7 Lights that go missing (2026-09-21)

Reported: the player's lights sometimes disappear, possibly when going uphill. Not
reproduced -- synthetic input was too unreliable to drive a hill. What was established:

- The measured car box is stable. Logged every re-measure for 40 s of start-line
  collisions, only min y moved, by 0.03 with the suspension. Nothing the game hangs under
  the master actor inflated it.
- The Thunderbucket is 0.32 x 0.26 x 0.78 units. With the first defaults (height 0.42,
  0.04 ahead of the nose) a lamp sat about 0.12 above the road and well outside the car.
  The game keeps the *car* out of the scenery; nothing keeps a lamp out of it, so a lamp
  held ahead of the bumper is buried by a slope the nose is only just meeting, and a
  buried emitter lights nothing. The defaults are now 0.55 and 0.01: the box already
  includes the bumpers, so 0.04 was never needed to clear them.
- A pool that fades over a crest, or under hard acceleration, is the beam leaving the
  road: it is only pitched 4 degrees down. That is a setting, not a fault.
- This runtime fork freezes a re-described light after a few frames unless the light is
  flagged dynamic (`fork_hooks::updateLightStaticSleep`). The lamps are flagged.

The Headlights tab now says why the player's car is dark when it is ("flagged as wasted",
"not being drawn", ...) or that it is lit and how big the game says it is, which separates
a lamp the proxy never submitted from one Remix occluded.

The game binds H itself (KEYMAP action 59, the horn); the toggle has since moved to F (section 48).

### 33.8 The lights were a frame behind the car (2026-09-21)

Reported: the faster the car goes, the smaller the cone, until it is gone. That is a lamp
being left behind. Remix gathers API lights when it injects: `LightManager::prepareSceneData`
opens with `flushPendingLightMutations`, which applies the queued `CreateLight` and
`DrawLightInstance` calls, and API calls ride the same bridge queue as the draws. The
headlights were described at the *end* of `brender_inject::submit`, after
`trigger_injection` had already marked the injection point, so frame N's lamps were lit in
frame N+1 -- around a car that had moved on. The lamp trails by speed x frame time: at 13
units/s and 60 fps that is 0.2 units, against a mount 0.01-0.04 ahead of the nose. The
emitter slides back into the bodywork, the shell shadows more and more of the cone, and
then all of it. It also explains 33.7's "going uphill": that is where one accelerates.

The lamps are now described at the top of `submit`, ahead of every draw. Seen working
with the car stationary; not seen at speed, because synthetic input could not get the car
clear of the start-line scrum.

### 33.9 Not verified in game yet

The defaults (brightness 10, emitter radius 0.012, cone 29 degrees, 4 degrees down) were
tuned in game by the user from a computed brightness 20 and cone 38, which were too bright. The
mode notice was not seen on screen. Whether distant, physics-inactive opponents keep a valid master
matrix is inferred, not observed; "Range from camera" keeps their lights off beyond 12
units either way.

## 34. Sprites look grainy and blobby: they were untagged, so Remix dithered them (2026-09-23)

Reported: explosions, fire, blood and the pickup sparkle look "blobby and grainy" under
Remix. Two causes. The main one is fixable in config; the other is the source art.

### 34.1 An untagged blended surface gets stochastic alpha in primary rays

The proxy draws every sprite with `SRCALPHA / INVSRCALPHA` blending. Remix sends a blended
surface one of two ways (`rtx_instance_manager.cpp` ~1205):

- **Tagged in `rtx.particleTextures`**: `isParticle` puts it in the unordered TLAS.
  `evaluateOpaqueApproximations` (resolve.slangh:136) then blends it by its opacity and
  lights it from the volumetric radiance cache. The result is smooth and has no noise.
- **Untagged**: it is resolved as an ordinary surface. With `rtx.enableStochasticAlphaBlend`
  (default on), every primary-ray hit with opacity in `(resolveTransparencyThreshold, 0.95]`
  is randomly passed through or stopped (resolve.slangh:510). The "Stochastic Alpha Blend"
  pass then fills the holes from neighbouring pixels. Dithering gives the grain, and the
  neighbour search plus the denoiser give the blobs.

`rtx.particleTextures` held one real entry: `SMOKE.PIX`. Every other sprite system from
section 25 took the stochastic path. The second entry, `-0x7A7F97E979F35B95`, is a
**removal**: in a hash list, a leading `-` removes the hash from the set
(`util_hash_set_layer.h:143`). It is not a signed hash. The same applies to the `-0x…`
entries in `decalTextures` and `worldSpaceUiTextures`.

### 34.2 Remix texture hashes can be computed offline

The hash is `XXH3_64bits` over mip 0 of the texture as uploaded
(`D3D9CommonTexture::SetupForRtxFrom`, d3d9_common_texture.cpp:683). `upload_pixelmap` always
uploads a tightly packed `A8R8G8B8`, so decoding a `.PIX` exactly as `upload_pixelmap` does
and hashing its BGRA bytes reproduces the runtime hash. Two tags from the user's runtime
confirm this: `SMOKE.PIX` = `0x4FF62ABC0E61D6A2` (particle) and `OILSMEAR.PIX` =
`0x7FF529EA8568DA95` (decal).

The sprite frames load as loose files from the `PIX16` folders, not from the `.TWT`
archives. `General.txt` (DATA.TWT) and `PEDS/SETTINGS.TXT` name 22 emitter frames, and
the track TXTs name none. `rtx.particleTextures` now tags 47 hashes:
`EX00000..7`, `BING1..6`, `TWINK1..4` (COMMON/boom), `FLM01..20` (COMMON/flames),
`BIGBL01..05` (PEDS/GIBLETS), `CSPLASH/CSPLISH/CSPLOSH` (COMMON/casplash) and `SMOKE`.
These are set in both `release/rtx.conf` and the installed one. The installed file was
backed up as `rtx.conf.pre-particles-bak`.

### 34.3 What the tag cannot fix

- **Resolution and bit depth.** Every sprite is `BR_PMT_RGBA_4444`, 16x16 (`TWINK`),
  32x32 or 32x64 (`FLM`), or 64x64 (the rest), with at most 16 alpha levels. `FLM*` have
  only 5-7 alpha levels, and the blood frames are stippled dots in the source art. Drawn
  across a car-sized billboard at 1440p, they are soft whatever the renderer does.
  The only remedy is replacement textures: upscaled or repainted, and keyed on the hashes
  above.
- **Brightness.** A particle takes albedo × volumetric radiance, so fire is lit by the
  scene instead of glowing. In BRender these materials are full-bright (flags exactly
  `BR_MATF_ALWAYS_VISIBLE`, section 32.1). Two ways to make them glow: an emissive
  replacement material on the same hashes, or drawing them with an emissive (additive)
  blend so Remix classifies them as `emissiveBlend`. 34.4 does the second.

The particle tag was confirmed in game on 2026-09-23: the sprites look better.

### 34.4 Full-bright sprites go out additively (2026-09-23)

Remix classifies a blended draw by its blend factors (`rtx_instance_manager.cpp` ~735).
`SRCALPHA / ONE` is `BlendType::kAlphaEmissive`. With
`rtx.enableEmissiveBlendEmissiveOverride` (default on), and a material that still uses the
legacy alpha state, the instance's emissive texture becomes its albedo texture, at
`rtx.emissiveBlendOverrideEmissiveIntensity` (default 0.2). In
`calcOpaqueSurfaceMaterialOpacity`, `kAlphaEmissive` has opacity 0 and emissive influence
= alpha. So the sprite glows by texture x alpha x intensity and blocks nothing behind it.
`ONE / INVSRCALPHA` (premultiplied) lands on the same blend type, so there is no blend mode
that both glows and occludes.

`resolve_draw_state` sets `draw_state::emissive` on a blended run whose material is
`material_is_fullbright` (flags exactly `BR_MATF_ALWAYS_VISIBLE`) and whose `colour_map`
name does not start with an `[Effects] EmissiveSpriteExclude` prefix. `draw_pass` binds
`D3DRS_DESTBLEND = ONE` for such a run and `INVSRCALPHA` otherwise. That covers the sprite
pool (explosion `EX*`, sparkle `BING*` / `TWINK*`) and the car flames (`FLM*`). Blood
(`BIGBL*`) comes from the same pool on the same kind of material, so it is excluded by
name, and that exclusion is the default. Smoke (flags 0x27) and splashes are not
full-bright and keep their alpha blend. The console logs each sprite texture once:
`emissive sprite: texture '...' drawn additively`.

Consequences:

- The dark, smoky tail frames of the explosion (`EX00005..7`) now glow dimly instead of
  darkening what is behind them.
- The emissive override replaces the emission of a mod material on the same texture. The
  user's hand-made emissive on `mat_B7FE737F9619BAF5` (`EX00001`) no longer applies. It
  would come back if that material overrode the legacy alpha state.
- `rtx.conf` sets `rtx.emissiveBlendOverrideEmissiveIntensity = 1`, the intensity the
  user's hand-made emissives use. Nothing else in the port draws with an emissive blend,
  so this global option only affects these sprites. It is tuned from the Remix menu.

`[Effects] EmissiveSprites=0` restores the alpha blend.

### 34.5 The full-bright test never matched, and why tagged particles go black (2026-09-23)

In the first run with 34.4 the sparkle came out black and car smoke black, and the log had
no `emissive sprite` line. The sprite materials had logged as "authored ambient 1.0"
rather than "effect material", so their flags at draw time were not 0x800.

Section 32.1 stopped reading too early. Both init routines pass the material on to the
preset routine at **0x005182F0** (`ecx = &material`, `edx = count 1`, stack arg = style)
right after the flag writes: `InitSpriteParticlePool` at 0x004EAA7D (style pushed at
0x004EAA49) and `InitFlames` at 0x004FC4CD (style pushed at 0x004FC4A0). Style 4 sets
`ka = 1.0`, `kd = 0`, `ks = 0`, clears PRELIT and sets LIGHT and SMOOTH. Depending on
three option globals (0x0074CF38, 0x0074CF20, 0x0074CA54) it also ORs in 0x20000, 0x10000
or 0x20. So the runtime recipe is **ALWAYS_VISIBLE | LIGHT | SMOOTH, ka 1.0, kd 0**: lit,
but only by an ambient of 1.0, which is full brightness. `material_is_fullbright` now tests
the 0x800 bit, `ka >= 1` and `kd == 0`. Styles 1-3 are other ambient/diffuse presets,
and styles above 4 give ka 1.0 with LIGHT left alone. This is the `BrMaterialSetPreset` that
kb.h has at 0x005183F0; 0x005183F0 is inside it, and the entry is 0x005182F0.

**Why a tagged particle that is not emissive renders black.** `evaluateOpaqueApproximations`
lights a particle as `albedo x evalVolumetricNEE(VolumeFilteredRadiance...)`, which is the
froxel radiance cache. That cache is filled from sampled analytic lights only: nothing in
`shaders/rtx/pass/volumetrics/` samples the sky. The sky is this port's only real light
(section 33), so outside the headlight cones the cache holds almost nothing, and
smoke, blood and any unemissive sprite come out near black. Before section 34 tagged them,
they took the stochastic path, which the path tracer lights from the sky.

Smoke is not meant to be black. `InitSmokeColours` at 0x004FB910 fills the per-type colour
table at 0x006B7840: type 0 from the runtime globals 0x006AA5B4/B8/BC, then `0x404040`,
`0x808080`, `0xC8C8C8` (x2) and `0xFEDF43` (from `.data` 0x00660160..0x0066018C). The game
draws those colours prelit, so they are the final colour: dark to light grey smoke, and yellow.

Remix has no particle mode that is unlit and also blocks what is behind it. An emissive blend
has opacity 0, and the world-space-UI path emits the bare texture without the vertex tint.
Blood (excluded from 34.4) and smoke therefore stay dark while they are tagged as particles.

### 34.6 Unlit sprites: one alpha-blended draw plus one additive copy (2026-09-23)

In the texture-stage evaluation (opaque_surface_material_interaction.slangh ~724), Remix
passes the emissive colour through the same colour op as the albedo. With
`MODULATE(TEXTURE, DIFFUSE)` a run's emission is therefore texture x vertex colour.
`emissiveBlendOverrideInfluence` is the stage's alpha (texture x TFACTOR). So an
additive draw glows in its own tint, scaled by an alpha the proxy sets per draw.

To draw smoke and blood unlit (BRender's full-bright colour over background), the
blended pass draws the run twice:

1. The ordinary alpha blend. As a particle it takes `1 - alpha` off what is behind it, and
   its own lighting (volumetric cache, section 34.5) adds almost nothing.
2. The same triangles with `DESTBLEND = ONE` and TFACTOR alpha = opacity x
   `[Effects] UnlitSpriteBrightness`. This adds colour x alpha x
   `rtx.emissiveBlendOverrideEmissiveIntensity`.

The sum is `colour x alpha + background x (1 - alpha)`, i.e. the original blend. The unordered resolve
accumulates any-hits without sorting. If the darkening draw is hit first, the glow is also
attenuated by `1 - alpha`, so an unlit sprite lands between `c x a` and `c x a x (1 - a)`.

`classify_glow` picks one of three glow modes per blended run. `unlit` is used for the
smoke models (`gBlend_model` / `gBlend_model2`, the `br_model*` globals at 0x0074CF30 /
0x0074CF94) and for full-bright sprites that match `EmissiveSpriteExclude` (blood).
`additive` is used for the remaining full-bright sprites. `lit` is used for everything
else. The log names each glowing texture once: `emissive sprite: ...` / `unlit sprite: ...`.

Whether a glowing particle lights anything else is a global Remix setting. Emissive particles
are never NEE lights. They reach other surfaces, and reflections, only through
`rtx.enableUnorderedEmissiveParticlesInIndirectRays`, which the user's `user.conf` turns on.

Reported working in game.

---

## 35. Distorted headlight shadows on walls: Remix's scene scale, not the normals (2026-09-23)

Reported: with the headlights on inside the Runway Runaway airport (`AIRPORT1.TXT`, 
`Airport1.TWT`), the shadows on the walls look distorted. The flat-normals change in the
working tree (per-face plane normals for non-`SMOOTH` materials) was meant to fix this and
did not.

### 35.1 The flat-normals path never runs, and the premise was wrong

Every material the game loads passes through the preset routine at 0x005182F0 (section
34.5), and styles 1-4 all OR in `SMOOTH`:

- The material-file loader 0x00502060 hands its whole batch to the preset routine with the
  style in the global **0x00660CB8** (-1 means 1). Its callers set that global:
  0x004F6640 (common store 0x0074D400, style 1), 0x004F6740 (cars, style 2),
  0x00502CF0 (caller's style: 3 for the ped gibs, 2 elsewhere) and 0x00502AD0.
- In 16-bit colour mode (`[0x0074CA60] == 0x10`, which is this port's Glide path),
  0x004F6640 then forces `flags = (flags & ~PRELIT) | LIGHT | SMOOTH` on the whole common
  store, and 0x004F67A0 does the same to each car's materials.

So BRender Gouraud-shades the track with the prepared vertex normals. Those normals are
what the original game displayed, and `!(flags & SMOOTH)` is false for practically every
material. The flat path was dead code. Had it run, it would have faceted surfaces the game
draws smooth.

The authored normals are also sound. `Airport1.dat` (209 models) was parsed and the
vertex normals rebuilt the way BRender does it (face-normal sum over faces that share the
vertex and overlap in the `smoothing` mask). Only 1.5% of the surface area has a corner normal
more than 10 degrees off its face, and most of that is props (powerups, luggage, trolley,
chopper). The runtime report agrees: `normals: 8192 sampled, 0 with no direction, 0 not unit
length`, `4096 of 4096 sampled triangles wind outward`.

### 35.2 The shadow-terminator offset is sized in centimetres of the wrong world

`rtx.sceneScale` is unset, so it defaults to 1, meaning "one game unit = 1 cm"
(`getMeterToWorldUnitScale() = 100 * sceneScale`). One BRender unit here is about 6.9 m
(section 33.2), so Remix believes the world is about 690x smaller than it is.

Remix offsets every shadow ray's origin along the interpolated vertex normals to hide the
triangle-shaped terminator artefacts on low-poly curved surfaces
(`calcShadowTerminatorOffset`, surface_interaction.slangh ~151; applied to the RTXDI
visibility rays in RtxdiApplicationBridge.slangh:294/552). The offset is capped by
`rtx.shadowTerminator.maxLength` (0.02 m) and skipped on faces larger than `maxArea`
(0.05 m^2). Both are converted with the scale:

| | default | at sceneScale 1 | what that really is here |
|---|---|---|---|
| maxLength | 0.02 m | 2 game units | ~14 m, five car lengths |
| maxArea | 0.05 m^2 | 500 unit^2 | every track face qualifies |

Evaluating the offset formula at each face centre of `Airport1.dat`: 9.8% of the area
gets an offset > 0.003 units, 6.6% > 0.05 units, 2.7% > 0.2 units (half a car length), and
the worst pieces reach 1.8 units. A headlight shadow ray starting that far off a wall
misses the geometry near the wall, which moves and bends the shadows.

At the true scale the limits are 0.0029 units and 0.00105 unit^2. That switches the offset
off on every track face and keeps a sub-centimetre offset on car panels, which is the
low-poly curved case the feature exists for.

### 35.3 What changed: rtx.sceneScale = 0.001449

`rtx.conf` (installed and `release/`) now sets `rtx.sceneScale = 0.001449`: 1 cm per 6.9 m
unit, so `getMeterToWorldUnitScale()` = 0.145 units per metre. The shadow-terminator
defaults then mean what they say: 0.0029 units and 0.00105 unit^2. The same fix also
corrects every other metre-based setting: NRD's hit-distance parameters, the NEE-cache
range, the neural radiance cache bounds and the fog-remap distances.

Two rtx.conf values were tuned under the old scale and were rescaled to keep their look:

| option | was | now | why |
|---|---|---|---|
| `rtx.volumetrics.transmittanceMeasurementDistanceMeters` | 15 | 10350 | 15 m at scale 1 was 1500 units, nearly clear air across a track; 1500 units x 6.9 m keeps that. At 15 true metres the track would drown in haze. |
| `rtx.freeCameraSpeed` | 7.4 | 5100 | the speed is multiplied by `sceneScale` (rtx_camera.cpp:443) |

What changes and was left at defaults:

- `rtx.volumetrics.froxelMaxDistanceMeters` (20 m) now reaches about 2.9 units, where it
  used to reach 2000 units (the whole map). The volumetric grid, and so the particle
  lighting cache (section 34.5), now covers 20 real metres around the camera, at far finer
  resolution.
- **Geometry hashes change.** `hashRegionLegacy` rounds positions to
  `0.01 m * getMeterToWorldUnitScale()` before hashing (rtx_hashing.cpp:180). At scale 1 that
  was a whole game unit; now it is 0.00145 units. Every mesh hash moves, so the mod's
  `mesh_EC5C7EFFFACAF8E7` (a car glass pane whose normals were replaced with its plane
  normal) no longer applies and has to be re-captured and re-keyed. Texture hashes, and so
  every texture tag and material replacement, are unaffected. Recomputing the new hash
  offline was not attempted: it depends on the exact vertex-buffer region, stride and index
  data of the draw.

### 35.4 Noted, not changed

`bake_actor` carries normals through the placement matrix itself rather than its
inverse-transpose and does not renormalize. That is wrong under non-uniform scale, but none
of the 119,172 actor transforms the game ships (every `.ACT`, loose and in `.TWT` archives)
is non-uniform. 20 carry a uniform scale other than 1, which only changes the normals'
length.

Reported working in game.

---

## 36. Oil slicks: a second decal pool the proxy did not know (2026-09-23)

Reported: oil slicks on the ground have a white border. Capture
`capture_2026-09-23_14-20-37.usd`: the slick's thumbnail shows the whole quad drawn,
with its transparent area opaque white.

The slick texture is `OIL.PIX`, Remix hash `0x7A7F97E979F35B95`, with exactly two
values: opaque black (2700 texels) and `(0,0,0,0)` (1396). There is no white in the
texture or the capture, and the mod has no override for it. In the capture the instance
is tagged `decal_Static` (rtx.conf lists the hash), but its draw state is
`alphaBlendEnabled False` with only an alpha test (`GREATER 0`). Its 0.82-unit quad lies
flush with the road (world y 2.01..2.08 on a sloped surface), with no lift.

Why that renders white:

- Remix reads decal categories only on a blended draw (`rtx_instance_manager.cpp` ~853,
  inside the `blendEnabled` branch). Unblended, the tag is inert and the quad is an
  ordinary alpha-tested surface.
- The proxy sent it unblended because `geometry.solid` was true. The proxy called the quad
  "solid" because it was in neither the ground-decal pool nor the sprite pool, and
  `SolidTranslucency` turns a solid alpha-carrying run into an alpha test.
- An alpha-tested quad exactly coplanar with the road: a primary ray that hits one of its
  transparent texels continues from just past the hit. That is past the road plane too, so
  the ray escapes under the track. What shows there is the fog and sky, white on this
  track.

**The pool.** `InitOilSpills` @ **0x004A6A10** builds 32 slots at **0x00690C90**, stride
**0x54**, each beginning with its `br_actor*`. Each slot gets its own
`BrModelAllocate(NULL, 4, 2)` quad (`x,z in -1..1`, normals +Y), its own material
(`colour_map = OIL.PIX`, `IDENTITY.TAB` shade table, preset style 2), and
`BrActorAdd(0x0074D64C, actor)`. That parent is one of the backdrop actors
`RenderView` draws under a screen-space depth bias (section 27). The bias is how BRender
kept the pool on top of the road it lies flush with.

The `GROUND_DECAL_POOL` comment claimed that pool covered oil spills and that nothing
else lays a quad flat on the ground. Both were wrong. `game::OIL_SPILL_POOL
{ 0x00690C90, 0x54, 32 }` now joins it in `is_decal_model`, so an oil spill is non-solid
(blended, which makes Remix's decal tag take effect), lifted by `[Effects] DecalOffset`,
and never baked into a static chunk (`vanishes_outright`).

`Oil_Slick` (0x0065EC78, used at 0x004DE048) is a powerup's name, unrelated.

Reported working in game.

---

## 37. A synthesized sky for stock Remix (2026-09-23)

The port's only light is the sky. On a Remix runtime without a physical atmosphere, nothing
drew one: the horizon scene is captured but never submitted (section 27), so the sky was
black and the track unlit. The proxy now draws a sky Remix rasterizes itself, following
the OpenJKDF2 port's `sithRenderSkybox` design.

### 37.1 What the game has (RaceTxtLoad 0x00504BF0, from disassembly)

Each race TXT's "HORIZON STUFF" block gives a sky texture, a horizontal repetition count,
the texture's vertical extent in degrees, and the pixel row of the horizon. At runtime:

| global | what | written at |
|---|---|---|
| `[0x0075D75C]` | the loaded sky `br_pixelmap*` (`BrMapFind(name)`; NULL for "none") | 0x00505DDE |
| `[0x0075D778]` | the **live** sky, copied from 0x75D75C by `CommitLevelDepthCue` (0x0044723B); NULL while the camera is in a special volume that hides the sky | 0x00445361 |
| `[0x0079EC2E]` u16 | `65536 / repetitions` (a br_angle) | 0x00505E18 |
| `[0x0079EC2C]` u16 | vertical extent, `degrees * 182.044` | 0x00505E30 |
| `[0x0079EC30]` u16 | bottom edge below the horizon, `(H - horizon_row) * extent / H` | 0x00505E56 |

`SetDepthCue` (0x00445340) puts the same pixelmap in HORIZON.MAT's `colour_map`. On the
hardware path there is no shade table on the horizon. kb.h had `0x0075D778` as
"g_fogShadeTable": it is the sky. A `.FLI` sky name gives an animated pixelmap that a
FLIC player rewrites; no shipped track uses one.

`DrawHorizon` places the texture world-locked. u = 0 faces world -Z, u increases clockwise
seen from above (`u = reps * atan2(dx, -dz) / 2pi`), and the scroll matches yaw exactly.
The model builder (0x00445E20) and the v generator (0x00445500) put elevation 0 at row
`horizon_row`, with the texture spanning `extent` degrees (less a one-texel inset). The
hardware horizon itself is a 22x4 band sized to the screen, with caps that clamp the edge
rows. With no sky, the frame is filled with a DRRENDER.PAL index (0 = black, or 255
when fog type 1), not the depth-cue colour (0x004E5A35..0x004E5A56).

All 23 shipped sky textures are 256x256 RGB565. Seven tracks have "none" (silos,
arena, nuke), and they get no synthesized sky, matching the game's black fill.

### 37.2 What Remix needs

A draw Remix classifies as sky is re-rasterized into a screen-sized sky matte and into a
cube probe around the camera (`RtxContext::rasterizeSky`: the game's projection with the
frustum forced to 90 degrees and the far plane dropped, viewed from
`inverse(view).translation`), then hidden from the acceleration structure. The probe is
the sky's lighting. So the geometry has to make sense seen from the camera in every
direction.

A draw is sky when its texture is in `rtx.skyBoxTextures`, **or when its viewport's
MinZ >= `rtx.skyMinZThreshold` (default 1.0)** (`shouldBakeSky`, rtx_types.cpp:551). The
proxy uses the viewport route, so no per-track hash has to be tagged.

### 37.3 What the proxy draws (`sky_dome`, `brender_inject::draw_sky`)

- **Shell**: a unit lat/long sphere, 64 columns x 32 rings, one static VB/IB, drawn with
  `DrawIndexedPrimitive` (Remix replays sky draws against the bound buffers). Eye position
  and radius (midway between hither and yon) are in WORLD, so the mesh hash is the same on
  every track. It is drawn first in the race submit with Z test/write off, blend/alpha test
  off, fog off, cull none, and a viewport of MinZ = MaxZ = 1. The viewport is restored
  afterwards.
- **Panorama**: 2048x1024 equirectangular, full box-filtered mip chain, opaque, wrap U /
  clamp V, anisotropic. Between the texture's top and bottom rows it is the game's
  layout exactly. Beyond them, the edge row is box-filtered over a widening arc of azimuth
  (prefix sums over the row) until, 75% of the way to the pole, it is the row's
  average. That gives a flat zenith and nadir colour with no streaks converging on the
  poles.
- **Rebake**: keyed on the pixelmap's pixel pointer, dimensions and the three layout
  numbers, so a track load rebakes and a restart does not. The console log reports each
  bake: `sky: baked panorama from '<name>' (...)`. The panorama hash is a pure
  function of those inputs, so a sky replacement made from a capture keeps applying.
- `decode_pixelmap` was split out of `upload_pixelmap` so both use one decoder.
- `[Sky] Synthesize` (default 1) turns it off, for runtimes that bring their own sky.

The mapping was checked offline by running the same bake in Python over `skyblue_01`,
`cityskape` and `twinpink`: skylines on the horizon, the right repeat count, and flat
poles.

Reported working in game.

---

## 38. Swap chain shrunk by display scaling (2026-09-24)

Reported: with nGlide set to 3840x2160 the game ran at 1536x864. That is 3840x2160 / 2.5:
Windows DPI virtualization at 250% display scaling. The resolution itself is nGlide's
(`HKCU\Software\Zeus Software\nGlide2\Resolution`, set with `nglide_config.exe`). Remix's
log shows the resulting swap chain (`D3D9DeviceEx::ResetSwapChain: Buffer size`).

The user's AppCompatFlags carried `HIGHDPIAWARE` for `...\CARMA2_HW.EXE`, but the Remix
launcher runs the renamed `CARMA2_HW0.EXE`, so the override no longer matched. The proxy
now calls `SetProcessDPIAware()` first thing in `DllMain`. It is loaded before the game
creates `Carma2MainWndClass` (main.cpp waits for that window), so that has the same effect
as the override, whatever the EXE is called.

Verified in game on 2026-09-24: the game runs at 3840x2160.

---

## 39. Opened car doors z-fight: the game makes them two-sided (2026-09-24)

Reported: the Thunderbucket's doors, once they fly open, show the outer paint and the
interior texture clashing, and the original game does it too. Research only; nothing is
changed yet.

### 39.1 How a flap opens (static, CARMA2_HW0.EXE)

- WAM crush entries are parsed by 0x0042A550 into 0x40-byte records hung off car+0x578.
  Keyword tables: crush type `boring`/`flap`/`detach` at 0x0058F848. A `flap` gets a 0x2C
  record at +0x30: hinge vertex indices at +0x0A/+0x0E/+0x10, Kev-o-flap byte at +0x20,
  current angle at +0x04, limits at +0x06/+0x08 (0x3FFC, about 90 degrees), and
  "is a door" (+0x28, name contains "door"/"dor").
- The swing is a **rotation of the door actor**. 0x004321D0, reached from the crush entry
  0x00431E20 when Kev-o-flap is 0, rebuilds actor+0x2C as translate(-hinge0), rotate
  about hinge0->hinge1, translate(the parent body's matching vertex). The same routine
  also bends the door's vertices slightly along the door plane's normal.
- Deformed models get `model->custom = 0x00431590` (0x00432E53), which calls
  `BrModelUpdate(model, 1)` at render time. That path rebuilds the prepared face planes
  (`BuildOnlineFacePlanes` 0x0051F6A0), so **culling stays correct**. Vertex normals are
  recomputed before the planes, so they lag one update behind (lighting only).

### 39.2 The cause: TWO_SIDED is set on the door's materials

The first time a normal flap opens, 0x004381B0 walks the door model's prepared groups and
ORs **`BR_MATF_TWO_SIDED` (0x1000)** into each group's material, then
`BrMaterialUpdate(mat, 2)` (0x0043829D). The Kev-o-flap hinge-joint path (0x004372DB),
detaching (0x004335B5) and splitting a car (0x0042E2E0, every LOD) do the same. It is only
cleared (0x0042DEB0, 0x0042D9B0) for materials whose name starts with `S`. The flag is set
on the shared material object, so other parts using the same material become two-sided
too. The Thunderbucket's `tbseats` and `tbrearwing` are shared with the body, hardtop,
front clip and engine.

Why that shows as z-fighting: in capture `capture_2026-09-24_13-28-05.usd` the open right
door's outer skin (`TBRDOOR`) and inner panel (`TBSEATS`) lie in the same plane with
**zero gap, facing opposite ways**. The door is a thin closed wedge (16 faces) that the
crush flattened. With back-face culling, coincident opposite faces are harmless: the skin
shows from outside and the panel from inside. With culling off, both are candidates from
both sides, in BRender's Z-buffer and in Remix's ray tracing alike. In the capture exactly
the flagged parts are `doubleSided` (`TBRDOOR`, `TBSEATS`, `TBREARWING`, `TBGRADS`). The
closed left door's `TBLDOOR` is not.

The texture hashes computed offline match the capture: TBLDOOR 30F014CFBF97458D, TBRDOOR
E7EF14948891D712, TBSEATS 189B674CF23020F4, TBREARWING 5B6191E7C062311E, TBGRADS2
0CED9367C58AF9D7 (the mod's metallic material).

### 39.3 Why the flag cannot simply be dropped

Of the 118 flapping parts across all cars (every car TWT's WAM `flap` actors, their models
welded by position), **97 are open meshes**: single-sheet doors, hoods and boot lids with
3 to 38 boundary edges, which would be see-through from inside once swung open without it.
21 are closed: the Thunderbucket's doors, Fair, Newhawk, the Ford pickup's tailgate, the
Mini's boot, Wideboy's and Zee's bonnets. Patching out 0x0043829D would fix the closed ones
and break the open ones.

A closed mesh never needs two-sided drawing: each face's back is hidden behind another face
of the same mesh. So the proxy can decide per model: honour TWO_SIDED only when the model
has open edges. That fixes the doors of the 21 closed parts, and the spill-over onto the
Thunderbucket body if the body is closed, and leaves the 97 open parts as the game draws them.

### 39.4 What the proxy does now (2026-09-24)

`[Effects] CullClosedMeshes` (default 1). `mesh_is_closed` welds a model's prepared
vertices by position (1/16384 of a unit) and requires every edge to border exactly two
faces. `draws_two_sided(material, closed)` then honours TWO_SIDED / ALWAYS_VISIBLE only
for open meshes. It is applied at part build, which is what static chunks bake, and per
draw in `resolve_draw_state`, which is what sees the flag the game sets mid-race.
`model_geometry::closed` is computed with the geometry.

For the Thunderbucket's models, only the doors (`ldoor`, `rdoor`), wheels, `drvbod.act`,
`rearbumper` and `engine` are closed. The body (`thunderbucket`, three- and four-way seam
edges), `hardtop`, `frontclip` and the rest are open, so the spill-over of TWO_SIDED onto
`tbseats`/`tbrearwing` leaves those parts two-sided as before. Their one coincident face
is the door jamb, hidden while the door is shut.

Reported working in game.

---

## 40. Fire and flame emissives (2026-09-24)

**Car flames.** In capture `capture_2026-09-24_13-28-05.usd` every car-flame frame
(`FLM12` = `846391EFE2F996E0`) reached Remix unblended and alpha-tested. The flame quad
(`"Lollipop"`, the `br_model*` at `0x006AA380`, InitFlames 0x004FC3A0) is in neither quad
pool, so SolidTranslucency treated it as solid and it never reached section 34's additive
path, even though its materials are full-bright (0x800 + preset 4, section 34.5).

`[Effects] AdditiveCarFlames` (default 1; also a checkbox in the F4 Effects tab, saved
back to the ini) treats the flame quad as a sprite. `model_geometry::flame` is set from
`ADDR_g_flame_model`, and `resolve_draw_state` passes `solid && !(flame && switch)` to
`plan_blending`. The flames then go out blended, the full-bright test makes them
additive, and they glow like the explosion frames. The choice is made per draw, so the switch takes effect at once. The
flame quad is in `vanishes_outright` regardless, so it is never baked. The splash quad
(`0x006A8758`) is left solid: splashes are not full-bright, and blending them would make
them particle-lit (dark, section 34.5).

With the switch off the flames are alpha-tested surfaces, and the mod's emissive masks for
them (below) apply. With it on they are inert: Remix replaces the emission of any blended
draw with its emissive-blend override. The same is true of the Emissive Helper's `EX00001`
mask, since the sprite pool is always additive (section 34.4).

**Other fire in the data.** Pixelmap names matching fire/flame/burn/expl/spark across every
TWT and loose PIX:

- `FIRE1..3` (Timber) are a stone fireplace wall; `FLAMING_DRUM01/03` (Junkyard) are the
  barrel's rust.
- `37FLAME1..3` are hot-rod paint, `BURNTBASE`, `NODDY_BURNT` and `ZEBURN` are scorch
  textures, and `AFTERBURNER`, `DISMEMBER` and `EXPLOPEDS` are powerup icons.
- Funfair's own `FLM01..FLM10` (FUNF.TWT, different pixels from the car flames) are the
  funk-animated `!=hoopflame` track material (funfair1.txt "frames ... Flm01..Flm10",
  "no fucking lighting"). It is alpha-tested track geometry, so masks are its only route.

**The masks.** They live in `rtxmod/emissives_fire.usda`, sublayered from `mod.usda` next to
the Emissive Helper's own `emissives.usda`, which the helper rewrites. There is one
`mat_<hash>` per frame, with masks in `assets/generated/<hash>_emissive.e.dds`
(A8R8G8B8, full mip chain). Each mask is the frame's own colour x smoothstep of its max
channel, **cut out only where alpha is 0**: an alpha-tested draw shows every texel with
alpha > 0 as solid, and these frames are mostly partial alpha.

- Car flames `FLM01..FLM20`: smoothstep 0.05..0.60, x1.0.
- Hoop flame: smoothstep 0.10..0.80, x0.8.

| family | frames | how it glows |
|---|---|---|
| explosion, powerup sparkle | EX00000..07, BING1..6, TWINK1..4 | additive (proxy) |
| car flames | FLM01..FLM20 | additive (proxy); masks when AdditiveCarFlames=0 |
| Funfair hoop flame | FUNF FLM01..FLM10 | masks |

**F4 menu.** A new "Effects" tab toggles `CullClosedMeshes` and `AdditiveCarFlames` live
and writes them back to the ini (`config::set_bool`). Culling changes reach dynamic models
immediately; baked chunks keep the value they were baked with until the next track load.

---

## 41. Destructible scenery was baked into the static world (2026-09-24)

Reported: the static baker bakes destructible objects, e.g. the Ski Track ice statues.
Capture `capture_2026-09-24_14-22-18.usd` confirms it. The two statues (`-icebear.act`, 96
faces each, texture `ICEBEAR` = `F26D0340ACA7E2EB`) are one chunk draw of 192 triangles
with an identity WORLD. So are the start gates (`STGATEPLNK`, `STGATEASPH`).

### 41.1 How the game smashes scenery (static, CARMA2_HW0.EXE)

- **Specs.** 0x004F0450 (from RaceTxtLoad 0x00505A09) parses each race TXT's "Smashable
  environment specs" into an array at `[0x006A5138]`, stride 0x2E0, count `[0x006A55B4]`.
  - +0x04: the resolved trigger (`br_model*` for kind 1, `br_material*` for kind 0).
  - +0x08: the trigger kind. 1 = a model (the name ends .ACT/.DAT or starts with `&`),
    2 = `&NN`, 0 = a material.
  - +0x0C: the mode (table 0x0065FE88). 0 nochange, 1 decal, 2 texturechange,
    3 remove, 4 replacemodel, 5 crush.
  - +0x2C0: the replacement `br_model*`.

  The "7" some mission TXTs show after an `&NN` trigger is not a mode but a bitmask byte
  (+0x05); the real mode follows it.
- **Marking.** At load 0x004F5CB0 renames every actor whose model is a kind-1 trigger to 11
  bytes: `name[0..4] | (index+1) name[-4..]`, with `'|'` at [5] and the spec index + 1 as
  the raw byte at [6]. The hit handler 0x004F1140 looks the spec up the same way
  (0x004F17F7).
- **remove** (SmashActor 0x004F4E20, case 3) writes `render_style = NONE` on the same
  actor (0x004F4FEF). It is not unlinked, moved or re-modelled. The scene walk then skips
  it and its children.
- **replacemodel** (case 4) writes the spec's new model into the same actor's `model`
  (0x004F4F2E) and sets every child to NONE (0x004F4FA2). The transform is untouched.
- Specs are re-parsed and actors rebuilt on every race start (DoGame -> InitRace 0x00481830
  -> RaceTxtLoad). Nothing restores a hidden actor during a race. Action replay swaps models
  back and forth but skips hides.

### 41.2 Why the baker missed them

The baker demotes an actor on a transform change or a model swap it observes, and punches
its triangles out of the chunk. It cannot see an actor that simply stops being walked, and
deliberately has no "not seen any more" rule, since streamed scenery would erode the
world. So a removed statue stayed in its chunk forever. A replaced model was caught by the
swap check, but its children, hidden by the same smash, were not.

### 41.3 The fix

`vanishes_outright` now also excludes **destructible scenery and everything under it**
(`destructible_ancestor`). It uses the game's own marking: an 11-byte identifier with '|' at
[5] whose spec is kind 1 with mode 3 or 4. Nothing else about the baker changed. These
actors are drawn dynamically from their first sighting, so when the game hides one it stops
being drawn, and a re-modelled one is drawn with its new model. The log names each one
once: `destructible scenery: '<model>' (removed when hit | re-modelled when hit) kept
out of the static world`.

Across all race TXTs the model-type remove/replacemodel specs are 200 entries (none are
material triggers). Per track:

- Ski Track: ice bears (2), start gates (2).
- Airport: barrier gates (5).
- Carrier: guns (8); plus WIGGLYBIT/CORE in net and mission.
- Desert: gas cylinders (9); plus the flying saucer on desert3.
- New City: petrol pumps (9-11); plus comsats on newcity4 and mission.
- Timber: petrol pumps (9).
- Silo: 12 cylinder models (2 each) and gas pipes (13); the mission adds gasometers (3),
  hatches (4) and the missile centre (2).

Reported working in game.

## 42. Ground shades wavy: the game's vertex normals average across hard edges (2026-09-24)

### 42.1 Symptom and cause

Flat ground, most visibly a tunnel road under the headlights, shows wavy light and shadow in
Remix. In capture_2026-09-24_15-27-23 the road faces (texture 0013076B066A42A7) have face
normals with y about 0.98, but their vertex normals lean 40 to 63 degrees off. The road
shares its edge vertices with the tunnel walls, and BRender's prepared normals average every
face at a vertex, the 90-degree wall faces included. A rasterizer lighting per vertex hides
this. Remix shades per pixel, so the normal swinging across a wide road face reads as waves.

Across the whole capture, the angle between each face corner's normal and its face plane
was p50/p90/p99 = 15.3/55.7/87 degrees. The angles between adjacent faces cluster below
5 degrees, with a second cluster at 85 to 95 degrees (3021 edges) and more above 95 (1978).
Those are real hard edges, and they were being smoothed.

### 42.2 The fix

`extract_geometry` now computes its own shading normals (`crease_normals`). Vertices are
welded by position across all of a model's groups, because the faces on either side of a
crease often carry different materials. Each face corner then averages the planes of the
faces at that vertex that lie within `[Effects] CreaseAngle` (default 45) of its own face.
A source vertex is emitted once per distinct normal, so it splits only along a crease.
Degenerate faces keep the game's normal. CreaseAngle=0 keeps the game's normals.

The fix applies to everything the proxy builds: baked static chunks, dynamic actors, and
cars, including cars rebuilt after damage. Simulated on the capture, corner deviation
p50/p90/p99 becomes 2.9/13.8/23.7 at 45 degrees (2.0/9.2/15.6 at 30, 3.3/18.2/30.8 at 60).

Reported working in game.

## 43. Sun direction (2026-09-24)

Static analysis only (Ghidra headless, capstone). Answer: there is exactly one directional
light, created once at startup with a hard-coded orientation. No track sets a direction, and
the light does not follow the camera or the car.

### 43.1 Where the light comes from

- `InitialiseDeathRace` 0x004924A0 calls `InitialiseWorld` 0x0047DD20 once. That function
  allocates `g_world_root_actor`, runs `LoadInRegistees` 0x00486E10 (0x0047DFA3), and later
  runs `AddLightsToWorld` 0x0047E500 (0x0047E136).
- `LoadInRegistees` walks `DATA\REG\{PALETTES,SHADETAB,PIXELMAP,MATERIAL,MODELS,ACTORS,LIGHTS}`
  with `DRForEveryFile` 0x0048F360. For `LIGHTS` the per-file callback is `LoadInLight`
  0x0048F2E0. `REG\LIGHTS.TWT` holds a single file, `SIMPLE.LIT`, so there is one light.
- `LoadInLight` ignores the file (its path arrives in ecx and is never read; nothing is loaded).
  It does this:
  - `BrActorAllocate(2 /*LIGHT*/, NULL)`
  - `light->type = 1`: BR_LIGHT_DIRECT, with the BR_LIGHT_VIEW bit clear, so the light is in
    model/world space.
  - `colour = g_light_rgb` 0x006572CC..D4 packed as 0xRRGGBB, `attenuation_c = 1.0`.
  - `BrMatrix34RotateX(&actor->t.mat, 0xD558)` at 0x005327F0. 0xD558 is -60.0 degrees.
  - `BrMatrix34PostRotateY(&actor->t.mat, 0x1554)` at 0x00533A60. 0x1554 is +30.0 degrees,
    and the function computes mat = mat * RotY via `BrMatrix34Mul` 0x00532620 and
    `BrMatrix34Copy` 0x005325D0.
  - Appends the actor to `g_lights` 0x0074B3E0 (`g_num_lights` 0x0068C720), then calls
    `EnableLights` 0x0047D6D0, which calls `BrLightEnable` 0x00524E30.
- `AddLightsToWorld` does `BrActorAdd(g_world_root_actor, light)`, frees any children, and
  calls `BrLightEnable`. The light is a direct child of the world root. No code was found
  writing the root's matrix. The check was a scan of every load of 0x0074D44C for an access
  to +0x2C..+0x5B in the next 5 instructions; it found none.
- `SIMPLE.LIT` has its own TRANSFORM_MATRIX34 chunk (Z row 0.8735, 0.4134, 0.2569), but the
  game never reads it.

### 43.2 The direction

The two rotations were run through an FPU simulation of the game's own code:
RotX = rows (1,0,0) (0,0.5002,-0.8659) (0,0.8659,0.5002), and RotY = rows
(0.8661,0,-0.4999) (0,1,0) (0.4999,0,0.8661). Their product, the light's world matrix, is:

```
row0 X = ( 0.8661,  0.0000, -0.4999)
row1 Y = (-0.4329,  0.5002, -0.7500)
row2 Z = ( 0.2500,  0.8659,  0.4332)
```

`BrSetupLights` 0x005250E0 (called from `SceneSetupCameraMatrices`, 0x00521DBF) sends a
DIRECT light's BRT_DIRECTION_V3 as column 2 of view_to_light. That is the light's +Z axis in
view space. BRender treats it as the vector toward the light (N.L > 0 is lit), so the light
shines down its local -Z.

- **Toward the sun (world, Y up): (0.250, 0.866, 0.433)**, 60.0 degrees above the horizon
  at azimuth atan2(x, z) = 30 degrees.
- **Direction the light travels: (-0.250, -0.866, -0.433).**

This is the same on every track and in every view.

### 43.3 What the race TXT actually does

`RaceTxtLoad` 0x00504BF0 calls `ParseGlobalLighting` 0x00486DC0 at 0x00505078. It reads the
RGB into `g_light_rgb` (defaults 255,255,255 in .data) and the three ambient/diffuse pairs
into 0x006572E8/EC, 0x006572E0/E4 and 0x006572D8/DC (defaults 0.2/0.8). The only other
reader of `g_light_rgb` is `LoadInLight`, which runs at startup before any race loads. The
per-track light colour is therefore dead data: the light stays at the .data default of white.
Every shipped track uses white anyway. The ambient/diffuse pairs feed material ka/kd
(`LoadCommonMaterials`/`FixCarMaterials` at 0x004F6703, 0x004F68BB and 0x004F6B1C) and the
effects pass (0x004E91FD). They are not a light.

### 43.4 Camera, car and other lights

- Nothing re-orients the light. The only references to `g_lights` are `LoadInLight`,
  `EnableLights`, `DisableLights` 0x0047D6A0 and `AddLightsToWorld`. The effects pass wraps
  its `BrZbSceneBeginFrameSetup` call (0x004E94C2..0x004E94EA) in Disable/EnableLights.
- The only other two `BrActorAllocate(2)` sites are outside the race scene:
  - 0x004664DC is in the off-race model-preview renderer 0x00466460. It has its own root and
    camera, and frees them afterwards.
  - 0x0046E888 is in the menu-screen setup 0x0046E830. It stores the light in 0x00763844
    and never adds or enables it.
- Car headlights are not BRender lights (section 33).

### 43.5 The sun in Remix

The `sun` module (`src/comp/modules/sun.*`) gives Remix this light as an API distant light.
It is described once, then described again only when a setting changes, since creating under
the same hash replaces the light. It is drawn on every race frame, just after the headlights,
and destroyed when the race is left. Its defaults are the game's: elevation 60, azimuth 30
(from +Z towards +X) and white. Remix wants the direction the light travels,
-(cos e sin a, sin e, cos e cos a).

Brightness is the API radiance. Remix's distant-light sample divides it by sin^2 of the half
angle (`distant_light.slangh`), so the irradiance comes out as pi x radiance whatever the
angular diameter. At 1.0, a white surface facing the sun is lit to full brightness, the same
as a legacy D3D9 directional light at `lightConversionDistantLightFixedIntensity` 1.0. The
angular diameter only changes how soft the shadows are.

The F4 menu has a Sun tab. Its settings save to `carma2-sun.ini` in the game folder, in a
[Sun] section, with the keys Enabled, Elevation, Azimuth, ColourR/G/B, Brightness,
AngularDiameter and VolumetricScale.

Reported working in game.

### 43.6 The sun sometimes missing after the menus

The sun was destroyed whenever the race view was left, and created again when it returned.
In the Remix Plus base (dxvk-remix gmod fork, `rtx_light_manager.cpp`), the two calls take
effect at different times:
- `CreateLight` adds the light at once (`addExternalLight`, emitted to the CS thread).
- `DestroyLight` only queues an erase (`m_pendingExternalLightErases`). The queue is applied in
  `prepareSceneData` at the start of the next scene frame.
- The handle is the light's hash (`reinterpret_cast<remixapi_LightHandle>(info->hash)`), which
  is the same for every sun.

When no scene frame ran between leaving and returning, the queued erase landed after the new
create and removed the sun. The proxy still held a live handle and kept drawing it, so the
sun stayed gone until it was toggled. The headlights recover on their own because they are
described again every frame.

The fix: the sun is destroyed only when it is switched off. Outside a race it is simply not
drawn, and Remix shows an API light only in the frames where it is drawn. The sun is also now
`isDynamic`: the same code stops applying updates to a static light after
`numFramesToKeepLights / 2` (50) of them, which dragging a menu slider exceeds.

## 44. One proxy for NVIDIA's RTX Remix and Remix Plus (2026-09-24)

The bridge client's `remixapi_InitializeLibrary` never checks the requested API version. It
returns success and copies its own function table into the caller's struct, in its own
layout. NVIDIA's runtime (API 0.6) and the Remix Plus lines insert entries in the middle of
the table. Read through the wrong layout, every call lands in another function. On
NVIDIA's runtime, our `CreateLight` would run `DestroyLight` with one argument too many,
which unbalances the __stdcall stack and crashes the game. GTA2's renderer solved the same
problem (`gta2_dx9/renderer/dll/src/remix_api.cpp`), and this is its approach.

`remix_api::initialize` receives the table into a struct padded with 64 spare slots. It
then identifies the runtime from which of slots 1..13 are filled; slot 0 (Shutdown) is
null in all of them:

| Runtime | Filled slots | CreateLight | DestroyLight | DrawLightInstance | SetConfigVariable | Evidence |
|---|---|---|---|---|---|---|
| Remix Plus 1.5+, API 0.1000 (deps/bridge_api) | 1 2 3 5 7 8 10 11 12 | 8 | 10 | 11 | 12 | disassembly of the installed d3d9_remix.dll (0x1005E470) |
| NVIDIA RTX Remix, API 0.6 | 1 2 3 4 6 7 8 9 10 11 12 | 7 | 8 | 9 | 10 | dxvk-remix bridge/src/client/remix_api.cpp:441-452 |
| Remix Plus 1.4, API 0.6.3 | 1 2 3 5 8 9 11 12 13 | 9 | 11 | 12 | 13 | GTA2 renderer, tested there |

On the native layout, `m_bridge` is the whole table. On the others it holds only the four
entry points the proxy calls, and everything else is null. An unrecognised table stops
initialization and logs the mask it found, so the headlights and the sun stay off. The API
being switched off in bridge.conf (`REMIXAPI_ERROR_CODE_NOT_INITIALIZED`) now gets its own
log line and is not retried.

The structs we pass are compatible across all three runtimes:
- `remixapi_LightInfo` in 0.1000 appends only `isDynamic` and `ignoreViewModel`, which an
  0.6 bridge does not serialise.
- The sphere and distant extensions, the light shaping struct and the sType values (6, 7,
  11) are identical.
- The fog keys `rtx.volumetrics.transmittanceColor` and `rtx.volumetrics.singleScatteringAlbedo`
  exist in NVIDIA's runtime too.

The runtime's name appears in the log, and in the Headlights and Sun tabs. Tested on Remix
Plus only so far.

## 45. Funk texturebits: which frame is which light state (2026-09-25)

Static analysis of CARMA2_HW0.EXE (Ghidra project + capstone). Not checked in game.

**Parser.** `AddFunkotronics` (0x00474AC0; Funkgroo.c) reads each speed-control keyword from
the 7-entry table at 0x00655B80: linear 0, harmonic 1, flash 2, controlled 3, absolute 4,
continuous 5, **texturebits 6**. The texture section uses the tables at 0x00655B68 (frames 0,
flic 1, camera 2, mirror 3) and 0x00655B78 (approximate 0, accurate 1). For texturebits the
next line is read as a string, and the parser allocates a 0x28-byte bit spec (for example
0x00474FA2 for the matrix section and 0x00475807 for the texture section):

- `+0x00` byte: letter count.
- `+0x01..` byte per letter: the letter's index in the string "THBVLRF" at 0x00655D68.
  The lookup is case-sensitive. An unknown letter leaves its byte unset.
- `+0x24`: the car. The parser copies g_car_being_loaded (0x0074B584, set by LoadCar at
  0x00488FA2), so each car's funk reads its own car.

The funk slot (stride 0x158, array 0x0068B84C) stores the texture mode at +0x50, the time
mode at +0x54, the speed mode at +0x5C, the spec pointer at +0x60, the frame count at +0x64
and the current frame at +0x68. The frame pixelmaps start at +0x70.

**Frame index.** `FunkThoseTronics` (0x00477230, called from the main loop at 0x004930C2),
texture mode frames, time mode accurate (+0x54 == 1), speed mode 6, at 0x00478808..0x0047885E:

```
bits = spec->car->light_bits;            // car + 0x18CC
frame = 0;
for (i = 0; i < spec->count; i++)
    if (bits & (1 << spec->letter[i])) frame += 1 << i;
funk->current_frame = frame;             // +0x68, then colour_map = frames[frame]
```

The first letter is bit 0 of the frame index. For "VB", frame = V + 2*B. The code does not
check the index against the frame count. The same decode is used for matrix and lighting
funks (0x00477595 ...) and for groovidelics (0x00479238, 0x004799E7 ...). In approximate
mode the value at +0x60 is treated as a period float, so texturebits only works with
`accurate`, as every car TXT uses it.

**Light bits.** `UpdateCarLightBits` (0x0041E5A0) is called from the main loop at 0x00492F37,
before the funk update in the same frame. For every car in g_active_car_list it does this:

```
if (car->car_master_actor->render_style == 1 /*hidden*/ || DAT_00676914) skip;  // bits keep last value
car->light_bits = 0;
if ((car->keys & 0x100000) ||                                   // keys.brake (C1: handbrake key)
    (car->brake_force != 0.0f && fabs(phys->vcs.z) > 1/13800.0))  // 0x12C0; phys = car+0x08, +0x1B0
    car->light_bits = 4;                                         // bit 2 = 'B'
if (car->gear < 0 ||                                             // 0x135C
    ((car == NULL || car->driver != 8) && phys->vcs.z > 0.0f))   // non-local-human car rolling backwards
    car->light_bits |= 8;                                        // bit 3 = 'V'
```

The only other writer of +0x18CC is the network car-state unpack (0x004C96AF), which copies
the sender's bits. The sender packs them at 0x004C6690. No code ever sets bits 0, 1, 4, 5
or 6.

| Letter | Bit | Meaning | Status |
|---|---|---|---|
| T | 0 | unknown, never set | always 0 |
| H | 1 | unknown (headlights?), never set | always 0 |
| **B** | 2 | brake lights: handbrake key, or brake_force non-zero while moving | proven |
| **V** | 3 | reverse lights: gear < 0 (any car), or rolling backwards (AI and net cars only) | proven |
| L | 4 | unknown (left indicator?), never set | always 0 |
| R | 5 | unknown (right indicator?), never set | always 0 |
| F | 6 | unknown, never set | always 0 |

"VB" (EAGLE3 EARLITL/EARLITR: EBACKALL,2,x,2,y):

| frame | V | B | state | atlas cell (x,y) |
|---|---|---|---|---|
| 0 | 0 | 0 | lights off | (0,0) |
| 1 | 1 | 0 | reverse only | (1,0) |
| 2 | 0 | 1 | brake only | (0,1) |
| 3 | 1 | 1 | brake + reverse | (1,1) |

"B" gives frame 0 when the brake light is off and frame 1 when it is on.

Proven from code:
- The letter table and its order.
- The bit order: the first letter is bit 0.
- The frame formula.
- Only bits 2 (B) and 3 (V) are ever set, and the exact conditions that set them.
- +0x18CC is sent over the network.

Inferred:
- Field names come from matching layouts in C1 (dethrace).
  - 0x12C0 is brake_force: 0x00415618 sets it from initial_brake (0x12AC) and
    brake_increase (0x12B0) on keys.dec, like C1 `ControlCar`.
  - 0x12D0 is tCar_controls keys: the brake bit is 0x100000, dec 0x80000, acc 0x40000.
  - 0x135C is gear: the net unpack stores `(packed >> 12) - 1`.
- Physics object +0x1A8 is velocity_car_space. At 0x004B7A67 it is transformed into +0x68 by
  BrMatrix34ApplyV (0x00533520). A car faces -Z, so z > 0 means moving backwards (C1 uses
  the same test).
- DAT_00676914 is gAction_replay_mode. FunkThoseTronics pairs it with 0x00402360
  (ReplayIsPaused) the way C1 does.
- The meanings of T, H, L, R and F are guesses from the letters (headlights, indicators and
  so on). They have no effect in this build.

The earlier kb.h note listed EBACKALL's quadrants as "off / brake / reverse / both". The
frame order is actually off, reverse, brake, both.

## 46. Funk atlas frames at race start (2026-09-25)

Static analysis only (Ghidra program `CARMA2_HW.EXE`, capstone on `CARMA2_HW0.EXE`, car data
extracted from `DATA/CARS/EAGLE3.TWT`). Not checked in game. Question: why the Eagle's rear
light panels show the whole `EBACKALL` atlas for the first frames of a race.

**Where the frame matrices live.** The texture section of a funk slot:

| offset | field |
|---|---|
| +0x58 | last frame-change time (approximate mode only) |
| +0x64 | frame count (at most 8: the pixelmap array ends where the matrix table starts) |
| +0x68 | current frame, set to 0 by the parser |
| +0x6C | 1 if any frame line has sub-rectangle numbers, else 0 |
| +0x70 | `br_pixelmap *frame_map[8]` |
| +0x90 | `br_matrix23 frame_xform[8]`, stride 0x18 |

Parser, frames loop at 0x004758C9..0x00475A1A (inside `AddFunkotronics`). Each line is split
with `strtok(line, "\t ,/")` (0x0058F41C). Then:

- `frame_map[i] = BrMapFind(name)` (0x0051EFF0). A missing map is fatal error 0x42.
- `BrMatrix23Identity(&frame_xform[i])` (0x00534BA0, called at 0x00475911).
- If a non-empty token follows, +0x6C is set to 1 and the next four tokens are read with
  `sscanf("%d")` (0x0058F418) as xdiv, xidx, ydiv, yidx, in that order.
  The matrix is written at 0x004759C7..0x004759FD:

```
m[0][0] = 1.0 / xdiv      m[0][1] = 0     (left over from the identity)
m[1][0] = 0               m[1][1] = 1.0 / ydiv
m[2][0] = xidx / xdiv     m[2][1] = yidx / ydiv
```

For `EBACKALL,2,x,2,y` this gives scale (0.5, 0.5) and translation (x/2, y/2).

**The parser never touches the material.** For texture mode `frames`, `AddFunkotronics` does
not write `colour_map` or `map_transform`. Only the `flic` branch sets `colour_map` and calls
`BrMaterialUpdate(mat, 0x7FFF)`. So until the first `FunkThoseTronics`, the material keeps what
`Eagle3.mat` gave it:
- `EARLITL`/`EARLITR`: `colour_map` = **`ebacklig`**. In `PIXIES.P16` this is a 4x4 red
  placeholder, not the atlas. The car's README calls it a placeholder, and EAGLE3.WAM names it
  as the "pixelmap to use when intact".
- `map_transform` = identity.

**How the per-frame update applies it.** This is in `FunkThoseTronics` 0x00477230, texture
mode 0. The kb.h entry "FunkApplyMapTransform 0x00478930" was not a function: 0x00478930 is
in the middle of an instruction in this block, and nothing references it.

```
if (!(replay_mode && ReplayIsPaused() && trigger != 3 && trigger != 4)) {    // 0x004786xx
    if (time_mode == accurate)            // texturebits: +0x68 = frame from car->light_bits,
        funk->frame = ...;                //   recomputed every call (0x00478808..0x0047885E)
    else if (now - funk->last >= period)  // approximate: 0x004788DA..0x00478902
        { funk->last = now; if (++funk->frame >= count) funk->frame = 0; }
    flags = 0;                                                   // 0x00478905
    if (mat->colour_map != frame_map[frame])                     // 0x0047890D
        { mat->colour_map = mat->[0x94] = frame_map[frame]; flags = 8; }
    if (funk->has_xforms /*+0x6C*/) {                            // 0x00478923
        m = &frame_xform[frame];              // esi + (frame+6)*0x18
        if (m00 != mat.m00 || m11 != mat.m11 || m20 != mat.m20 || m21 != mat.m21)  // m01/m10 not compared
            { BrMatrix23Copy(&mat->map_transform, m); flags |= 1; }             // 0x0047896F
    }
    if (flags) BrMaterialUpdate(mat, flags);                     // 0x00478987
}
```

So the transform is applied **unconditionally every call**. It compares against the
material's current values, not against the previous frame index. Frame 0 (lights off) is
written on the very first call, because 0.5 != 1.0. Accurate/texturebits has no time guard
and no first-frame early-out. `colour_map` and `map_transform` change in the same call, so
there is no moment where the material has `colour_map = EBACKALL` with an identity transform.

Before the texture section, the slot is skipped only in these cases:
- `owner == -999`.
- **`+0x04 != 0`**. This is a disable-flags word:
  - bit 0 is set by 0x0047B250, called from the smash-damage texture swap 0x004ED2B0, and
    cleared by 0x0047B280 when `SetSmashLevel` 0x004EF840 repairs to level 0;
  - bit 1 is set by 0x0047B2B0 and cleared by 0x0047B2E0, from net-game / powerup code
    (0x004F8CA0, 0x004F9020, 0x004FE360, the last gated by `[0x0074D3DC]`).
  The parser clears only bit 0.
- Trigger mode `+0x0C` is distance (1) with no visible proximity triangle, or is lap-gated
  (2, 3). The Eagle funks use `constant` (0).

None of these is set for a car in a single-player race at the start.

**Order in the race loop.** `MainGameLoop` entry is **0x00492950**. 0x00492980, the kb.h
address, is the body after a `jmp` over a small prologue; `DoGame` 0x00503C50 calls 0x00492950
at 0x00503FD1. Every iteration runs, in straight-line code:

```
0x00492F37  UpdateCarLightBits      (inside if (!replay_mode))
0x004930C2  FunkThoseTronics        (unconditional)
0x00493A3F  0x004ECFB0 smash-damage queue    0x00493A44  0x004F00F0 repair tick
0x00493AEA  RenderAFrame            (only when [0x0075B8F0] == 0 or its deadline has passed)
```

`[0x0075B8F0]` is a render-start delay. Before the loop it is converted to an absolute time
(`+= GetTotalTime()`). While it is pending, the loop runs `FunkThoseTronics` without
rendering. The funk update therefore always runs before the render in the same iteration.

**No race-world render before the loop.** The direct callers of `RenderAFrame` are:
- the loop (0x00493AEA);
- replay code (0x004E69B0, 0x004E72E0, and 0x004E6FF0, which only renders when the replay
  flag is set);
- the movie-capture helper 0x004E1A20.

`RenderView`/`RenderScene` are only called from `RenderAFrame`. The other `BrZbSceneRender`
callers render their own worlds: the frontend 0x0046D8E0 / 0x00472B00, net HUD 0x00499A00,
HUD 0x0047CAD0 / 0x0044BAC0, and the tint poly 0x004D8290. A depth-5 call-graph walk found
no path to any scene render from these starting points:
- the pre-loop part of `MainGameLoop` (0x004A57E0, 0x00413780, 0x004A7A60, 0x0044C850,
  0x00492680, 0x004940E0, 0x00504300, 0x0047B880, 0x004B5330 ...);
- the race set-up in `DoGame` before 0x00503FD1 (0x00481830, 0x004E2B70, 0x004148D0,
  0x004010B0, 0x00414410, 0x00455BB0, 0x004C6580, 0x004EA770, 0x004EA840, 0x004E3410).

`0x0047B880` only clears the screen and back buffer and swaps.

**Other writers of these materials.**
- `0x004ED2B0` (smash damage level up): sets `colour_map`/+0x94 to the damage pixelmap,
  resets `map_transform` to identity or a random flip, calls `BrMaterialUpdate(0x7FFF)`, and
  disables the funk (bit 0). A damaged light therefore shows its whole damage texture. That is
  the intended look for that texture, not the atlas.
- `0x004EF840` (repair a level): restores the level's pixelmap (`ebacklig` at level 0) and
  re-enables the funk only at level 0. It is called from the repair tick 0x004F00F0 after the
  funk update, so for one rendered frame after a full repair the panel shows `ebacklig`
  through the old damage transform. Both only run for records with damage level +0x4C != 0.
  Nothing calls them at race start.
- The other `BrMatrix23Identity` callers (0x004A6A10, 0x004CB1E0, 0x004EED70) initialise
  unrelated materials: smears, slicks, shrapnel.

**Conclusion.**
- **Proven:**
  - The frame matrix formula and where it is stored.
  - The unconditional compare-and-copy apply, including frame 0 on the first call.
  - `FunkThoseTronics` runs before `RenderAFrame` in every loop iteration.
  - Nothing renders the race world before the loop starts.
- **Inferred:** The unmodified game never presents the Eagle's rear lights with an identity
  `map_transform` at race start. Their material's pre-funk state would not even show the
  atlas: it is the 4x4 `ebacklig` placeholder.
- **Consequence:** The "whole atlas for the first frames" is not the game's state. The proxy
  or Remix must be drawing those panels either with material state from another moment, or
  without the texture-stage transform, for its first frames. In the game, the combination
  `colour_map == EBACKALL` with an identity transform never exists at any render.

### 46.1 The cause is the static-world baker

The game never shows the atlas, but the proxy's baker does:
- An actor that keeps the same placement for `STATIC_PROMOTE_SIGHTINGS` (3) scenes is baked
  into a chunk. Cars standing on the grid through the countdown qualify.
- Chunks are drawn with `D3DTSS_TEXTURETRANSFORMFLAGS = D3DTTFF_DISABLE`. A baked rear light
  therefore shows the whole atlas.
- `bake_actor` did refuse animated materials, but it learned that a material is animated only
  by `on_material_update` seeing its UV transform change in two different scenes.
  `FunkThoseTronics` writes the transform only when it differs from the material's (6.4, 46),
  so a light that stays off through the countdown reports no change.
- The light returned to the dynamic path only when the car moved (placement change) or the
  lights first changed state (the second UV update). That is the brief full-atlas frame at
  the start of a race.

Fix: `bake_actor` also refuses any material that has a live funk slot. It reads
`g_funk_slots` at 0x0068B84C, which is a pointer to the slot array and is null before a load
(FunkThoseTronics and DisposeFunkotronics test it). The count is at 0x0068B844 and the stride
is 0x158. A slot is live when its owner at +0x00 is not -999; its material is at +0x08.
Funk-animated materials (car lights, scrolling and flashing track textures, sign frames) are
now dynamic from their first sighting, instead of after their second visible change.
`m_animated_materials` still catches what the funk table does not declare: opacity fades,
and damage re-mapping a car panel (0x004ED2B0).

Reported working in game: the rear lights show one cell from the first frame.

## 47. Pickups are culled again (2026-09-25)

Shove Thy Neighbor (`JUNKYARD2.TXT`) ran at 33-55 fps, where the city race in the same session
ran at 60-107 fps. The scene stats pointed at the "vanishing" dynamic reason: 830-930 actors
on the junkyard against ~450 in the city, and 3,000-4,000 draws against ~1,800-2,000. The
track's actor file has 869 pickups (`&74POWERUPB.ACT` alone 388 times); newcity1 has 491.
Pickups spin, so they can never bake. With `DisableFrustum`, every one of them was a draw of
its own each frame, on screen or not.

`[Culling] CullPickups=1` (default on) leaves the game's own bounds-test verdict in place for
pickups. BrZbActorRender (0x005221E0) calls the renderer's boundsTest (vtable +0xD0) with
`&model->bounds_min` (model + 0x34) for a model actor (0x005222E5, 0x00522496), and with
`actor->type_data` for a bounds actor. So `hk_bounds_test` recovers the model from the pointer
and compares it with `m_pickup_models`: the models captured for actors whose identifier
passes the game's pickup test (0xA3 as the second character). The comparison is by pointer
only, and nothing is dereferenced. A pickup is always drawn on its first frame, because
culling is off until its model has been seen. The set is cleared with the rest of the
track state.

Reported working in game: close to 60 fps. The "vanishing" count in the scene stats should fall to roughly the
pickups in view.

## 48. The headlight key moves from H to F (2026-09-25)

A key-map file (`DATA/DKEYMAP0..3.TXT` for the defaults, `Keymap_0..3.txt` for the player's
own) holds one key code per action slot. A code is its line number in `DATA/KEYNAMES.TXT`
minus 2; that offset makes every known default land where it should (Recover Insert, Repair
Backspace, Hand Brake Space, Map Tab, Cockpit View C, Buy Delete/End/PgDn). The control names
come from the executable's name-to-slot table at 0x00596250 (8-byte `{char* name, int slot}`
entries).

- **H** is slot 59, **Horn**, in all four layouts, so the old headlight key sounded the horn.
- **F** is unbound in layouts 0, 2 and 3. Layout 1 (the one that drives with A/Alt and steers
  with comma/period) uses it for slot 52. Slots 51-53 are Q/W/E in the default layout, so slot
  52 is look forward.

The toggle (`imgui::input_message`) now listens for F.

## 49. Key map in memory (2026-09-25)

Static analysis of CARMA2_HW0.EXE. It is the same image as the Ghidra project's CARMA2_HW.EXE
(bytes compared). The function names are ours. The C1 equivalents come from dethrace.

### The array and its loaders

- **`g_key_mapping` = `int[77]` at 0x0074B5E0..0x0074B714**: one key code per slot. A code is
  its KEYNAMES.TXT line number minus 2. The names array at 0x00688458 is indexed as
  `names[code + 2]`, and code reads `0x00688460 + code*4` directly. -2 means unbound.
- **LoadKeyMapping 0x00487E10** builds `<data>\KEYMAP_X.TXT` with X = `g_key_map_index`
  (0x0068B88C). That index is `KeyMapIndex` in OPTIONS.TXT, read just before by LoadOptions
  (0x0048D8F0). The loader then calls `fscanf("%d")` 77 times. Its only caller is
  InitialiseWorld (0x0047DD20) at **0x0047DE1A**, once at startup. Nothing reloads the map per
  race.
- The only other live code that writes the array is the front-end **Options > Controls** screen.
  Its descriptor is at 0x00604940, and its start and end callback pointers are at 0x00604A48 and
  **0x00604A4C**. CreateMenu calls the start callback at 0x0046C9B0 and DestroyMenu calls the end
  callback at 0x0046CCE8, both with ecx = descriptor. DestroyMenu always calls the end callback.
  - ControlsScreenStart (0x004725F0) loads KEYNAMES.TXT. It then **reloads all four
    KEYMAP_N.TXT files from disk** into the array and leaves the current layout there. It also
    copies them to 0x006869E0 [4][77], which nothing reads again. Anything written to the array
    before the screen opens is discarded.
  - ControlsScreenSwitchLayout (0x00472440, layout buttons 0x27..0x2A) **writes** the array to
    KEYMAP_<old>.TXT. It then sets `g_key_map_index` and reads KEYMAP_<new>.TXT.
  - ControlsScreenAssignKey (0x00472B00) stores the new code at 0x00472D5C.
  - ControlsScreenEnd (0x00472A30) **writes** the array to KEYMAP_<index>.TXT as `"%d\r\n"` per
    slot and returns 1. This is the only save.
- DKEYMAP0..3.TXT are read only by a second controls screen that uses C1 slot numbers
  (0x004B28C0..0x004B3C61; driver 0x004B39F0, descriptor 0x0065CA90, list 0x0065BD30 = 48, 49,
  46, 47, ...). No call, jump or pointer anywhere in the image refers to 0x004B39F0, so that
  screen and its "defaults" button are dead. The KeyIsDown variant at 0x00483970 is also dead.

### How a slot becomes a key test

- `g_key_array` = `int[151]` at 0x0068BEE0, indexed by key code. An entry is nonzero while its
  key is down.
- KeyIsDown (0x004833A0, `__fastcall`, ecx = slot) returns
  `g_key_array[g_key_mapping[slot]]`. KeyIsDownPoll (0x00483040) does the same but polls again
  first if the last poll is more than 500 ms old. It also takes -1 (any key from the list at
  0x00657200) and -2 (always 1).
- These tests bypass the map: RawKeyIsDown (0x00482550) and 0x00482590 take ecx = key code, and
  GetPressedKeyCode (0x00482A00) returns the code of the key being pressed.
- PDSetKeyArray (0x0051CEF0) reads the keyboard with `IDirectInputDevice::GetDeviceState(256)`.
  It sets `g_key_array[code]` from `state[g_dik_for_code[code]]`. `g_dik_for_code` is an
  `int[151]` at 0x006B34A0, filled with constants by 0x0051BA10.
  - Arrows use the E0-extended DIK codes: crsr-left 70 -> 0xCB, right 71 -> 0xCD, up 72 -> 0xC8,
    down 73 -> 0xD0.
  - Keypad 2/4/6/8 (codes 83/85/87/89) -> 0x50/0x4B/0x4D/0x48.
  - The arrows and the keypad are therefore **separate entries**, whatever the NumLock state,
    because DirectInput reports physical keys.
  - Codes 0..3 (Shift/Alt/Control/Command) map to 0xFF, meaning either side.

### What the slots do

**Slots 47..50 (proven from KeyIsDown call sites with a constant ecx):**
- PollCarControls (0x00443E80) fills `g_player_car.keys` (0x0075CEFC = 0x0075BC2C + 0x12D0):
  - **47 left 0x10000, 48 right 0x20000, 49 accelerate 0x40000, 50 brake/reverse 0x80000.**
  - 54 hand brake 0x100000, 56 wheel spin 0x4000000, 59 horn 0x8000000, 13 0x800000.
  - 14 and 12 give 0x200000 and 0x400000, only when 0x0068B910 is set. In C1's layout these bits
    are gear up and gear down.
- Keyboard steering is used only when the codes in slots 47 and 48 are both below 0x8F.
  Otherwise they are joystick axes. PollCarControls and 0x00418850 (at 0x004197F4) read
  0x0074B69C and 0x0074B6A0 directly for this check.
- When 0x00705BE0 is set, left/right and accelerate/brake are swapped.

**Slots 31..34 = up, down, left, right (proven from every constant-ecx call site):**
- PackCameraKeys (0x00444270, called from MainGameLoop at 0x00493073):
  - If raw Shift and raw Alt are both up, slots 31..34 go to bits 1..4 of `g_camera_keys`
    (0x0079EFA4), and raw Control goes to bit 0.
  - With Shift held it calls MoveHeadupMap (0x00497620) instead: **Shift + up/down/left/right
    moves the heads-up mini-map**. The position is 0x0074ABD8/DC, saved as HeadupMapX/Y by
    SaveOptions (0x0048D190).
  - With Alt held, the arrows do nothing.
- UpdateCamera (0x0040EA30) switches on `g_camera_mode` (0x0079EFA8, 0..8, cycled by the Camera
  Mode toggle):
  - Modes 0, 4 and 7 (and the target cameras 5 and 6) use 0x00410C60, C1's PollCameraControls.
    **Up and down zoom the external chase camera in and out** (0x00655F40, 0.1..2.0). **Left and
    right orbit it around the car** (yaw at 0x0068B908). Both together put it back behind the car.
    It does nothing in map mode.
  - Mode 3 uses 0x0040EF90. Left and right orbit the camera, and both together recentre it. Up
    and down move it closer or further away, and Ctrl + up/down raise or lower it.
  - Mode 8 uses 0x0040F590. Up and down slide the camera between two positions stored in the car
    (+0x18D8 and +0x18E4).
  - So the arrows move the **external view**. Cockpit look is slots 51..53 (Q/W/E).
- 0x00442F90, at the end of CheckToggles (0x00442E90): in map mode (0x0075B9A4 == 2), slots
  31..34 **pan the map** (0x00659B30 y, 0x00659B2C x). This is C1's CheckMapRenderMove.

The toggle table at 0x005900A0 uses none of slots 31..34 or 47..50. It holds 44 entries of 24
bytes, `{slot, modifier slot, ..., last, func}`.

### The Controls screen refuses keys held by fixed slots (proven)

The screen shows the 29 slots listed at 0x00604888: 49 50 47 48 54 45 60 58 56 46 57 67 68 69 71
61 62 63 64 74 59 70 72 73 75 76 35 65 66.

When a key is pressed, ControlsScreenAssignKey scans slots **28..76** for another slot with the
same code:
- If that slot is in the list, it is set to -2. ControlsScreenCheckUnbound (0x00472D80) then makes
  the player rebind it before leaving.
- If that slot is **not** in the list (31..34 are not), a sound plays (0x00455690) and the key is
  refused.

Slots 31..34 hold the arrows in every layout, so **the game's own screen can never give the arrows
to steering, throttle or brake**. Slots 0..27 are not checked.

The {name, slot} table at 0x00596250 does not belong to this screen. It feeds the joystick button
setup (0x0045B790, 0x0045C240, 0x0045C590).

### Raw arrow and keypad reads that bypass the map

I checked the 230 call sites of the raw tests that pass a constant key code. Every one that tests
an arrow is menu code:
- the interface loop 0x004846E0
- the front-end handler 0x00470C20
- 0x0046C0D0 and its neighbours
- code around 0x004739xx

Each of them also tests the matching keypad key: up 0x48 with KP8 0x59, down 0x49 with KP2 0x53,
left 0x46 with KP4 0x55, right 0x47 with KP6 0x57. No in-race code reads the arrows raw.

Two in-race readers take typed key codes raw:
- The action replay (0x004E69B0) runs from MainGameLoop only while 0x00676914 is set. It uses:
  - KP4 or PgUp to rewind, KP6 or PgDn to fast-forward
  - KP5 or Space
  - KP0, KP1, KP3, KP7, Keypad / and Keypad *
- Chat entry (0x00444910, net games) compares typed keys with slot 71.

### Keypad 8/2/4/6 in the shipped layouts

Only slots 47..50 use them, as Keypad 4, 6, 8 and 2, in DKEYMAP0/2/3 and Keymap_2/3.txt. Layout 1
uses none of them.

Other keypad keys: slot 10 KP0, 11 KP1, 12 KP3, 13 KP5, 14 KP9, 55 Keypad -, 75 Keypad *.

On this machine, Keymap_0.txt (the active layout, KeyMapIndex 0) has x, v, d, c in slots 47..50.

### For the arrows-drive option in the proxy

- **Where to apply the rewrite.** Apply it at two points:
  - After LoadKeyMapping returns: retarget the call at 0x0047DE1A, or apply once when the proxy
    starts if that is later.
  - After ControlsScreenEnd returns: replace the pointer at 0x00604A4C with a `__fastcall`
    wrapper that calls 0x00472A30 and then rewrites.

  Nothing else reloads the map, so no per-frame work is needed.
- **Never modify the array while the Controls screen is open.** ControlsScreenSwitchLayout and
  ControlsScreenEnd write it to KEYMAP_N.TXT, so a per-frame rewriter would leak into the file. A
  rewrite made before the screen opens is harmless, because ControlsScreenStart reloads the files.
- **Make the rewrite a fixed assignment, not a swap.** Give every slot holding an arrow code the
  keypad code for that direction, then set slots 47..50 to 70, 71, 72, 73. A swap applied to a
  file that was already saved swapped would undo itself.
- **One key per slot.** Whatever the player had on 47..50 (the keypad, or x/v/d/c here) stops
  working.
- **What the screen shows.** While the Controls screen is open it shows the bindings from the
  file, and it still refuses the arrows for driving.
- **Action replay.** In a replay, KP4/KP6 would both rewind/fast-forward and orbit the camera.

### 49.1 The arrow-key driving option

`[Controls] ArrowKeyDriving=1` (default on; `src/comp/game/controls.cpp`) rewrites
`g_key_mapping` in memory after `LoadKeyMapping` returns (MinHook detour on 0x00487E10). It
does the same after the Controls screen closes: the end-callback pointer at 0x00604A4C, checked
to still hold 0x00472A30, is replaced by a `__fastcall` wrapper. The rewrite is also applied
once at install, in case the map was already loaded.

The rewrite is a fixed assignment:
- Every slot outside 47..50 that holds an arrow gets the matching numpad key: up to KP8 (89),
  down to KP2 (83), left to KP4 (85), right to KP6 (87).
- Slots 47..50 then get left 70, right 71, up 72 and down 73.
- A driving slot that holds a joystick code (107 and up, "Joy 1 B1" onward) is left alone.

The array is never touched while the Controls screen is open. The screen reloads the
KEYMAP_N.TXT files when it opens and saves the array when it closes, so the files, and what
the screen shows, keep the player's own bindings. Turning the option off restores them.

Known side effect: action replay reads KP4/KP6 directly for rewind and fast-forward, so in a
replay those keys also circle the camera.

Reported working in game.
