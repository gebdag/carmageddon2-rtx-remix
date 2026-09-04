/* Carmageddon 2: Carpocalypse Now (GOG) — knowledge base
 *
 * Binary:  CARMA2_HW.EXE  (x86 PE, image base 0x00400000, 3734 functions)
 * Engine:  BRender 1.3.x (Argonaut) — statically linked into the EXE.
 *          The Glide device driver is 3dfx_win.bdd, mapped by BRender's own PE loader
 *          (FUN_00530E70, via BrDLLLoad 0x0052FFA0) -- its BRCORE1/BRHOST1/BRPMAP1 imports
 *          resolve against the statically linked BRender; only glide2x.dll (nGlide) goes
 *          through the OS loader. The EXE itself has no glide2x import.
 *
 * Renderer selection (from carma2.exe launcher, reg key Software\SCI\Carmageddon2):
 *          carma2_hw.exe                 -> 3dfx / Glide  (glide2x.dll, nGlide on GOG)
 *          carma2_hw.exe -d3d            -> Direct3D (DirectDraw + D3D IM, HAL/RGB/Ramp)
 *          extra flags: -afe  -noblend  -zombie
 */

/* ---------------------------------------------------------------- BRender types */

struct br_vector3 { float x, y, z; };
struct br_vector2 { float u, v; };

/* 3x4 row-major affine transform; rows 0..2 are the basis, row 3 is translation. */
struct br_matrix34 { float m[4][3]; };          /* 48 bytes */
struct br_matrix4  { float m[4][4]; };          /* 64 bytes */

/* Stride confirmed 0x28 from BrModelUpdate's radius loop (pfVar14 += 10 floats). */
struct br_vertex {
    struct br_vector3 p;        /* 0x00  MODEL-SPACE position          */
    struct br_vector2 map;      /* 0x0C  texture coordinates           */
    unsigned char index;        /* 0x14                                */
    unsigned char red;          /* 0x15                                */
    unsigned char green;        /* 0x16                                */
    unsigned char blue;         /* 0x17                                */
    unsigned int  reserved;     /* 0x18                                */
    struct br_vector3 n;        /* 0x1C  vertex normal                 */
};                              /* 0x28 */

struct br_face {
    unsigned short vertices[3]; /* 0x00  indices into br_model.vertices */
    unsigned short smoothing;   /* 0x06                                 */
    void *material;             /* 0x08  br_material*                   */
    unsigned char index;        /* 0x0C                                 */
    unsigned char red;          /* 0x0D                                 */
    unsigned char green;        /* 0x0E                                 */
    unsigned char blue;         /* 0x0F                                 */
    unsigned char pad[8];       /* 0x10                                 */
    struct br_vector3 n;        /* 0x18  face normal                    */
    float d;                    /* 0x24  plane constant                 */
};                              /* 0x28 */

struct br_model {
    struct br_model *next;      /* 0x00 */
    char *identifier;           /* 0x04 */
    struct br_vertex *vertices; /* 0x08  UNTRANSFORMED source vertices */
    struct br_face *faces;      /* 0x0C */
    unsigned short nvertices;   /* 0x10 */
    unsigned short nfaces;      /* 0x12 */
    struct br_vector3 pivot;    /* 0x14 */
    unsigned short flags;       /* 0x20  0x0020 = has custom callback  */
    unsigned char  flags_hi;    /* 0x21 */
    void *custom;               /* 0x24  br_model_custom_cbfn*         */
    void *user;                 /* 0x28 */
    float radius;               /* 0x30 */
    struct br_vector3 bmin;     /* 0x34 */
    struct br_vector3 bmax;     /* 0x40 */
    void *prepared;             /* 0x4C  v1 prepared block (see below) */
    void *stored;               /* 0x50  br_geometry_stored*           */
};

/* model->prepared, built by BrModelUpdate. Geometry here is still MODEL SPACE. */
struct v1_prepared {
    unsigned int size;          /* 0x00 */
    unsigned short ngroups;     /* 0x08 */
    struct v1_group *groups;    /* 0x18 */
};

struct v1_group {               /* stride 0x24 */
    void *material;             /* 0x00 */
    void *face_colours;         /* 0x08 */
    void *face_vertex_indices;  /* 0x0C */
    struct v1_online_vertex *vertices; /* 0x10  MODEL-SPACE, pivot-relative */
    void *vertex_colours;       /* 0x14 */
    unsigned short *vertex_src; /* 0x18  index back into br_model.vertices */
    unsigned short nfaces;      /* 0x1C */
    unsigned short nvertices;   /* 0x1E */
};

/* The buffer the hardware geometry renderer walks. THIS is what RTX Remix wants. */
struct v1_online_vertex {
    struct br_vector3 p;        /* 0x00  model space, minus model->pivot */
    struct br_vector2 map;      /* 0x0C */
    struct br_vector3 n;        /* 0x14 */
};                              /* 0x20 */

/* 2x3 affine UV transform, row-vector: u' = u*m[0][0] + v*m[1][0] + m[2][0]. */
struct br_matrix23 { float m[3][2]; };          /* 24 bytes */

struct br_token_value { unsigned int token; unsigned int value; };

/* Offsets from MaterialNeedsAlpha (0x0051F630) and the funk material update (0x00478930).
 * Larger than stock BRender 1.3. */
struct br_material {
    struct br_material *next;       /* 0x00 */
    char *identifier;               /* 0x04 */
    unsigned int colour;            /* 0x08  0x00RRGGBB */
    unsigned char opacity;          /* 0x0C */
    float ka, kd, ks, power;        /* 0x10 0x14 0x18 0x1C */
    unsigned int flags;             /* 0x20  BR_MATF_* */
    struct br_matrix23 map_transform; /* 0x24  animated by the funkotronic system */
    unsigned char index_base;       /* 0x3C */
    unsigned char index_range;      /* 0x3D */
    unsigned short pad3E;
    struct br_pixelmap *colour_map; /* 0x40 */
    void *screendoor;               /* 0x44 */
    void *index_shade;              /* 0x48 */
    void *index_blend;              /* 0x4C  MaterialNeedsAlpha reads translucency off this */
    unsigned char pad50[0x08];
    struct br_token_value *extra;   /* 0x58  NOT always zero-terminated: 'Acc Poly Mat' at
                                     *       0x005962F8 runs into the next object. Bound any
                                     *       walk of it. */
    unsigned char pad5C[0x3C];
    unsigned int stored;            /* 0x98  driver-side material; v1_group[0x00] holds this */
};

/* Read off live textures 'ROKWATX64' (type 5) and 'WATER' (type 0x12), both 64x64.
 * row_bytes is width*2 for both; +0x08 is the pixel buffer, +0x40 a driver object. */
struct br_pixelmap {
    void *dispatch;         /* 0x00  points into .rdata */
    char *identifier;       /* 0x04 */
    void *pixels;           /* 0x08 */
    unsigned int id;        /* 0x0C */
    struct br_pixelmap *map;/* 0x10  palette for the INDEX_* types; 0x004FA340 hands
                             *       SMOKE.PIX the DRRENDER.PAL pixelmap (0x0074A674) */
    unsigned char pad14[0x14];
    unsigned int row_bytes; /* 0x28 */
    unsigned char type;     /* 0x2C  3=INDEX_8 4=RGB_555 5=RGB_565 6=RGB_888 8=RGBA_8888
                             *       0x12=RGBA_4444 (alpha in the high nibble) */
    unsigned char flags;    /* 0x2D */
    unsigned short pad2E;
    unsigned short base_x;  /* 0x30 */
    unsigned short base_y;  /* 0x32 */
    unsigned short width;   /* 0x34 */
    unsigned short height;  /* 0x36 */
    short origin_x;         /* 0x38 */
    short origin_y;         /* 0x3A */
    unsigned int pad3C;
    void *stored;           /* 0x40  driver-side pixelmap object */
};

struct br_camera {              /* actor->type_data, actor + 0x5C */
    char *identifier;           /* 0x00 */
    unsigned char type;         /* 0x04  1 = PERSPECTIVE_FOV, 3/0 = PARALLEL */
    unsigned short field_of_view; /* 0x06  br_angle */
    float hither_z;             /* 0x08 */
    float yon_z;                /* 0x0C */
    float aspect;               /* 0x10 */
    float width;                /* 0x14 */
    float height;               /* 0x18 */
};

struct br_actor {
    struct br_actor *next;      /* 0x00 */
    struct br_actor *children;  /* 0x08 */
    struct br_actor *parent;    /* 0x0C */
    unsigned char type;         /* 0x12  1 = MODEL, 5 = CAMERA, 6 = ?  */
    unsigned short depth;       /* 0x10 */
    char *identifier;           /* 0x14  '&' + '\xA3' + 2 digits = powerup pickup */
    void *model;                /* 0x18 */
    void *material;             /* 0x1C */
    unsigned char render_style; /* 0x20 */
    void *env_map;              /* 0x24 */
    unsigned short t_type;      /* 0x28  transform type */
    struct br_matrix34 t;       /* 0x2C  transform matrix */
    void *type_data;            /* 0x5C  br_camera* / br_light* */
};

/* -------------------------------------------------- renderer dispatch offsets */
enum br_renderer_dispatch {
    RD_partSet        = 0x80,
    RD_partQuery      = 0x84,
    RD_templateQuery  = 0x8C,   /* (self, BRT_MATRIX, 0, &out, buf, size, token) */
    RD_queryMany      = 0x90,
    RD_modelMul       = 0xA4,   /* (self, br_matrix34*) — push actor transform  */
    RD_modelIdentity  = 0xB4,
    RD_statePush      = 0xB8,
    RD_statePop       = 0xBC,
    RD_boundsTest     = 0xD0,   /* -> 0x113 outside / 0x114 inside / 0x115 cull */
    RD_flush          = 0xFC,
};

/* br_actor::render_style, and the style argument BrZbModelRender dispatches on. Both
 * recursive actor walkers (0x005221FC, 0x00522510) return early on NONE, so a hidden
 * actor never reaches BrZbModelRender at all. */
enum br_render_style {
    BR_RSTYLE_DEFAULT = 0,          /* resolves to FACES */
    BR_RSTYLE_NONE = 1,
    BR_RSTYLE_POINTS = 2,
    BR_RSTYLE_EDGES = 3,
    BR_RSTYLE_FACES = 4,
    BR_RSTYLE_BOUNDING_POINTS = 5,
    BR_RSTYLE_BOUNDING_EDGES = 6,
    BR_RSTYLE_BOUNDING_FACES = 7,
};

/* BrMaterialUpdate reads `flags` as a byte only; bits 8-15 of the 0x7FFF callers pass are
 * ignored. Branch sites: 0x00520EEA, 0x00520F0C, 0x005211CE, 0x005212DB, 0x0052136E,
 * 0x005213AF, 0x005213DD. */
enum br_material_update {
    BR_MATU_MAP_TRANSFORM = 0x0001, /* -> &material->map_transform, token 0xC8 */
    BR_MATU_MATERIAL      = 0x0002, /* -> colour/opacity/flags token list */
    BR_MATU_LIGHTING      = 0x0004,
    BR_MATU_COLOUR_MAP    = 0x0008,
    BR_MATU_EXTRA         = 0x0040, /* -> material->extra; how sprites animate opacity */
    BR_MATU_ALL = 0x7FFF,
};

/* The `flags` argument BrModelUpdate masks, read at 0x0051FAF7..0x0051FB1F. */
enum br_model_update {
    BR_MODU_VERTEX_POSITIONS = 0x0001,
    BR_MODU_VERTEX_COLOURS   = 0x0002, /* fills v1_group::vertex_colours @0x0051FB66 */
    BR_MODU_VERTEX_MAPPING   = 0x0004,
    BR_MODU_VERTEX_NORMALS   = 0x0008,
    BR_MODU_FACE_COLOURS     = 0x0020,
    BR_MODU_ALL = 0x7FFF,
};

/* br_material::flags, from the bit tests in BrMaterialUpdate's BR_MATU_MATERIAL branch. */
enum br_material_flags {
    BR_MATF_LIGHT   = 0x0001,
    BR_MATF_PRELIT  = 0x0002,   /* vertex colours are final; sprites carry their tint here */
    BR_MATF_SMOOTH  = 0x0004,
    BR_MATF_PERSPECTIVE = 0x0020,
    BR_MATF_DECAL   = 0x0040,
};

/* Resolved from the BRender token-name table (records of {char* name, ?, token, type},
 * 0x00668000..0x0066A400). Both spellings carry the same 0..255 opacity byte -- the fixed
 * form in its integer part -- and MaterialNeedsAlpha treats either as translucency. */
enum br_material_token {
    BRT_BLEND_B   = 0x0085,
    BRT_OPACITY_X = 0x00BE,
    BRT_OPACITY_F = 0x00BF,
};

enum br_matrix_token {
    BRT_MATRIX          = 0x76,
    BRT_MODEL_TO_VIEW   = 0xEA, /* br_matrix34, 48 bytes */
    BRT_VIEW_TO_SCREEN  = 0xEE, /* br_matrix4,  64 bytes — PROJECTION */
    BRT_CAMERA_TYPE     = 0xF0, /* 0xF1 perspective, 0xF2 parallel    */
};

/* ------------------------------------------------------------------ functions */

@ 0x00522f30 void BrZbSceneRender(br_actor *world, br_actor *camera, br_pixelmap *colour, br_pixelmap *depth);
@ 0x00522c80 void BrZbSceneBeginFrameSetup(br_actor *world, br_actor *camera, br_pixelmap *colour, br_pixelmap *depth);
@ 0x00521c10 void SceneSetupCameraMatrices(br_actor *world, br_actor *camera);
@ 0x0051e520 br_token BrCameraToScreenMatrix4(br_matrix4 *dest, br_actor *camera);
@ 0x005221e0 void BrZbActorRender(br_actor *actor, br_model *def_model, br_material *def_mat, br_actor *def_env, char style, unsigned int bounds);
@ 0x00521890 void BrZbModelRender(br_actor *actor, br_model *model, br_material *material, br_actor *env, int style, unsigned int bounds, int use_custom);
@ 0x00525fc0 void ModelRenderStyle_Faces(br_actor *actor, br_model *model, br_material *material, void *env, void *unused, br_token bounds_result);
@ 0x00526040 void ModelRenderStyle_Edges(br_actor *actor, br_model *model, br_material *material, void *env, void *unused, br_token bounds_result);
@ 0x00526090 void ModelRenderStyle_Points(br_actor *actor, br_model *model, br_material *material, void *env, void *unused, br_token bounds_result);
@ 0x005260e0 void ModelRenderStyle_None(void);   /* bare ret 0x18 */
@ 0x00520e70 void BrMaterialUpdate(br_material *material, unsigned short flags);
@ 0x00522ea0 void BrZbSceneRenderAdd(br_actor *actor);
@ 0x0051e070 void BrActorRemove(br_actor *actor);
@ 0x0051dfe0 void BrActorAdd(br_actor *parent, br_actor *actor);
@ 0x0051e1a0 br_actor *BrActorAllocate(unsigned char type, void *type_data);
@ 0x005217f0 br_model *BrModelAllocate(char *name, int nvertices, int nfaces);
@ 0x0051ed00 void BrModelAdd(br_model *model);
@ 0x0051f240 br_material *BrMaterialAllocate(char *name);
@ 0x0051ee50 void BrMaterialAdd(br_material *material);
@ 0x0051eea0 br_material *BrMaterialFind(char *name);
@ 0x00534ae0 void BrMatrix23Copy(br_matrix23 *dest, br_matrix23 *src);

/* --- Carmageddon 2 effect subsystems (see findings.md) --- */
@ 0x004f6b80 br_token DrawLine3D(br_vector3 *a /*ecx*/, br_vector3 *b /*edx*/, ...);  /* rewrites gLine_model, BrModelUpdate, BrZbSceneRenderAdd */
@ 0x0047e610 void InitLineAndSmokeStuff(void);       /* builds gLine_model / gLine_material / gLine_actor */
@ 0x004f7cb0 void SetLineColour(char white /*cl*/);  /* white: both verts ffffff. else v0=ff0000, v1=ffff00 (spark) */
@ 0x004e9c40 void InitSpillsAndSkids(void);          /* shadow materials + the 100-quad ground decal ring */
/* 0x004EA880 is InitSpriteParticlePool -- see section 25; it is the shared sprite
 * particle pool, not a decal pool. */
@ 0x00478930 void FunkApplyMapTransform(void);       /* copies a frame's br_matrix23 into material->map_transform */
@ 0x005259b0 int  BrRendererBegin(br_device *device, br_renderer *renderer);
@ 0x0051f950 void BrModelUpdate(br_model *model, unsigned short flags);
@ 0x00522eb0 void BrZbBucketFlushAndSwap(void);
@ 0x005226d0 void BrZbSceneAddActorIncremental(br_actor *world, br_actor *camera, br_pixelmap *colour, br_pixelmap *depth); /* a self-contained scene render (setup camera, walk children, flush), not an add; the HUD text/flush path */
@ 0x0051f630 int  __stdcall MaterialNeedsAlpha(br_material *material);  /* callee-cleans: both exits are `ret 4` */
@ 0x00531870 void BrTransformToMatrix34(br_matrix34 *dest, br_transform *t);
@ 0x00532620 void BrMatrix34Mul(br_matrix34 *dest, br_matrix34 *a, br_matrix34 *b);
@ 0x00527c80 void BrFatalOrWarning(const char *fmt, ...);

/* -------------------------------------------------------------------- globals */

$ 0x0079efec void*  g_pRenderer            /* br_renderer* */
$ 0x0079eff4 void*  g_pGeometryV1Model     /* br_geometry* BRT_GEOMETRY_V1_MODEL   (0x31) */
$ 0x0079eff8 void*  g_pGeometryV1Buckets   /* br_geometry* BRT_GEOMETRY_V1_BUCKETS (0x32) */
$ 0x0079effc void*  g_pGeometryPrimitives  /* br_geometry* BRT_GEOMETRY_PRIMITIVES (0x33) */
$ 0x0079f07c void*  g_camera_matrix_stack  /* stride 56; entry [0] = world_to_view br_matrix34 */
$ 0x0079f4dc void*  g_pColourBufferPixelmap
$ 0x00665090 void*  g_pfnModelRenderStyleTable /* 8 render-style thunks, indexed by style & 0xFF */
$ 0x0079efe8 int    g_render_mode          /* 2 = ?, 3 = scene render in progress */
$ 0x0079f074 int    g_bounds_state
$ 0x0074cac8 void*  g_line_model           /* br_model*, 2 verts / 1 face (0,0,1) */
$ 0x0074ca4c void*  g_line_material        /* br_material* "gLine_material", flags 0x1007 */
$ 0x0074ca34 void*  g_line_actor           /* br_actor*, render_style = BR_RSTYLE_EDGES */
$ 0x0074cf68 int    g_lines_as_3d_models   /* non-zero: lines go through gLine_model instead of a 2D blit */
$ 0x006a27f0 void*  g_ground_decal_ring    /* 100 entries, stride 0x1C, [0] = br_actor*; unit XZ quad at y=0 */
$ 0x006a27e8 short  g_ground_decal_next    /* ring index, wraps at 100 */
/* 0x006A55D8 is g_sprite_particles + 0x10 (the actor field), NOT a pool base. */

/* --- Powerup pickups (see findings.md section 10) ---
 * Pickups are track-hierarchy br_actors named "&\xA3NN..."; NN = index into the POWERUP.TXT
 * definition table. There is NO array of live pickups. Index 86/87 pickups get one of three
 * shared icon models below, cycled by PowerupModelCustomCB — that pointer triple is the
 * cheapest "is this a powerup?" test from a render hook. Collection sets
 * actor->render_style = BR_RSTYLE_NONE (0x004F4FEF); respawn sets it back to FACES
 * (0x004DB8A8). The actor is reused, never removed. */
@ 0x0040d1f0 void SpecialActorEnumCallback(br_actor *actor, void *ctx);   /* dispatches on "&\xA3NN" name */
@ 0x004df570 void __thiscall PowerupActorSetupSpin(br_actor *actor /*ecx*/, int colour /*edx*/); /* index 66..85 */
@ 0x004df6c0 void __thiscall PowerupActorSetupIcon(br_actor *actor /*ecx*/);                     /* index 86..87 */
@ 0x004dfe10 unsigned int PowerupModelCustomCB(br_actor *actor, ...);  /* model->custom: cycles the 3 icon models + spins */
@ 0x004df650 unsigned int SpecialActorModelCustomCB(br_actor *actor, ...); /* model->custom for index 66..85 */
@ 0x004f1030 void __thiscall QueueSpecialActorHit(void *car /*ecx*/, int powerup_index /*edx*/, br_actor *actor);
@ 0x004f4e20 void __thiscall SpecialActorReact(void *desc /*ecx*/, ...); /* hides the pickup at 0x004F4FEF */
@ 0x004e0750 void __thiscall RegisterCollectedPickup(int powerup_index /*ecx*/, br_actor *actor /*edx*/);
@ 0x004db880 void RespawnDuePickups(void);        /* per-frame; shows + frees due slots */
@ 0x004e07d0 void __thiscall ShowActor(br_actor *actor);  /* render_style = FACES */
@ 0x004e07e0 void __thiscall HideActor(br_actor *actor);  /* render_style = NONE  */
@ 0x004d96c0 void LoadPowerups(void);             /* POWERUP.TXT / ZOMPOWERUP.TXT / ALPOWERUP.TXT */
@ 0x004d8d30 void __thiscall ApplyPowerupToCar(void *car /*ecx*/, int powerup_index /*edx*/);

$ 0x006a0ae0 void*  g_powerup_model_arm    /* br_model* "PowArm", cloned from &68powerup1.ACT */
$ 0x006a0ae4 void*  g_powerup_model_pow    /* br_model* "PowPow", cloned from &70powerup1.ACT */
$ 0x006a0ae8 void*  g_powerup_model_off    /* br_model* "PowOff", cloned from &69powerup1.ACT */
$ 0x006a0458 void*  g_pickup_respawn_slots /* 100 entries, stride 0x0C: [0]=br_actor* (NULL=free), [4]=powerup index, [8]=deadline ms */
$ 0x006a4430 void*  g_pickup_hit_queue     /* 50 entries, stride 0x0C: [0]=owner car, [4]=powerup index, [8]=br_actor* */
$ 0x006a55bc int    g_pickup_hit_count     /* drained + zeroed each physics step at 0x004ED268 */
$ 0x006a0a54 void*  g_powerup_defs         /* POWERUP.TXT type table, stride 0xAC, kMem tag 0xC5 */
$ 0x006a0ad0 int    g_powerup_count        /* entries in g_powerup_defs */
$ 0x006a0a50 void*  g_powerup_enabled_tbl  /* byte[g_powerup_count]; 0 = never respawns */
$ 0x007447d8 int    g_pickup_respawn_base  /* ms */
$ 0x007447e8 int    g_pickup_respawn_range /* ms; deadline = now + base + range/2 */

/* --- Tinted/pulse poly overlay pool (see findings.md section 11) ---
 * Fixed array of 10 slots, base 0x00705C80, stride 0x6450 (25680), limit 0x007447D0.
 * Zeroed wholesale by TintPolyInit. TintPolyShow/Hide take the slot index in ecx and
 * check only slot->in_use -- there is NO index bounds check, so a negative index reads
 * the track pool below the table and can yield a garbage br_actor* (ESC-menu crash). */
struct tint_poly_slot {          /* 0x6450 */
    struct br_actor *actor;      /* 0x00  render_style at actor+0x20 */
    unsigned char pad04[0x08];
    unsigned int cleared0C;      /* 0x0C  zeroed by TintPolyHide */
    unsigned int cleared10;      /* 0x10 */
    unsigned int cleared14;      /* 0x14 */
    unsigned char pad18[0x18];
    unsigned int in_use;         /* 0x30  0 = free slot (TintPolyCreate's scan key) */
    unsigned int visible;        /* 0x34 */
    unsigned int subclass;       /* 0x38  2..6, "Invalid Pulse Poly subclass" */
    unsigned char pad3C[0x04];
    struct br_material *material;/* 0x40  "Tint Poly Mat" */
};

@ 0x004d7040 void TintPolyInit(void);                  /* rep stosd 0xFAC8 dwords @0x705C80 */
@ 0x004d70c0 int  __fastcall TintPolyCreate(int a /*ecx*/, int b /*edx*/, int w, int h, int subclass, ...); /* -> slot index, -1 if pool full */
@ 0x004d8220 void __fastcall TintPolyShow(int slot /*ecx*/);   /* render_style = FACES; NO bounds check */
@ 0x004d8250 void __fastcall TintPolyHide(int slot /*ecx*/);   /* render_style = NONE;  NO bounds check -- crashes at 0x004D826E for slot < 0 */
@ 0x004d8cf0 int  __fastcall TintPolyIsVisible(int slot /*ecx*/);
@ 0x004d8630 void __fastcall TintPolyTick(int slot /*ecx*/);   /* derefs slot->actor unguarded */
@ 0x004d8290 void TintPolySceneRender(void);           /* BrZbSceneRender with the tinted_poly_camera */

$ 0x00705c80 void*  g_tint_poly_pool        /* tint_poly_slot[10], stride 0x6450 */
$ 0x00655e48 int    g_tint_poly_fullscreen  /* slot handle, set at 0x0047E00A; 0 at runtime */
$ 0x00655e4c int    g_tint_poly_pulse       /* slot handle, set at 0x0047E01B; 1 at runtime */
$ 0x00655e50 int    g_tint_poly_dead        /* NEVER WRITTEN -- stays -1; read only at 0x0046D91C */

/* --- Race loop / frontend entry (see findings.md section 11.5) --- */
@ 0x00503c50 void RaceMainLoop(void);
@ 0x004939ea void RaceFrameTick(void);                 /* brackets the pause menu with 0x00504230 / 0x005042A0 */
@ 0x00494570 int  PauseMenuHandler(void);              /* calls FrontendEnterFromRace(1) */
@ 0x0046d8e0 int  __thiscall FrontendEnterFromRace(void *this /*ecx*/); /* hides the 3 tint polys, then Frontend_Setup */
@ 0x0046d1c0 void __thiscall Frontend_Setup(void *this /*ecx*/);        /* "START OF FRONTEND_Setup" */
@ 0x00504230 void RaceLoopPauseHideTintPolys(void);    /* hides only slots 0x655E48 / 0x655E4C -- safe */
@ 0x005042a0 void RaceLoopPauseRestoreTintPolys(void);

/* --- Smoke / blend sprites (see findings.md section 12) --- */
@ 0x004f9fc0 void InitSmokeStuff(void);                /* gBlend_model(2), gBlend_actor, 35 "some smoke" materials */
@ 0x004fb1b0 void DrawSmokeParticles(void);            /* depth-sorts, then one BrZbSceneRenderAdd per particle */
@ 0x0048ec00 struct br_pixelmap *__thiscall LoadPixelmap(char *name /*ecx*/);

$ 0x0074cf30 void*  g_blend_model           /* br_model* "gBlend_model", 4 verts / 2 faces */
$ 0x0074cf94 void*  g_blend_model2          /* br_model* "gBlend_model2", 6 verts / 4 faces */
$ 0x0074caac void*  g_blend_actor           /* br_actor* "gBlend_actor", reused for every particle */
$ 0x0074a674 void*  g_render_palette        /* br_pixelmap* DRRENDER.PAL, loaded at 0x004B50DA */
$ 0x006a87f0 void*  g_smoke_draw_records    /* 35 entries, stride 0x24; [0x1C] = the slot's material */
$ 0x006a880c void*  g_smoke_materials       /* == g_smoke_draw_records + 0x1C, walked as stride 0x24 */
$ 0x006a8760 void*  g_smoke_draw_list       /* br_actor-less pointer list into the records above */
$ 0x006aa56c int    g_smoke_draw_count
$ 0x006b7840 void*  g_smoke_type_colours    /* 16 x 0x00RRGGBB, indexed by particle type nibble */
$ 0x00660148 void*  g_smoke_extra_tokens    /* { BLEND_B 1 }, { OPACITY_X <rewritten per particle> }, { 0, 0 } */
$ 0x005962f8 void*  g_accpoly_extra_tokens  /* same shape, value 0x00800000 (128/255); NOT terminated */

/* --- Fog / depth cue (see findings.md section 21) --- */
/* br_material fog fields (struct size 0x9C, reflection table @ 0x006637F0):
     +0x20 flags   bit 0x00080000 = BR_MATF_FOG_LOCAL (fog enable)
     +0x5C float   fog_min      world units, same scale as camera hither/yon
     +0x60 float   fog_max
     +0x64 BR_COLOUR fog_colour 0x00RRGGBB                                     */
enum BrMaterialFogFlag { BR_MATF_FOG_LOCAL = 0x00080000 };
enum BrFogToken {
    BRT_NONE      = 0x001,
    BRT_LINEAR    = 0x093,
    BRT_FOG_T     = 0x095,   /* value = BRT_LINEAR or BRT_NONE */
    BRT_FOG_RGB   = 0x096,   /* br_colour  0x00RRGGBB */
    BRT_FOG_MIN_F = 0x097,   /* float */
    BRT_FOG_MIN_X = 0x098,   /* br_fixed, unused by this build */
    BRT_FOG_MAX_F = 0x099,   /* float */
    BRT_FOG_MAX_X = 0x09A,   /* br_fixed, unused */
    BRT_FOG_TL    = 0x12F,
    BRT_PRIMITIVE = 0x07C    /* state part the fog tokens live on */
};
enum DepthCueType { DEPTHCUE_OFF = -1, DEPTHCUE_DARK = 0, DEPTHCUE_FOG = 1, DEPTHCUE_COLOUR = 2 };

@ 0x00445340 void __fastcall SetDepthCue(int type /*ecx*/, void *shade_table /*edx*/, int p1, int p2, int r, int g, int b, int apply); /* ret 0x18 */
@ 0x004451a0 void __fastcall ApplyDepthCueToMaterial(void *material /*ecx*/); /* writes flags|0x80000, fog_min, fog_max, fog_colour, then BrMaterialUpdate(mat,0x7FFF) */
@ 0x00447220 void CommitLevelDepthCue(void);           /* level block 0x75D744.. -> live block 0x75D760.. */
@ 0x00446cc0 void CycleDepthEffectMode(void);          /* debug key: "Fog mode"/"Colour Fog mode"/"Darkness mode"/"Depth effects disabled" */
@ 0x00445620 void LoadDepthCueTables(void);            /* DEPTHCUE/FOG/ACIDFOG/BLUEGIT .TAB + HORIZON.MAT */
@ 0x00520e70 void __cdecl BrMaterialUpdate(void *material, int parts); /* emits FOG_T/MIN_F/MAX_F/RGB on BRT_PRIMITIVE via renderer vtbl+0x84 */
@ 0x00504bf0 void RaceTxtLoad(void);                   /* depth-cue block parsed at 0x00505E6B..0x00505EC7 */
@ 0x0048fa70 int  __fastcall ParseEnumFromList(void *file /*ecx*/, char **names /*edx*/, int count);
@ 0x0048fdc0 void __fastcall ParseTwoInts(void *file /*ecx*/, int *a /*edx*/, int *b);
@ 0x0048fe30 void __fastcall ParseThreeInts(void *file /*ecx*/, int *a /*edx*/, int *b, int *c);

$ 0x0075d744 int    g_depthCueType          /* from race TXT: -1 none / 0 dark / 1 fog / 2 colour */
$ 0x0075d748 int    g_depthCueP1            /* fog-start exponent; fog_min = g_yon * 10^(-P1/10) */
$ 0x0075d74c int    g_depthCueP2            /* fog-end exponent;   fog_max = g_yon * 10^( P2/10) */
$ 0x0075d750 int    g_depthCueR             /* 0..255 */
$ 0x0075d754 int    g_depthCueG
$ 0x0075d758 int    g_depthCueB
$ 0x0075d75c void*  g_depthCueShadeTable    /* br_pixelmap* */
$ 0x0075d760 int    g_fogType               /* live copy -- read this per frame */
$ 0x0075d764 int    g_fogP1
$ 0x0075d768 int    g_fogP2
$ 0x0075d76c int    g_fogR
$ 0x0075d770 int    g_fogG
$ 0x0075d774 int    g_fogB
$ 0x0075d778 void*  g_fogShadeTable         /* br_pixelmap* */
$ 0x0074caa8 int    g_levelFogR             /* duplicate of g_depthCueR, written at 0x00505EB6 */
$ 0x0074cf2c int    g_levelFogG
$ 0x0074cad0 int    g_levelFogB
$ 0x00761f4c float  g_yon                   /* view depth / far distance, default 5.0; scales fog_min/fog_max */
$ 0x0067c4e0 void*  g_horizonMaterial       /* HORIZON.MAT -- NEVER fogged; shade table goes in its +0x40 colour_map */
$ 0x0067c4a0 void*  g_shadeTabPixelmap      /* SHADETAB */
$ 0x0079ec20 void*  g_pmDepthCueTab         /* DEPTHCUE.TAB */
$ 0x0079ec38 void*  g_pmFogTab              /* FOG.TAB */
$ 0x0079ec24 void*  g_pmAcidFogTab          /* ACIDFOG.TAB */
$ 0x0079ec28 void*  g_pmBlueGitTab          /* BLUEGIT.TAB */
$ 0x00660e90 void*  g_depthCueModeNames     /* {"dark","fog","colour"} */


/* --- Billboard sprite particle systems (see findings.md section 25) ---------------
 * Three independent systems, all drawing camera-facing textured quads. None of them
 * ever calls BrModelUpdate: the geometry is a constant unit quad and everything that
 * makes one particle look different from the next lives in the material's colour_map
 * or in the actor transform. All three are rendered by the ordinary scene walk (the
 * actors are BrActorAdd'ed into g_effects_parent_actor), so BrZbModelRender receives
 * actor->material as its 3rd argument -- the model faces carry no material. */

/* One animation frame. Parsed from the effect text blocks; the authored opacity is
 * stored and then immediately overwritten with 100.0f at 0x004EE8F2, so only `map`
 * survives into the game. */
struct br_sprite_frame {
    float opacity;                  /* 0x00  always 100.0f -- authored value discarded */
    struct br_pixelmap *map;        /* 0x04  BrMapFind(name) */
};                                  /* 0x08 */

/* One "explosion group" from the text spec. Array allocated by ParseSpriteEmitterList. */
struct br_sprite_emitter {          /* 0x44 */
    short count_min, count_max;     /* 0x00 0x02 */
    short nframes;                  /* 0x04 */
    short delay_min, delay_max;     /* 0x06 0x08 */
    short rate_min, rate_max;       /* 0x0A 0x0C */
    float scale_min, scale_max;     /* 0x10 0x14 */
    unsigned char pad18[0x06];
    float vel_x_min, vel_x_max;     /* 0x18 0x1C */
    float vel_y_min, vel_y_max;     /* 0x20 0x24 */
    float vel_z_min, vel_z_max;     /* 0x28 0x2C */
    struct br_vector3 offset;       /* 0x30 */
    int rotate_mode;                /* 0x3C  0 = norotate, else randomrotate */
    struct br_sprite_frame *frames; /* 0x40  nframes entries */
};

/* Header the spawner takes in ecx: { count, br_sprite_emitter* }. */
struct br_sprite_emitter_list { int count; struct br_sprite_emitter *emitters; };

/* The single 50-slot pool every data-driven sprite effect draws from -- explosion
 * fire, powerup sparkle, blood clouds, "BANG!" marks. Slots are handed out
 * round-robin, so a slot actor/model/material triple is recycled ACROSS EFFECT
 * TYPES: nothing about a slot identifies which effect currently owns it. */
struct sprite_particle {            /* 0x78 */
    int death_time;                 /* 0x00  0 = never used; ms */
    int size_seed;                  /* 0x04 */
    unsigned char opacity;          /* 0x08  written 0xFF at spawn */
    unsigned char nframes;          /* 0x09 */
    unsigned char free;             /* 0x0A  0 = live, 1 = free */
    unsigned char pad0B;
    void *owner;                    /* 0x0C  emitter descriptor / owning actor */
    struct br_actor *actor;         /* 0x10  own br_actor, own br_model, own br_material */
    int frame_period;               /* 0x14 */
    short angle;                    /* 0x18  random Z rotation when rotate_mode != 0 */
    short pad1A;
    struct br_vector3 origin;       /* 0x1C */
    struct br_sprite_frame frames[1]; /* 0x28  nframes entries, COPIED from the emitter */
};

/* One burning car. Root actor + exactly 3 child sprite actors; every child has its own
 * br_material but they ALL share g_flame_model, so a per-br_model texture cache
 * collapses all 30 flame sprites onto one animation frame. */
struct flame_slot {                 /* 0x7C */
    void *car;                      /* 0x00 */
    unsigned char pad04[0x0C];
    int timer;                      /* 0x10  set to 2000 by StartCarFire */
    int pad14;
    int intensity;                  /* 0x18 */
    int pad1C;
    struct br_actor *root;          /* 0x20  3 children, each a Lollipop sprite */
    int frame[3];                   /* 0x24  per-child index into g_flame_pixelmaps */
    /* 0x30.. per-child scale/offset randoms, addressed as frame[i] + 6/9/12/15 dwords */
};

@ 0x004ee780 void __fastcall ParseSpriteEmitterList(void *file /*ecx*/, struct br_sprite_emitter_list *out /*edx*/); /* frames: BrMapFind by name, error 0x77 if missing */
@ 0x004efa00 void __fastcall ParseGeneralTxtEffectBlocks(void *file /*ecx*/); /* GENERAL.TXT: wasted-explosion / powerup-collect / powerup-respawn */
@ 0x004ea880 void InitSpriteParticlePool(void);   /* WAS "InitImpactDecals": 50 slots, each its OWN br_actor + unit XY-quad br_model + "BANG!" br_material */
@ 0x004ead00 void __fastcall SpawnSpriteParticles(struct br_sprite_emitter_list *emitters /*ecx*/, void *owner /*edx*/, struct br_vector3 *bbox /*arg0*/, struct br_vector3 *origin /*arg1*/);
@ 0x004eaaf0 void AnimateSpriteParticles(void);   /* per frame: material->colour_map = frames[t].map; BrMaterialUpdate(mat, 0x7FFF) */
@ 0x004eb020 void __fastcall KillSpriteParticlesForOwner(void *owner /*ecx*/);
@ 0x004fdc10 void InitExplosionsAndSprites(void); /* debris models + 30 debris actors, then InitFlames + InitSplashes */
@ 0x004fc3a0 void InitFlames(void);               /* "Lollipop" model, FLAMES.PIX x20, 10 slots x 3 child actors/materials */
@ 0x004fc2e0 void ShutdownFlames(void);
@ 0x004fbdd0 void __fastcall UpdateFlameSlot(int slot /*ecx*/, struct br_vector3 *pos /*edx*/, int alive); /* writes child->material->colour_map per frame */
@ 0x004fc9e0 void RemoveAllFlameActors(void);
@ 0x004fcab0 void __fastcall StartCarFire(void *car /*ecx*/, int seat /*edx*/, int intensity);
@ 0x004fed90 int  __fastcall IsCarOnFire(void *car /*ecx*/);
@ 0x004fdde0 void __fastcall InitSplashes(void *name_list /*ecx*/); /* "Splash" model, SPLSHBLU.PIX x<=20, one material PER FRAME, 32 actors */
@ 0x004fd530 void __fastcall SpawnSplash(void *car /*ecx*/, ...);   /* round-robin over g_splash_slots; also seeds a spark */
@ 0x004f9790 void EffectsTick(void);              /* splash + debris transforms; no material or model change */
@ 0x00513a30 int  __fastcall LoadPixelmapMany(char *name /*ecx*/, struct br_pixelmap **out /*edx*/, int max); /* -> count loaded */
@ 0x0051f010 void BrMapAddMany(struct br_pixelmap **maps, int count);
@ 0x0051eff0 struct br_pixelmap *BrMapFind(char *name);

$ 0x006aa380 void*  g_flame_model          /* br_model* "Lollipop", 4 verts / 2 faces, XY quad x -0.5..0.5, y 0..1 */
$ 0x006a8638 void*  g_flame_pixelmaps      /* br_pixelmap*[20] from FLAMES.PIX (FLM01..FLM20) */
$ 0x00660118 void*  g_flame_frame_size     /* 20 x { u8 width, u8 height } source sizes for the 20 frames */
$ 0x006a96ac void*  g_flame_slots          /* flame_slot[10], stride 0x7C */
$ 0x006aa59c int    g_flame_slot_mask      /* bit per live flame slot */
$ 0x006a8758 void*  g_splash_model         /* br_model* "Splash", identical quad to Lollipop */
$ 0x006a9130 void*  g_splash_frame_materials /* br_material*[g_splash_frame_count], ONE per animation frame */
$ 0x006aa5a4 int    g_splash_frame_count   /* <= 20 */
$ 0x006a82b8 void*  g_splash_slots         /* 32 entries, stride 0x1C: [0x00] actor, [0x10] alive, [0x14] size, [0x18] flip */
$ 0x006aa570 int    g_splash_slot_mask
$ 0x006a82ac int    g_splash_next          /* round-robin index, wraps at 32 */
$ 0x006a55c8 void*  g_sprite_particles     /* sprite_particle[50], stride 0x78 */
$ 0x006a82a0 int    g_sprite_particle_next /* round-robin scan start when the pool is full */
$ 0x0074d35c void*  g_effects_camera_actor /* its br_matrix34 is copied into every particle actor -- billboarding */
$ 0x007634b8 void*  g_effects_parent_actor /* BrActorAdd target for particle / splash / debris actors */
$ 0x006a9180 void*  g_debris_slots         /* 30 entries, stride 0x2C; 3D chunks, not sprites */
$ 0x006aa584 int    g_debris_slot_mask
$ 0x006aa588 void*  g_debris_model_a       /* alternated per slot with g_debris_model_b */
$ 0x006aa58c void*  g_debris_model_b
$ 0x006a52d0 void*  g_fx_wasted_explosion  /* 0x2E0 GENERAL.TXT block; emitter list at +0x23C = 0x006A550C (ex00001..ex00007) */
$ 0x006a7ce0 void*  g_fx_powerup_collect   /* 0x2E0 block; emitter list at 0x006A7F1C (BING1..6, TWINK1..4) */
$ 0x006a3660 void*  g_fx_powerup_respawn   /* 0x2E0 block */
$ 0x006a7f1c void*  g_fx_powerup_collect_emitters /* { count, br_sprite_emitter* } -- the sparkle */
$ 0x00694478 void*  g_fx_ped_blood_emitters      /* PEDS/SETTINGS.TXT blood clouds (BIGBL01..05) */
$ 0x0069bc28 void*  g_fx_ped_blood_emitters2
$ 0x007620f8 void*  g_fx_impact_emitters         /* current car-impact effect */

/* --- Race frame order, pixelmaps and the Glide route (2026-09-02) ---
 * The Glide code is NOT in the EXE: CARMA2_HW.EXE has no glide2x import and no "gr*" string.
 * 3dfx_win.bdd (base 0x10000000, export BrDrv1Begin @ +0x1A70) is mapped by BRender's OWN PE
 * loader (BrDLLLoadImage 0x00530E70), which resolves its BRCORE1/BRPMAP1/BRHOST1 imports from the
 * statically linked BRender inside the EXE; only its glide2x.dll import goes through the OS
 * loader. That is how nGlide ends up in the process. grSstIdle is never imported or called --
 * the only sync is grBufferNumPending immediately before grBufferSwap. */

/* br_device_pixelmap dispatch offsets, as used by the EXE wrappers. The Glide dispatch struct
 * lives at 3dfx_win.bdd+0xE470; the glide2x entry each slot reaches is noted. */
enum br_pixelmap_dispatch {
    PMD_isType            = 0x20,
    PMD_match             = 0x4C, /* bdd 0x100023A0 -- makes the back buffer / depth buffer */
    PMD_allocateSub       = 0x50, /* bdd 0x10002E60 -- viewport sub-pixelmaps */
    PMD_copy              = 0x54,
    PMD_copyTo            = 0x58,
    PMD_copyFrom          = 0x5C,
    PMD_fill              = 0x60, /* bdd 0x100026C0 -> grRenderBuffer/grColorMask/grDepthMask/grBufferClear */
    PMD_doubleBuffer      = 0x64, /* bdd 0x100028F0 -> grBufferNumPending x2, grBufferSwap */
    PMD_rectangle         = 0x7C,
    PMD_rectangleCopy     = 0x84, /* bdd 0x10002A30 -> grLfbWriteRegion */
    PMD_rectangleCopyTo   = 0x88, /* same */
    PMD_rectangleCopyFrom = 0x8C, /* bdd 0x10002AF0 -> grLfbReadRegion */
    PMD_rectStretchCopy   = 0x90,
    PMD_rectangleFill     = 0x9C, /* bdd 0x10002580 -> grLfbLock / write / grLfbUnlock */
    PMD_pixelSet          = 0xA0, /* bdd 0x10002950 -> grLfbLock / grLfbUnlock */
    PMD_line              = 0xA4, /* STUB on the Glide device */
    PMD_text              = 0xAC, /* STUB on the Glide device */
    PMD_directLock        = 0xD8, /* bdd 0x10002F40 -> grLfbLock */
    PMD_directUnlock      = 0xDC, /* bdd 0x10002FB0 -> grLfbUnlock */
};

@ 0x004e4e40 void RenderAFrame(void);          /* the whole frame; called from RaceFrameTick @0x00493AEA */
@ 0x004e54f0 void __fastcall RenderView(int view_index /*ecx*/, br_actor *camera, br_pixelmap *colour, br_pixelmap *depth); /* view 0 also renders the reflection textures first */
@ 0x004e5680 void RenderScene(br_pixelmap *colour, br_pixelmap *depth, float yon_scale, int do_shadows, int do_particles, int); /* edx = camera actor, ecx = owning car; ret 0x18 */
@ 0x00445cb0 void DrawHorizon(br_actor *camera /*edx*/, void *ctx /*ecx*/); /* scrolls g_horizonMaterial.map_transform by camera yaw, then BrZbSceneRenderAdd @0x00445E05 */
@ 0x00446340 void FrameDepthCueUpdate(void);   /* TintPolyShow/Hide + refresh the horizon shade table */
@ 0x004e74d0 void BuildCarShadows(void);       /* -> 0x004E7650 per nearby car */
@ 0x00540560 void SetScreenDepthBias(unsigned int level); /* [0x0079FEB4] = g_depth_bias_table[level] */
@ 0x004d3610 void DrawSeveredLimbs(void);      /* Limbs_actor pool, added then removed each frame */
@ 0x00506e50 void DrawPickupsAndMisc(br_actor *cam, void *car);
@ 0x0051c300 void PDAllocateScreenAndBack(void); /* BrDevBeginVar("3DFX_WIN",640,480,16,RGB_565), _match back + third page */
@ 0x0051c520 void PDScreenSwap(void);          /* BrPixelmapDoubleBuffer(g_pmScreen, g_pmBackBuffer) -> grBufferSwap */
@ 0x004e4940 void AllocateDepthBuffer(void);   /* [0x0068B8A4] = back->_match(back, 1) */
@ 0x004e4980 void __fastcall SetupRaceViewport(int x /*ecx*/, int y /*edx*/, int w, int h); /* calls PDAllocateScreenAndBack, then g_pmRaceView = sub-pixelmap of the back buffer */
@ 0x004e5cb0 void MirrorQueueReset(void);      /* per frame, from RaceFrameTick @0x00492BC9 */
@ 0x004e5cc0 void __fastcall MirrorQueueAdd(br_actor *camera /*ecx*/, br_material *mat /*edx*/);
@ 0x00464e40 void HudDrawText3D(int y, int font, float align, int flush); /* ecx = string, edx = x; glyph actors -> BrZbSceneAddActorIncremental */
@ 0x004e5ad0 void __fastcall HudQueueActor(br_actor *actor /*ecx*/); /* -> g_hud_actor_list, max 128 */
@ 0x004e5b00 void HudFlush(void);              /* one BrZbSceneAddActorIncremental for the whole HUD queue */
@ 0x0047cad0 void DrawOverlayQuad(int y0, int x0, int y1);  /* edx = x1; rebuilds g_overlay_quad_model, then BrZbSceneRender @0x0047CB9C */
@ 0x0047ba80 void __fastcall BlitSprite16(short dst_y, br_pixelmap *src, short sx, short sy, short w, short h); /* ecx = dst pixelmap, edx = dst_x; raw 16-bit CPU blit, colour-keys on 0 */
@ 0x0047c740 void DrawRaceMap(void);           /* 11x BrPixelmapLine into a MEMORY pixelmap */
@ 0x00523160 void BrPixelmapStore(br_pixelmap *pm, unsigned int flags); /* uploads pm into the driver as a texture (renderer +0x78, token 0xA5) */
@ 0x005382f0 void BrPixelmapFill(br_pixelmap *pm, unsigned int colour);
@ 0x005389e0 void BrPixelmapDoubleBuffer(br_pixelmap *dst, br_pixelmap *src);
@ 0x00538640 void BrPixelmapRectangleFill(br_pixelmap *pm, int x, int y, int w, int h, unsigned int colour);
@ 0x00538590 void BrPixelmapRectangleCopy(br_pixelmap *dst, int dx, int dy, br_pixelmap *src, int sx, int sy, int w, int h);
@ 0x00538990 void BrPixelmapLine(br_pixelmap *pm, int x1, int y1, int x2, int y2, unsigned int colour);
@ 0x00538a10 void BrPixelmapText(br_pixelmap *pm, int x, int y, unsigned int colour, void *font, const char *text);
@ 0x00538d80 br_pixelmap *BrPixelmapAllocate(unsigned char type, int w, int h, void *pixels, int flags);
@ 0x00537e20 br_pixelmap *BrPixelmapMatch(br_pixelmap *src, int match_type);
@ 0x00538d20 void *BrPixelmapDirectLock(br_pixelmap *pm, int);   /* -> grLfbLock */
@ 0x00538d50 void BrPixelmapDirectUnlock(br_pixelmap *pm);       /* -> grLfbUnlock */
@ 0x00530e70 void *BrDLLLoadImage(const char *path);  /* BRender own PE loader for .bdd drivers */
@ 0x0052ffa0 void *BrDLLLoad(const char *name);
@ 0x005301e0 void *BrDLLQuerySymbol(void *module, const char *name, int);
@ 0x00528e10 int  BrDevBeginVar(void **pmap, const char *device, ...);
@ 0x005285c0 int  BrDevFindOrLoad(void **out, const char *name, void *tokens);

$ 0x0074d3e0 void*  g_pmScreen          /* front buffer; identifier "Voodoo Graphics" under Glide */
$ 0x0074d360 void*  g_pmBackBuffer      /* g_pmScreen->_match(); every 2D/HUD/tint draw targets this. */
$ 0x006ad47c void*  g_pmThirdPage       /* g_pmBackBuffer->_match() */
$ 0x0068b8a4 void*  g_pmDepthBuffer     /* g_pmBackBuffer->_match(.., 1) */
$ 0x00762128 void*  g_pmRaceView        /* sub-pixelmap of g_pmBackBuffer -- the 3D viewport */
$ 0x0068b8a8 void*  g_pmSecondView      /* sub-pixelmap of g_pmBackBuffer -- mirror / PiP colour target */
$ 0x0075b93c void*  g_pmSecondViewDepth /* holds g_pmDepthBuffer */
$ 0x006a22bc void*  g_pmReflection      /* 64x64 render target for mirrors / env maps */
$ 0x006a22c0 int    g_mirror_queue_count
$ 0x006a22c8 void*  g_mirror_queue      /* stride 8: { br_actor* camera, br_material* target } */
$ 0x0074d44c void*  g_world_root_actor  /* BrActorAllocate(0,0) at 0x0047DE61; the `world` argument */
$ 0x0075b940 void*  g_second_view_camera
$ 0x00704e40 int    g_second_view_active
$ 0x0074b778 int    g_current_view_index /* 0 = main view, 1 = second view; read by DrawHorizon */
$ 0x0074d644 void*  g_backdrop_actors   /* 4 entries 0x0074D644..0x0074D650 (0x0074D648 skipped), drawn first with a depth bias */
$ 0x00670530 float* g_depth_bias_table  /* { 0, -1.5, -3, -4.5, -6, -7.5, -9 } */
$ 0x0079feb4 float  g_screen_depth_bias /* added to screen Z by the rasterizer (0x00547A53 et al) */
$ 0x0067c4c0 void*  g_horizon_actor_alt /* used when g_current_view_index != 0 */
$ 0x0067c4d8 void*  g_horizon_actor     /* main view sky dome */
$ 0x0067c4ac short  g_horizon_built_fov
$ 0x0067c4b8 float  g_horizon_built_yon
$ 0x0074ca00 void*  g_hud_root_actor    /* world for the HUD BrZbSceneAddActorIncremental */
$ 0x0074cf74 void*  g_hud_camera_actor
$ 0x0074cf10 void*  g_hud_text_root     /* parent of the glyph actors */
$ 0x0074cae0 void*  g_hud_glyph_actors  /* actor pool, cap 0x100 */
$ 0x00686490 int    g_hud_glyph_count
$ 0x00704e60 void*  g_hud_actor_list    /* 128 slots; "Not enough HUD actor storage" @0x0065FB90 */
$ 0x00703e28 int    g_hud_actor_count
$ 0x0074cac4 void*  g_overlay_camera_actor /* world == camera for the DrawOverlayQuad BrZbSceneRender */
$ 0x0074ca70 void*  g_overlay_quad_model
$ 0x0074cf24 void*  g_overlay_quad_actor
$ 0x0074ca1c void*  g_pmDashboard       /* current entry of g_dashboard_pixelmaps; CPU blit target */
$ 0x0067fd00 void*  g_dashboard_pixelmaps /* 12 memory pixelmaps loaded by LoadPixelmap */
$ 0x0068be38 int    g_letterbox_enabled  /* gates the 4 back-buffer rectangleFills */
$ 0x0067c478 unsigned int g_swap_hold_ms /* PDScreenSwap is skipped until now > this + 500 */
$ 0x0079f940 void*  g_brender_device_list /* BrDevAdd target, capacity 16, allocated at 0x00527EDF */
$ 0x0079f934 void*  g_brender_module_list /* the BrDLLLoad internal module registry */

/* --- Backface culling (see findings.md section 30) ---
 * BRender culls per face, in MODEL space, from the prepared "online" face planes -- not
 * from a screen-space area and not from br_face. The Glide driver never culls
 * (grCullMode(GR_CULL_DISABLE) once at init), so this is the only facing test in the game,
 * and it runs downstream of the ModelRenderStyle_Faces hook: a hook there sees unculled
 * model-space geometry.
 *
 * The pipeline builder at 0x00542BAD switches on renderer+0x18 (BRP_CULL type, published
 * by BrMaterialUpdate from the material flags):
 *     BRT_NONE      0x01  -> 0x00543110 / 0x00543190: every face marked visible, no test
 *     BRT_ONE_SIDED 0xAD  -> 0x005431F0 / 0x00543450 -> 0x005432B0 (persp) / 0x00543380 (par)
 *     BRT_TWO_SIDED 0xAE  -> 0x005434E0 / 0x005437C0 -> 0x005435A0 / 0x005436B0: same test,
 *                           never culls; sets front/back flag 4/5 and a normal-flip sign
 *
 * The one-sided perspective test, 0x005432DB..0x0054330C:
 *     keep face  <=>  dot(online_face->n, g_cull_eye_model) >= online_face->d
 * with d = dot(n, v0) and n = normalize((v1-v0) x (v2-v0)) over the PIVOT-RELATIVE online
 * vertices, so the test is dot(n, eye - v0) >= 0: keep when the eye is on the normal's
 * side. Equality is kept. The parallel variant compares dot(n, viewdir) against 0.0.
 *
 * Winding: a front face is v0->v1->v2 counter-clockwise as seen from the eye in BRender's
 * right-handed model space. Under a right-handed projection that is counter-clockwise in
 * NDC and, after D3D's downward-Y viewport flip, CLOCKWISE on screen -- which is D3D9's
 * own front-face convention (dxvk d3d9_rtx.cpp: frontFace = VK_FRONT_FACE_CLOCKWISE), so
 * the mode that culls back faces is D3DCULL_CCW (dxvk d3d9_util.cpp:271 maps it to
 * VK_CULL_MODE_BACK_BIT).
 */
enum br_material_cull_flags {
    BR_MATF_ALWAYS_VISIBLE = 0x0800,  /* -> BRT_NONE: no facing test at all */
    BR_MATF_TWO_SIDED      = 0x1000,  /* -> BRT_TWO_SIDED: tested, never culled; wins over 0x0800 */
    BR_MATF_FORCE_FRONT    = 0x2000,  /* BRT_FORCE_FRONT_B -- lighting only, not culling */
};

enum br_cull_token {
    BRP_CULL       = 0x74,   /* renderer part BrMaterialUpdate publishes the mode on */
    BRT_TYPE_T     = 0xAC,
    BRT_NONE_CULL  = 0x01,
    BRT_ONE_SIDED  = 0xAD,
    BRT_TWO_SIDED  = 0xAE,
};

/* The prepared face the cull walks: v1_group +0x04, stride 0x1C. Its plane is rebuilt by
 * FUN_0051F6A0 from the online (pivot-relative) vertices -- it is NOT a copy of br_face. */
struct v1_online_face_plane {
    unsigned short v[3];        /* 0x00 */
    unsigned char pad06[6];     /* 0x06  per-face index/colour */
    struct br_vector3 n;        /* 0x0C */
    float d;                    /* 0x18  = dot(n, v0 - pivot) */
};                              /* 0x1C */

@ 0x00542930 void V1Model_Render(void *geom, void *renderer, void *prepared, void *material, int type);       /* vtable 0x0058BE98 +0x44 */
@ 0x00543a10 void V1Model_RenderOnScreen(void *geom, void *renderer, void *prepared, void *material, int type);/* +0x4C, bounds ACCEPT */
@ 0x00542960 void V1Model_RenderGroups(void);          /* builds the per-group stage pipeline, then runs it */
@ 0x00540d00 void *V1ModelGeometryAllocate(void);      /* writes vtable 0x0058BE98 */
@ 0x00540590 void *DefaultRendererFloatAllocate(void); /* "Default-Renderer-Float" -- owns the geometry objects */
@ 0x005432b0 void CullFacesOneSidedPerspective(void);  /* THE backface cull, test at 0x00543304 */
@ 0x00543380 void CullFacesOneSidedParallel(void);
@ 0x005435a0 void CullFacesTwoSidedPerspective(void);  /* flags front/back, never culls */
@ 0x005436b0 void CullFacesTwoSidedParallel(void);
@ 0x00543110 void CullFacesNone(void);                 /* marks every face visible */
@ 0x00536fb0 void BrPlaneEquation(br_vector3 *out_n_and_d, br_vector3 *v0, br_vector3 *v1, br_vector3 *v2); /* n = (v1-v0)x(v2-v0) normalized, d = +dot(n,v0) */
@ 0x0051f6a0 void BuildOnlineFacePlanes(void *group);  /* called from BrModelUpdate 0x0051FCC5 */
@ 0x00543a80 void ComputeCullEye(void);                /* model-space eye (persp) or view dir (parallel) */

$ 0x0079faf4 float  g_cull_eye_model_x     /* eye position in model space; w at 0x0079FB00 = 1.0 */
$ 0x0079faf8 float  g_cull_eye_model_y
$ 0x0079fafc float  g_cull_eye_model_z
$ 0x0079f9a4 void*  g_online_faces         /* v1_group+0x04, stride 0x1C -- what the cull walks */
$ 0x0079f984 void*  g_face_flags           /* stride 4, flag byte at +2: 0 culled, 4 front, 5 back */
$ 0x0079f988 void*  g_vertex_refcounts
$ 0x0079f99c int    g_visible_face_count
$ 0x0079f9b4 int    g_online_face_count

/* ------------------------------------------------------------------ backface culling:
 * where the cull mode is published, and proof the driver never culls (2026-09-05)
 *
 * BrMaterialUpdate 0x00520E70..0x00521459 turns the two material flag bits into the
 * renderer's cull mode. The whole decision is 0x00521187..0x005211B8:
 *
 *     0x00521187  mov  eax, [esi+0x20]     ; br_material::flags
 *     0x0052118D  test ah, 8               ; & 0x0800  BR_MATF_ALWAYS_VISIBLE
 *     0x00521190  mov  ecx, 0xAD           ; default   BRT_ONE_SIDED (cull back faces)
 *     0x00521195  je   0x0052119C
 *     0x00521197  mov  ecx, 1              ; set       BRT_NONE      (no facing test)
 *     0x0052119C  test ah, 0x10            ; & 0x1000  BR_MATF_TWO_SIDED
 *     0x0052119F  je   0x005211A6
 *     0x005211A1  mov  ecx, 0xAE           ; set       BRT_TWO_SIDED (tested, never culled)
 *     0x005211AC  push 0xAC / push 0 / push 0x74 ; renderer->partSet(BRP_CULL, 0, 0xAC, ecx)
 *
 * TWO_SIDED is tested last, so it wins when a material carries both bits. No other flag
 * bit, and no model or actor field, reaches the cull mode: culling is a pure function of
 * br_material::flags & 0x1800.
 *
 * Sprite billboards, InitSpriteParticlePool 0x004EAA4E: `and [eax+0x20],~1` clears
 * BR_MATF_LIGHT, then 0x004EAA5F `or dh,8` sets BR_MATF_ALWAYS_VISIBLE -- so every sprite
 * material is 0x0800 and is never culled. gLine_material's 0x1007 carries BR_MATF_TWO_SIDED.
 *
 * The renderer+0x18 mode reaches two face-emit loops, picked by the same switch:
 *     0x00542830  BRT_ONE_SIDED emit -- `test byte [edi+2], 4` at 0x0054285D skips the face
 *                 when the cull stage cleared the visible bit
 *     0x005428B0  every other mode -- emits unconditionally (the bit is always 4 there)
 *     0x00543940  the clipped-path emit; same `test al, 4` gate at 0x0054398C
 *
 * BrZbSceneRenderEnd 0x00522EB0 does NOT cull: partSet(0x7D,0,0xF9), bucket sort
 * (0x005267B0 / 0x00526770), renderer->flush(+0xFC). Every facing decision is already
 * baked into g_face_flags by then.
 *
 * The Glide driver does not cull either, and says so explicitly. In 3dfx_win.bdd the
 * device-open function 0x10001FB0 zeroes ESI at 0x10001FBA and never reloads it, then:
 *     0x10002112  push esi ; call 0x10005A0A -> [0x1000B1F8] _grSstSelect@4   (0)
 *     0x10002118  push esi ; call 0x10005A04 -> [0x1000B1FC] _grCullMode@4    (0)
 * grCullMode(0) is GR_CULL_DISABLE, and it is the only call to it in the driver. So a
 * proxy that replaces the rasterizer inherits no hardware cull state -- the software test
 * at 0x005432B0 is the entire facing rule.
 *
 * Float constants: 0x0058C8E0 = 0.0f (the parallel test's threshold); BrPlaneEquation uses
 * 0x0058B95C = 0.0f as its degenerate-length epsilon and 0x0058B960 = 1.0f as 1/len's
 * numerator, leaving n = (0,0,0) and d = 0 for a zero-area face (which then fails
 * `dot >= d` only when dot < 0, i.e. it survives as a front face).
 */

@ 0x00520e70 void BrMaterialUpdate_PublishesCull(br_material *material, unsigned short flags); /* cull decision at 0x00521187 */
@ 0x00542830 void EmitFacesCulled(void *self, void *renderer);   /* honours g_face_flags bit 4 */
@ 0x005428b0 void EmitFacesAll(void *self, void *renderer);      /* no facing gate */
@ 0x00543940 void EmitFacesClipped(void *self, void *renderer);  /* clipped path, same gate */
@ 0x005434c6 void CullDispatchOnScreenPerspective(void);         /* -> 0x005432b0 */
@ 0x005434b6 void CullDispatchOnScreenParallel(void);            /* -> 0x00543380 */

$ 0x0079fb00 float g_cull_eye_model_w      /* 1.0 perspective, 0.0 parallel (direction) */
$ 0x0058c8e0 float g_zero_f                /* parallel cull threshold */

/* 3dfx_win.bdd (base 0x10000000) -- culling is disabled at the hardware */
@ 0x10001fb0 int  Glide_DeviceOpen(void);  /* grSstSelect(0) + grCullMode(0) at 0x10002112/18 */
$ 0x1000b1fc void* p_grCullMode            /* IAT slot; thunk 0x10005A04, one call site */
$ 0x1000b1f8 void* p_grSstSelect           /* IAT slot; thunk 0x10005A0A */
