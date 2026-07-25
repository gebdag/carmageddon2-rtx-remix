#pragma once

// BRender 1.3.x structures as laid out in CARMA2_HW.EXE.
// Offsets were recovered from BrModelUpdate (0x0051F950) and the BrZb* scene renderer;
// see patches/Carmageddon2/kb.h and findings.md for the derivation.

namespace comp::game
{
	// Row-vector affine transform: v' = v * M. Rows 0-2 are the linear part, row 3 the translation.
	struct br_matrix34
	{
		float m[4][3];
	};

	struct br_vector3
	{
		float v[3];
	};

	// The authored, model-space vertex array (br_model::vertices). Freed after prepare
	// unless the model carries BR_MODF_KEEP_ORIGINAL, which is why the renderer reads
	// the prepared block below instead.
	struct br_vertex
	{
		br_vector3 p;       // 0x00
		float map[2];       // 0x0C
		uint8_t index;      // 0x14
		uint8_t red;        // 0x15
		uint8_t green;      // 0x16
		uint8_t blue;       // 0x17
		uint32_t reserved;  // 0x18
		br_vector3 n;       // 0x1C
	};                      // 0x28

	// One entry of the prepared vertex array. Position is model space with br_model::pivot
	// already subtracted; add the pivot back to recover true model space.
	struct v1_online_vertex
	{
		float px, py, pz;   // 0x00
		float u, v;         // 0x0C
		float nx, ny, nz;   // 0x14
	};                      // 0x20

	// One prepared face. Only the three vertex indices matter here; they are relative to
	// the owning group's vertex array, not to br_model::vertices.
	struct v1_online_face
	{
		uint16_t v[3];      // 0x00
		uint8_t pad[0x16];
	};                      // 0x1C

	// A run of faces sharing one material. This is the unit the hardware geometry renderer
	// walks, and the unit we forward to Remix as a single draw call.
	struct v1_group
	{
		uint32_t material_token;    // 0x00  value of material[0x98], not the br_material itself
		v1_online_face* faces;      // 0x04
		uint32_t* face_colours;     // 0x08
		uint16_t* face_src_index;   // 0x0C
		v1_online_vertex* vertices; // 0x10
		uint32_t* vertex_colours;   // 0x14
		uint16_t* vertex_src_index; // 0x18
		uint16_t nfaces;            // 0x1C
		uint16_t nvertices;         // 0x1E
		uint32_t pad;               // 0x20
	};                              // 0x24

	struct v1_prepared
	{
		uint32_t size;      // 0x00
		uint32_t pad0;      // 0x04
		uint16_t ngroups;   // 0x08
		uint16_t pad1;      // 0x0A
		uint32_t pad2[3];   // 0x0C
		v1_group* groups;   // 0x18
	};

	struct br_model
	{
		br_model* next;             // 0x00
		char* identifier;           // 0x04
		br_vertex* vertices;        // 0x08
		void* faces;                // 0x0C
		uint16_t nvertices;         // 0x10
		uint16_t nfaces;            // 0x12
		br_vector3 pivot;           // 0x14
		uint16_t flags;             // 0x20
		uint16_t flags_hi;          // 0x22
		void* custom;               // 0x24
		void* user;                 // 0x28
		uint32_t pad0;              // 0x2C
		float radius;               // 0x30
		br_vector3 bounds_min;      // 0x34
		br_vector3 bounds_max;      // 0x40
		v1_prepared* prepared;      // 0x4C
		void* stored;               // 0x50
	};

	// Offsets recovered from MaterialNeedsAlpha (0x0051F630), which tests colour_map->type
	// and walks the token-value list. The struct is larger than stock BRender 1.3.
	struct br_material
	{
		uint8_t pad00[0x40];
		void* colour_map;       // 0x40  br_pixelmap*
		uint8_t pad44[0x08];
		void* index_shade;      // 0x4C
		uint8_t pad50[0x08];
		void* extra;            // 0x58  br_token_value list
		uint8_t pad5C[0x3C];
		uint32_t stored;        // 0x98  driver-side prepared material; groups key off this
	};

	// The authored face array (br_model::faces). Valid only while BrModelUpdate is running —
	// it is freed on the way out unless the model carries BR_MODF_KEEP_ORIGINAL.
	struct br_face
	{
		uint16_t vertices[3];   // 0x00
		uint16_t smoothing;     // 0x06
		br_material* material;  // 0x08
		uint8_t index;          // 0x0C
		uint8_t red;            // 0x0D
		uint8_t green;          // 0x0E
		uint8_t blue;           // 0x0F
		uint8_t pad10[0x08];    // 0x10
		br_vector3 n;           // 0x18
		float d;                // 0x24
	};                          // 0x28

	// Layout read off live textures ('ROKWATX64' 64x64 type 5, 'WATER' 64x64 type 0x12).
	// row_bytes is always width*2 for both, and +0x08 is the only pointer whose contents
	// vary smoothly like image data — +0x40 points at a driver-side object instead.
	struct br_pixelmap
	{
		void* dispatch;     // 0x00  points into .rdata
		char* identifier;   // 0x04
		void* pixels;       // 0x08
		uint32_t id;        // 0x0C
		uint8_t pad10[0x18];
		uint32_t row_bytes; // 0x28
		uint8_t type;       // 0x2C  br_pixelmap_type
		uint8_t flags;      // 0x2D
		uint16_t pad2E;
		uint16_t base_x;    // 0x30
		uint16_t base_y;    // 0x32
		uint16_t width;     // 0x34
		uint16_t height;    // 0x36
		int16_t origin_x;   // 0x38
		int16_t origin_y;   // 0x3A
		uint32_t pad3C;
		void* stored;       // 0x40  driver-side pixelmap
	};

	// The alpha-bearing entries match the set MaterialNeedsAlpha (0x0051F630) tests for:
	// 0x0D, 0x0E, 0x12, 0x18, 0x19, 0x1A, 0x1F.
	enum br_pixelmap_type : uint8_t
	{
		BR_PMT_INDEX_8 = 3,
		BR_PMT_RGB_555 = 4,
		BR_PMT_RGB_565 = 5,
		BR_PMT_RGB_888 = 6,
		BR_PMT_RGBX_888 = 7,
		BR_PMT_RGBA_8888 = 8,
		BR_PMT_RGBA_4444 = 0x12,
	};

	// br_actor::type_data for a camera actor (actor + 0x5C).
	struct br_camera
	{
		char* identifier;       // 0x00
		uint8_t type;           // 0x04  1 = BR_CAMERA_PERSPECTIVE_FOV
		uint8_t pad0;           // 0x05
		uint16_t field_of_view; // 0x06  br_angle: 0x10000 == 360 degrees
		float hither_z;         // 0x08
		float yon_z;            // 0x0C
		float aspect;           // 0x10
		float width;            // 0x14
		float height;           // 0x18
	};

	struct br_actor
	{
		br_actor* next;         // 0x00
		uint32_t pad0;          // 0x04
		br_actor* children;     // 0x08
		br_actor* parent;       // 0x0C
		uint16_t depth;         // 0x10
		uint8_t type;           // 0x12  1 = MODEL, 5 = CAMERA
		uint8_t pad1;           // 0x13
		uint32_t pad2;          // 0x14
		br_model* model;        // 0x18
		void* material;         // 0x1C
		uint8_t render_style;   // 0x20
		uint8_t pad3[3];        // 0x21
		void* env_map;          // 0x24
		uint16_t t_type;        // 0x28  6 = identity, <2 = matrix stored inline at 0x2C
		uint16_t pad4;          // 0x2A
		br_matrix34 t;          // 0x2C
		void* type_data;        // 0x5C  br_camera* for camera actors
	};

	// BRender token values used by the renderer dispatch.
	enum br_token : uint32_t
	{
		BRT_MATRIX = 0x76,
		BRT_MODEL_TO_VIEW = 0xEA,
		BRT_VIEW_TO_SCREEN = 0xEE,
	};

	// Byte offsets into the br_renderer dispatch table (first dword of the renderer object).
	enum br_renderer_dispatch : uint32_t
	{
		RD_PART_SET = 0x80,
		RD_TEMPLATE_QUERY = 0x8C,
		RD_MODEL_MUL = 0xA4,
		RD_FLUSH = 0xFC,
	};
}
