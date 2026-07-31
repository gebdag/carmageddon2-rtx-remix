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
