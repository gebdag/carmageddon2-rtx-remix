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
