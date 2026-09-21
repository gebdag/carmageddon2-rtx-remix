#pragma once
#include "structs.hpp"

namespace comp::game
{
	// CARMA2_HW.EXE has no .reloc section, so it always loads at its preferred base of
	// 0x00400000. The rebase is kept anyway so the addresses stay meaningful if that changes.
	constexpr uint32_t PREFERRED_BASE = 0x00400000u;

	inline uint32_t rebase(const uint32_t static_addr) {
		return shared::globals::exe_module_addr + (static_addr - PREFERRED_BASE);
	}

	// --------------
	// game functions. Most are __cdecl, but not all -- BRender's internal helpers are
	// compiled callee-cleans. Check how the target returns (`ret` vs `ret N`) before
	// writing a typedef for one: calling a `ret 4` function through a __cdecl pointer
	// pops the argument twice and walks the stack pointer up through the caller's frame.

	// The race is drawn with the incremental API — BrZbSceneRenderBegin, then repeated
	// BrZbSceneRenderAdd, then BrZbSceneRenderEnd — driven from 0x004E5680 / 0x004E7650.
	// The all-in-one BrZbSceneRender is only used for HUD and menu overlays.
	constexpr uint32_t ADDR_BrZbSceneRenderBegin = 0x00522C80u;
	constexpr uint32_t ADDR_BrZbSceneRenderEnd = 0x00522EB0u;
	constexpr uint32_t ADDR_BrZbSceneRender = 0x00522F30u;

	// Called at the tail of Begin. Once it returns, the renderer's model_to_view holds
	// world_to_view, because no actor transform has been pushed yet.
	constexpr uint32_t ADDR_SceneSetupCameraMatrices = 0x00521C10u;

	// BrZbModelRender(actor, model, material, env, style, bounds_token, use_custom).
	// The renderer's model_to_view is fully accumulated by the time this is entered.
	constexpr uint32_t ADDR_BrZbModelRender = 0x00521890u;

	// BrModelUpdate(model, flags) — builds model->prepared. The authored face array, and with
	// it every face's br_material, is only reachable while this is on the stack.
	constexpr uint32_t ADDR_BrModelUpdate = 0x0051F950u;
	typedef void(__cdecl* BrModelUpdate_t)(br_model* model, uint16_t flags);

	// BrMaterialUpdate(material, flags) — bit 0 is BR_MATU_MAP_TRANSFORM. The funkotronic
	// system calls it every time it animates a material's UV transform, which is what marks
	// a material as unbakeable: scrolling water and flashing signs must stay dynamic.
	constexpr uint32_t ADDR_BrMaterialUpdate = 0x00520E70u;
	typedef void(__cdecl* BrMaterialUpdate_t)(br_material* material, uint16_t flags);

	/*
	 * Frontend_Setup(this) — the frontend coming up, and the only unambiguous "not in a
	 * race" edge the game offers.
	 *
	 * Everything reached from the race is entered through it: finishing, the pause menu via
	 * FrontendEnterFromRace (0x0046D8E0), and the first-run boot. Inferring the same thing
	 * from the scene walk cannot work, because a race change reuses the world actor and the
	 * camera, so nothing the renderer sees distinguishes a new track from the old one.
	 *
	 * __thiscall, so the detour takes `this` in ecx: a __fastcall stub with an unused edx
	 * has the same calling sequence.
	 */
	constexpr uint32_t ADDR_Frontend_Setup = 0x0046D1C0u;
	typedef void(__fastcall* Frontend_Setup_t)(void* self, void* unused);

	// The bits BrMaterialUpdate actually tests, in the order its branches read them
	// (0x00520EEA, 0x00520F0C, 0x005213DD). MAP_TRANSFORM republishes the UV transform;
	// MATERIAL republishes colour, opacity and flags; EXTRA republishes the token list.
	// The last two are how the game animates a sprite's opacity mid-race.
	constexpr uint16_t BR_MATU_MAP_TRANSFORM = 0x0001;
	constexpr uint16_t BR_MATU_MATERIAL = 0x0002;
	constexpr uint16_t BR_MATU_EXTRA = 0x0040;

	// BRender's own translucency test, and the one its device drivers act on: colour_map
	// carries alpha, or the material has an index shade/blend table, or its extra token
	// list requests blending. BrZbModelRender (0x0052196D) calls it to decide whether a
	// model goes into the depth-sorted bucket instead of straight to the rasterizer.
	// Callee-cleans -- both return paths are `ret 4` (0x0051F694, 0x0051F699).
	constexpr uint32_t ADDR_MaterialNeedsAlpha = 0x0051F630u;
	typedef int(__stdcall* MaterialNeedsAlpha_t)(const br_material* material);

	inline bool material_needs_alpha(const br_material* material)
	{
		const auto fn = reinterpret_cast<MaterialNeedsAlpha_t>(rebase(ADDR_MaterialNeedsAlpha));
		return fn(material) != 0;
	}

	// Two pools of pooled quad actors, both built once at startup and recycled for the
	// rest of the session. Each entry holds a br_actor* whose model is the quad.
	struct quad_pool
	{
		uint32_t address;
		uint32_t stride;
		uint32_t count;
	};

	// InitSpillsAndSkids @ 0x004E9C40 -- tyre tracks, oil spills, smears, car shadows.
	// Membership here is what "is a decal" means: nothing else in the game lays a quad
	// flat onto another surface, so this pool is the authority on which geometry needs
	// lifting clear of what it overlays.
	constexpr quad_pool GROUND_DECAL_POOL{ 0x006A27F0u, 0x1Cu, 100u };

	// InitSpriteParticlePool @ 0x004EA880 -- the camera-facing sprite billboards: explosion
	// fire, powerup sparkle, blood clouds, impact "BANG!" marks. One shared pool for every
	// data-driven sprite effect, recycled round-robin across effect types, animated by
	// swapping the slot material's colour_map between frames (AnimateSpriteParticles
	// @ 0x004EAAF0). Never lifted -- a billboard overlays nothing -- but, like the decals,
	// re-placed rather than moved, so never baked. The slots start at 0x006A55C8; the
	// address here is where slot 0 keeps its br_actor*.
	constexpr quad_pool SPRITE_PARTICLE_POOL{ 0x006A55D8u, 0x78u, 50u };

	typedef void(__cdecl* BrZbSceneRender_t)(br_actor* world, br_actor* camera, void* colour, void* depth);
	typedef void(__cdecl* BrZbSceneRenderEnd_t)();
	typedef void(__cdecl* SceneSetupCameraMatrices_t)(br_actor* world, br_actor* camera);
	typedef void(__cdecl* BrZbModelRender_t)(br_actor* actor, br_model* model, void* material, void* env,
	                                         uint32_t style, uint32_t bounds, uint32_t use_custom);

	// br_renderer dispatch, invoked as templateQuery(self, BRT_MATRIX, 0, &count, buf, size, token).
	typedef int(__cdecl* renderer_template_query_t)(void* self, uint32_t part, uint32_t index,
	                                                int* count, void* buffer, uint32_t size, uint32_t token);

	// boundsTest(self, &out_token, bounds) — BrZbActorRender culls the actor when the token
	// comes back BRT_BOUNDS_OUTSIDE. Bounds are model space; the renderer's current
	// model_to_view has already been pushed by the time this is called.
	typedef int(__cdecl* renderer_bounds_test_t)(void* self, uint32_t* out_token, const float* bounds);

	enum br_bounds_result : uint32_t
	{
		BRT_BOUNDS_PARTIAL = 0x113,
		BRT_BOUNDS_INSIDE = 0x114,
		BRT_BOUNDS_OUTSIDE = 0x115,
	};

	// --------------
	// game variables

	// br_renderer* — set by BrRendererBegin (0x005259B0).
	constexpr uint32_t ADDR_g_pRenderer = 0x0079EFECu;

	// br_pixelmap* — the race view's colour target, a sub-pixelmap of the back buffer
	// (created at 0x004E49B7). RenderView (0x004E54F0) opens the main scene on it; the
	// second view (mirror) draws to 0x0068B8A8 and the reflection passes to the 64x64
	// texture at 0x006A22BC, all through the same BrZbSceneRenderBegin.
	constexpr uint32_t ADDR_g_race_view_pixelmap = 0x00762128u;

	inline void* get_renderer() {
		return *reinterpret_cast<void**>(rebase(ADDR_g_pRenderer));
	}

	/*
	 * The live depth-cue block — the race TXT's one fog description, mirrored here by
	 * SetDepthCue (0x00445340) and baked into each fogged material's fog_min / fog_max /
	 * fog_colour by ApplyDepthCueToMaterial (0x004451A0). There is no other scene fog
	 * state in the game: these five ints plus g_yon are everything the depth cue is
	 * computed from, so reading them mirrors what every material was patched with.
	 */
	constexpr uint32_t ADDR_g_fogType = 0x0075D760u; // int: -1 none, 0 dark, 1 fog, 2 colour
	constexpr uint32_t ADDR_g_fogP1 = 0x0075D764u;   // int: fog-start exponent
	constexpr uint32_t ADDR_g_fogP2 = 0x0075D768u;   // int: fog-end exponent
	constexpr uint32_t ADDR_g_fogR = 0x0075D76Cu;    // int 0..255, mode "colour" only
	constexpr uint32_t ADDR_g_fogG = 0x0075D770u;
	constexpr uint32_t ADDR_g_fogB = 0x0075D774u;

	// The "Yon" draw distance the fog exponents scale. Also written into every camera's
	// yon_z when one is created (0x0047DA0B, 0x0047E405).
	constexpr uint32_t ADDR_g_yon = 0x00761F4Cu;     // float

	struct scene_fog
	{
		bool enabled;
		uint32_t colour;     // 0x00RRGGBB
		float min_distance;  // world units at which fog begins
		float max_distance;  // world units at which it saturates
	};

	// The scene's depth cue as linear fog parameters, computed the way
	// ApplyDepthCueToMaterial bakes them into a material.
	scene_fog read_scene_fog();

	/*
	 * Cars. The player's tCar_spec is a global struct, not an allocation: GetCarSpec
	 * (0x004AE7E0) returns the constant for category 0, and BuildCarShadows (0x004E74D0)
	 * loads the same address. Opponents and cops live in two tOpponent_spec arrays with a
	 * shared layout, each entry holding a pointer to its tCar_spec -- the lookup loops at
	 * 0x004A9CEC and 0x004A9D27 prove base, stride and offset. Net players are a third
	 * category this does not read.
	 */
	constexpr uint32_t ADDR_g_player_car = 0x0075BC2Cu;     // tCar_spec
	constexpr uint32_t ADDR_g_opponents = 0x0075D8A0u;      // tOpponent_spec[]
	constexpr uint32_t ADDR_g_num_opponents = 0x0075D7A0u;  // int
	constexpr uint32_t ADDR_g_cops = 0x007609D8u;           // tOpponent_spec[]
	constexpr uint32_t ADDR_g_num_cops = 0x00691744u;       // int
	constexpr uint32_t OPPONENT_SPEC_STRIDE = 0x1A4u;
	constexpr uint32_t OPPONENT_SPEC_CAR = 0x08u;           // tCar_spec*
	constexpr uint32_t MAX_OPPONENT_SPECS = 30u;

	// tCar_spec fields. The master actor's matrix is car-to-world; a car faces down its
	// local -Z with +Y up (0x0041410C negates matrix row 2 into car->direction). The model
	// actor is the loaded car .ACT, linked under the master with an identity transform.
	constexpr uint32_t CAR_MASTER_ACTOR = 0x010u;           // br_actor*
	constexpr uint32_t CAR_KNACKERED = 0x1D4u;              // int, set by KnackerThisCar (0x0043F5F0)
	constexpr uint32_t CAR_MODEL_ACTOR = 0xE0Cu;            // br_actor*

	// gProgram_state.racing -- raised at the top of MainGameLoop (0x00492A5C), dropped on
	// every way out of a race and for the length of the pause frontend (0x00494484).
	constexpr uint32_t ADDR_g_racing = 0x0075BBA8u;         // int

	struct race_car
	{
		const void* spec;           // identity of the car for as long as the race lasts
		const br_actor* master;
		const br_actor* model;      // may be null
		bool is_player;
		bool knackered;
	};

	// Every car in the race whose master actor can be read: the player first, then the
	// opponents, then the cops. Empty outside a race.
	void collect_race_cars(std::vector<race_car>& out);

	// Whether the whole span can be read without faulting. Game pointers that are only
	// valid for part of a race go through this before they are followed.
	bool can_read(const void* p, size_t bytes);

	// ---

	extern void init_game_addresses();
}
