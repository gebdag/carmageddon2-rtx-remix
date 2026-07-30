/* Carmageddon 2: Carpocalypse Now (GOG) — knowledge base
 *
 * Binary:  CARMA2_HW.EXE  (x86 PE, image base 0x00400000, 3734 functions)
 * Engine:  BRender 1.3.x (Argonaut) — statically linked into the EXE.
 *          The *.bdd device drivers on disk are NOT loadable in this release
 *          (they import BRCORE1.dll / BRHOST1.dll / BRPMAP1.dll, none of which ship).
 *          Both the Glide and the Direct3D device drivers are compiled into the EXE.
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
    unsigned char pad44[0x08];
    void *index_shade;              /* 0x4C */
    unsigned char pad50[0x08];
    void *extra;                    /* 0x58  br_token_value list */
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
    unsigned char pad10[0x18];
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
    void *pixels;           /* 0x40 */
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

enum br_material_update {
    BR_MATU_MAP_TRANSFORM = 0x0001,
    BR_MATU_ALL = 0x7FFF,
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
@ 0x004ea880 void InitImpactDecals(void);            /* the 50-quad "BANG!" decal pool */
@ 0x00478930 void FunkApplyMapTransform(void);       /* copies a frame's br_matrix23 into material->map_transform */
@ 0x005259b0 int  BrRendererBegin(br_device *device, br_renderer *renderer);
@ 0x0051f950 void BrModelUpdate(br_model *model, unsigned short flags);
@ 0x00522eb0 void BrZbBucketFlushAndSwap(void);
@ 0x005226d0 void BrZbSceneAddActorIncremental(br_actor *actor, ...);
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
$ 0x006a55d8 void*  g_impact_decal_pool    /* 50 entries, stride 0x78, [0] = br_actor*; unit XY quad, "BANG!" material */
