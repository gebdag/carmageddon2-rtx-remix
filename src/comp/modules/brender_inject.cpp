#include "std_include.hpp"
#include "brender_inject.hpp"
#include "shared/common/config.hpp"
#include "shared/common/ffp_state.hpp"
#include "shared/common/remix_api.hpp"

namespace comp
{
	namespace
	{
		// Declared explicitly rather than via D3DFVF_XYZ|NORMAL|TEX1. Remix reports
		// "trying to bind a texture to a mesh without UVs" for the FVF path, so the
		// texcoord usage is spelled out here instead of inferred.
		constexpr D3DVERTEXELEMENT9 INJECT_DECL[] =
		{
			{ 0,  0, D3DDECLTYPE_FLOAT3,  D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
			{ 0, 12, D3DDECLTYPE_FLOAT3,  D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL,   0 },
			{ 0, 24, D3DDECLTYPE_FLOAT2,  D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
			{ 0, 32, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR,   0 },
			D3DDECL_END()
		};

		// A br_angle covers a full turn in 16 bits.
		constexpr float BR_ANGLE_TO_RADIANS = 6.283185307f / 65536.0f;

		constexpr D3DMATRIX IDENTITY_MATRIX =
		{
			{ { 1, 0, 0, 0,
			    0, 1, 0, 0,
			    0, 0, 1, 0,
			    0, 0, 0, 1 } }
		};

		game::BrZbSceneRender_t o_scene_render = nullptr;
		game::BrZbSceneRender_t o_scene_begin = nullptr;
		game::BrZbSceneRenderEnd_t o_scene_end = nullptr;
		game::SceneSetupCameraMatrices_t o_setup_camera = nullptr;
		game::BrZbModelRender_t o_model_render = nullptr;
		game::BrModelUpdate_t o_model_update = nullptr;
		game::renderer_bounds_test_t o_bounds_test = nullptr;

		// BRender and D3D9's fixed-function pipeline both use row vectors (v' = v * M), so a
		// br_matrix34 drops straight into a D3DMATRIX with the fourth column filled in.
		D3DMATRIX to_d3d(const game::br_matrix34& b)
		{
			D3DMATRIX d{};
			for (int row = 0; row < 4; ++row)
			{
				d.m[row][0] = b.m[row][0];
				d.m[row][1] = b.m[row][1];
				d.m[row][2] = b.m[row][2];
				d.m[row][3] = (row == 3) ? 1.0f : 0.0f;
			}
			return d;
		}

		// out = a * b, row-vector convention: applying `out` equals applying a then b.
		void mul34(const game::br_matrix34& a, const game::br_matrix34& b, game::br_matrix34& out)
		{
			for (int row = 0; row < 4; ++row)
			{
				for (int col = 0; col < 3; ++col)
				{
					out.m[row][col] = a.m[row][0] * b.m[0][col]
					                + a.m[row][1] * b.m[1][col]
					                + a.m[row][2] * b.m[2][col]
					                + ((row == 3) ? b.m[3][col] : 0.0f);
				}
			}
		}

		// Inverts the affine transform. BRender lets actors carry scale, so this does a full
		// 3x3 inverse rather than assuming an orthonormal basis.
		bool invert34(const game::br_matrix34& m, game::br_matrix34& out)
		{
			const float a = m.m[0][0], b = m.m[0][1], c = m.m[0][2];
			const float d = m.m[1][0], e = m.m[1][1], f = m.m[1][2];
			const float g = m.m[2][0], h = m.m[2][1], i = m.m[2][2];

			const float A = e * i - f * h;
			const float B = f * g - d * i;
			const float C = d * h - e * g;
			const float det = a * A + b * B + c * C;

			if (fabsf(det) < 1e-12f) {
				return false;
			}

			const float inv_det = 1.0f / det;
			out.m[0][0] = A * inv_det;
			out.m[0][1] = (c * h - b * i) * inv_det;
			out.m[0][2] = (b * f - c * e) * inv_det;
			out.m[1][0] = B * inv_det;
			out.m[1][1] = (a * i - c * g) * inv_det;
			out.m[1][2] = (c * d - a * f) * inv_det;
			out.m[2][0] = C * inv_det;
			out.m[2][1] = (b * g - a * h) * inv_det;
			out.m[2][2] = (a * e - b * d) * inv_det;

			// Translation of the inverse is -t * L^-1.
			for (int col = 0; col < 3; ++col)
			{
				out.m[3][col] = -(m.m[3][0] * out.m[0][col]
				                + m.m[3][1] * out.m[1][col]
				                + m.m[3][2] * out.m[2][col]);
			}
			return true;
		}

		/*
		 * Folds br_material::map_transform into a D3D9 texture-stage matrix.
		 *
		 * BRender's br_matrix23 is a row-vector 2x3 affine UV transform. D3D9's fixed
		 * function pipeline expands a two-component texture coordinate to (u, v, 1) before
		 * multiplying, so the translation row lands in the matrix's third row rather than
		 * its fourth.
		 *
		 * Returns false for a transform that would not change anything, which is the common
		 * case: leaving the stage's transform disabled then keeps Remix's texcoord handling
		 * on the path it is known to work on.
		 */
		bool build_texture_matrix(const game::br_matrix23& b, D3DMATRIX& out)
		{
			out = IDENTITY_MATRIX;
			out.m[0][0] = b.m[0][0]; out.m[0][1] = b.m[0][1];
			out.m[1][0] = b.m[1][0]; out.m[1][1] = b.m[1][1];
			out.m[2][0] = b.m[2][0]; out.m[2][1] = b.m[2][1];

			// A singular linear part means the material was never given a transform. Using
			// it would collapse every texture coordinate onto a single texel.
			constexpr float eps = 1e-6f;
			const float det = b.m[0][0] * b.m[1][1] - b.m[0][1] * b.m[1][0];
			if (fabsf(det) < eps) {
				return false;
			}

			return fabsf(b.m[0][0] - 1.0f) > eps || fabsf(b.m[0][1]) > eps
			    || fabsf(b.m[1][0]) > eps || fabsf(b.m[1][1] - 1.0f) > eps
			    || fabsf(b.m[2][0]) > eps || fabsf(b.m[2][1]) > eps;
		}

		void cross(const float a[3], const float b[3], float out[3])
		{
			out[0] = a[1] * b[2] - a[2] * b[1];
			out[1] = a[2] * b[0] - a[0] * b[2];
			out[2] = a[0] * b[1] - a[1] * b[0];
		}

		bool normalize(float v[3])
		{
			const float length = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
			if (length < 1e-12f) {
				return false;
			}

			v[0] /= length; v[1] /= length; v[2] /= length;
			return true;
		}

		/*
		 * Identity of an actor's placement, taken from the transform chain itself.
		 *
		 * Comparing reconstructed world matrices does not work: model_to_world comes back
		 * through the camera (model_to_view times the view inverse), and at city-scale
		 * translations the float error of that round trip exceeds any workable epsilon --
		 * every static actor read as jittering and nothing ever promoted. Nothing writes a
		 * static actor's transform, so hashing the raw bytes up the parent chain is exact:
		 * same bytes, same placement.
		 *
		 * Placement is the transforms alone. Node addresses are hashed separately because
		 * they answer a different question: where the actor hangs, not where it stands. The
		 * game relinks whole groups of scenery without moving any of it, and folding the
		 * addresses into the placement made every one of those relinks read as movement.
		 */
		struct placement_id
		{
			uint64_t where;   // transform bytes up the chain
			uint64_t parent;  // chain node addresses
		};

		placement_id placement_fingerprint(const game::br_actor* actor)
		{
			placement_id id{ 1469598103934665603ull, 1469598103934665603ull };
			const auto mix = [](uint64_t& hash, const void* data, const size_t bytes)
			{
				const auto p = static_cast<const uint8_t*>(data);
				for (size_t i = 0; i < bytes; ++i) {
					hash = (hash ^ p[i]) * 1099511628211ull;
				}
			};

			int depth = 0;
			for (const game::br_actor* node = actor; node && depth < 32; node = node->parent, ++depth)
			{
				mix(id.where, &node->t_type, sizeof(node->t_type));
				mix(id.where, &node->t, sizeof(node->t));
				mix(id.parent, &node, sizeof(node));
			}

			return id;
		}

		// Noncars get chunks of their own: a hit noncar is punched out of its chunk, and
		// keeping that write away from the pristine world chunks is what keeps *their*
		// geometry hashes immutable for Remix modding. Opacity joins the key because a chunk
		// draws under one texture factor, so runs faded to different degrees cannot share it.
		uint64_t chunk_key(const IDirect3DTexture9* texture, const bool has_alpha,
			const bool noncar, const uint8_t opacity)
		{
			return reinterpret_cast<uintptr_t>(texture)
				| (static_cast<uint64_t>(opacity) << 32)
				| (has_alpha ? 1ull << 63 : 0ull)
				| (noncar ? 1ull << 62 : 0ull);
		}

		// Asks the live renderer for its current model_to_view. Mirrors the call
		// SceneSetupCameraMatrices makes at 0x00521DA9.
		bool query_model_to_view(game::br_matrix34& out)
		{
			void* rend = game::get_renderer();
			if (!rend) {
				return false;
			}

			const auto dispatch = *reinterpret_cast<uint8_t**>(rend);
			if (!dispatch) {
				return false;
			}

			const auto query = *reinterpret_cast<game::renderer_template_query_t*>(
				dispatch + game::RD_TEMPLATE_QUERY);
			if (!query) {
				return false;
			}

			int count = 0;
			return query(rend, game::BRT_MATRIX, 0, &count, &out, sizeof(out), game::BRT_MODEL_TO_VIEW) == 0;
		}

		int64_t now_ticks()
		{
			LARGE_INTEGER t{};
			QueryPerformanceCounter(&t);
			return t.QuadPart;
		}

		/*
		 * Regions readable() has already confirmed during the current scene.
		 *
		 * VirtualQuery is a syscall and readable() sits on paths that run per part, per
		 * model, per frame -- several thousand times a scene. The regions it reports back
		 * are few and large, because materials, models and the decal pools all live in a
		 * handful of heap blocks, so a confirmed region answers most of the calls that
		 * follow it. Cleared at the start of every scene: a block freed between frames is
		 * re-queried rather than trusted.
		 */
		std::vector<std::pair<const uint8_t*, const uint8_t*>> g_readable_regions;

		void forget_readable_regions()
		{
			g_readable_regions.clear();
		}

		// A pointer is only worth dereferencing if the whole span is committed and readable.
		bool readable(const void* p, const size_t bytes)
		{
			if (!p || reinterpret_cast<uintptr_t>(p) < 0x10000) {
				return false;
			}

			const auto first = static_cast<const uint8_t*>(p);
			const auto last = first + bytes;

			for (const auto& [begin, end] : g_readable_regions)
			{
				if (first >= begin && last <= end) {
					return true;
				}
			}

			MEMORY_BASIC_INFORMATION mbi{};
			if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) {
				return false;
			}

			constexpr DWORD readable_mask = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY
				| PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
			if (!(mbi.Protect & readable_mask) || (mbi.Protect & PAGE_GUARD)) {
				return false;
			}

			const auto end = static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
			if (last > end) {
				return false;
			}

			g_readable_regions.emplace_back(static_cast<const uint8_t*>(mbi.BaseAddress), end);
			return true;
		}

		/*
		 * The opacity BRender would hand its device driver for this material, 0..255.
		 *
		 * BrMaterialUpdate publishes br_material::opacity as BRT_OPACITY_F after scaling it
		 * by 1/255 (0x00520F24) and turns blending on whenever it is below 255 (0x00520F6D).
		 * A material's extra token list can carry the same quantity, and that is what the
		 * game animates: the smoke renderer rewrites the shared list's value with
		 * alpha * 150 before every particle (0x004FB258) and the tint overlay writes 128
		 * (0x0045AAB4). Both land in the integer part of the fixed form, so the two token
		 * spellings differ only in how the byte is encoded.
		 *
		 * The list is walked defensively: 'Acc Poly Mat' shares its terminator with the data
		 * that follows it, so a malformed list has to end the walk rather than run off.
		 */
		uint8_t material_opacity(const game::br_material* material)
		{
			if (!material) {
				return 255;
			}

			uint32_t opacity = material->opacity;

			constexpr int MAX_TOKENS = 32;
			const game::br_token_value* token = material->extra;
			for (int i = 0; i < MAX_TOKENS && readable(token, sizeof(*token)) && token->token; ++i, ++token)
			{
				if (token->token == game::BRT_OPACITY_X) {
					opacity = token->value >> 16;
				}
				else if (token->token == game::BRT_OPACITY_F)
				{
					float scalar = 0.0f;
					std::memcpy(&scalar, &token->value, sizeof(scalar));
					opacity = static_cast<uint32_t>(std::clamp(scalar, 0.0f, 1.0f) * 255.0f + 0.5f);
				}
			}

			return static_cast<uint8_t>(std::min(opacity, 255u));
		}

		/*
		 * Flattens a BRender palette pixelmap into 256 ARGB entries.
		 *
		 * A palette is an ordinary pixelmap whose pixels are the colour table, so it carries
		 * its own type -- DRRENDER.PAL loads as one row of 32-bit entries. Entries the
		 * palette does not define stay transparent black, which is what BRender's own
		 * conversion leaves them as.
		 */
		bool build_palette(const game::br_pixelmap* pm, std::array<uint32_t, 256>& out)
		{
			if (!readable(pm, sizeof(*pm))) {
				return false;
			}

			uint32_t bpp = 0;
			switch (pm->type)
			{
			case game::BR_PMT_RGB_888:   bpp = 3; break;
			case game::BR_PMT_RGBX_888:
			case game::BR_PMT_RGBA_8888: bpp = 4; break;
			default: return false;
			}

			const uint32_t entries = std::min<uint32_t>(
				static_cast<uint32_t>(pm->width) * pm->height, 256u);
			if (entries == 0 || pm->row_bytes < pm->width * bpp
				|| !readable(pm->pixels, static_cast<size_t>(pm->row_bytes) * pm->height)) {
				return false;
			}

			const auto base = static_cast<const uint8_t*>(pm->pixels);
			for (uint32_t i = 0; i < entries; ++i)
			{
				const uint8_t* src = base + (i / pm->width) * pm->row_bytes + (i % pm->width) * bpp;
				const uint32_t a = pm->type == game::BR_PMT_RGBA_8888 ? src[3] : 255u;
				out[i] = (a << 24) | (src[2] << 16) | (src[1] << 8) | src[0];
			}

			return true;
		}

		// BRender writes the palette index into the top byte of a prepared vertex colour
		// (0x0051FB70); only the low three bytes are the colour. D3DCOLOR wants the alpha
		// there instead, and opacity travels as a render state rather than per vertex.
		uint32_t to_d3d_colour(const uint32_t br_colour)
		{
			return 0xFF000000u | (br_colour & 0x00FFFFFFu);
		}

		/*
		 * Whether a model is one of the game's pooled decal quads.
		 *
		 * Translucency is not the test. Glass, smoke and water are translucent too, and none
		 * of them overlays a surface it could fight with -- lifting those off their own
		 * normals is what pulls a windscreen out of its frame. The two pools are small,
		 * fixed, and built once at startup, so they are walked directly rather than cached:
		 * this only runs when a model is first seen, and nothing can then go stale.
		 */
		bool is_decal_model(const game::br_model* model)
		{
			if (!model) {
				return false;
			}

			for (const game::decal_pool& pool : { game::GROUND_DECAL_POOL, game::IMPACT_DECAL_POOL })
			{
				const auto entries = reinterpret_cast<const uint8_t*>(game::rebase(pool.address));
				if (!readable(entries, static_cast<size_t>(pool.stride) * pool.count)) {
					continue;
				}

				for (uint32_t i = 0; i < pool.count; ++i)
				{
					const auto actor = *reinterpret_cast<game::br_actor* const*>(entries + i * pool.stride);
					if (readable(actor, sizeof(game::br_actor)) && actor->model == model) {
						return true;
					}
				}
			}

			return false;
		}

		/*
		 * Whether the game removes this actor rather than ever moving it.
		 *
		 * Holding still proves an actor is not being driven, but it says nothing about one
		 * that is about to be deleted, and a chunk cannot give geometry back. Powerup
		 * pickups disappear the instant they are taken -- a pickup is any actor whose
		 * identifier carries 0xA3 ('£') as its second character, the test
		 * SpecialActorEnumCallback (0x0040D1F0) uses -- and decal quads are pooled and
		 * recycled at a new placement rather than moved to it.
		 */
		bool vanishes_outright(const game::br_actor* actor, const game::br_model* model)
		{
			const char* name = actor->identifier;
			const bool pickup = readable(name, 2)
				&& name[0] && static_cast<uint8_t>(name[1]) == 0xA3;

			return pickup || is_decal_model(model);
		}

		// bounds is min[3] then max[3] in model space. The camera sits at the origin in view
		// space, so the transformed centre is already the camera-relative position.
		bool inside_camera_bubble(const float* bounds, const float radius)
		{
			game::br_matrix34 model_to_view{};
			if (!query_model_to_view(model_to_view)) {
				return false;
			}

			const float cx = (bounds[0] + bounds[3]) * 0.5f;
			const float cy = (bounds[1] + bounds[4]) * 0.5f;
			const float cz = (bounds[2] + bounds[5]) * 0.5f;

			float view[3];
			for (int col = 0; col < 3; ++col)
			{
				view[col] = cx * model_to_view.m[0][col]
				          + cy * model_to_view.m[1][col]
				          + cz * model_to_view.m[2][col]
				          + model_to_view.m[3][col];
			}

			const float ex = (bounds[3] - bounds[0]) * 0.5f;
			const float ey = (bounds[4] - bounds[1]) * 0.5f;
			const float ez = (bounds[5] - bounds[2]) * 0.5f;
			const float extent = sqrtf(ex * ex + ey * ey + ez * ez);

			const float distance = sqrtf(view[0] * view[0] + view[1] * view[1] + view[2] * view[2]);
			return distance - extent <= radius;
		}

		// BrZbActorRender drops any actor this reports as outside, which for path tracing
		// removes exactly the geometry that should still occlude and bounce light: walls
		// behind and beside the camera. Rejected actors are downgraded to PARTIAL rather
		// than INSIDE so BRender still clips whatever it ends up rasterizing itself.
		// DisableFrustum keeps everything; the bubble keeps a radius around the camera.
		int __cdecl hk_bounds_test(void* self, uint32_t* out_token, const float* bounds)
		{
			const int result = o_bounds_test(self, out_token, bounds);

			if (!out_token || *out_token != game::BRT_BOUNDS_OUTSIDE || !bounds) {
				return result;
			}

			const auto& culling = shared::common::config::get().culling;
			if (culling.disable_frustum)
			{
				*out_token = game::BRT_BOUNDS_PARTIAL;
				return result;
			}

			if (culling.bubble_radius <= 0.0f) {
				return result;
			}

			const auto inject = brender_inject::get();
			const int64_t start = now_ticks();
			const bool keep = inside_camera_bubble(bounds, culling.bubble_radius);
			if (inject) {
				inject->profile().bounds_ticks += now_ticks() - start;
			}

			if (keep) {
				*out_token = game::BRT_BOUNDS_PARTIAL;
			}

			return result;
		}

		void __cdecl hk_scene_begin(game::br_actor* world, game::br_actor* camera, void* colour, void* depth)
		{
			// Cameras are built once by FUN_0047E3B0, which bakes the "Yon" option into
			// br_camera::yon_z (the mirror camera gets half). Rewriting it here each frame
			// is what makes the override stick, and it reaches the game's own frustum
			// culling as well as the projection we hand Remix.
			if (const float far_plane = shared::common::config::get().culling.far_plane;
				far_plane > 0.0f && camera && camera->type_data)
			{
				const auto cam = static_cast<game::br_camera*>(camera->type_data);
				if (cam->yon_z < far_plane) {
					cam->yon_z = far_plane;
				}
			}

			if (const auto self = brender_inject::get(); self) {
				self->begin_scene(world, camera);
			}

			o_scene_begin(world, camera, colour, depth);
		}

		void __cdecl hk_setup_camera(game::br_actor* world, game::br_actor* camera)
		{
			o_setup_camera(world, camera);

			// No actor transform has been pushed yet, so model_to_view == world_to_view.
			if (const auto self = brender_inject::get(); self) {
				self->capture_camera();
			}
		}

		/*
		 * BrZbSceneRenderEnd is where BRender sorts its buckets and rasterizes them, which is
		 * also where nGlide receives the frame's triangles and pushes them across the bridge.
		 * None of that is BrZbModelRender, so none of it was covered by the game timer -- it
		 * fell into the unattributed remainder, and it is the part that scales with draw
		 * distance.
		 */
		void __cdecl hk_scene_end()
		{
			const int64_t start = now_ticks();
			o_scene_end();
			const int64_t elapsed = now_ticks() - start;

			if (const auto self = brender_inject::get(); self)
			{
				self->profile().scene_end_ticks += elapsed;
				self->end_scene();
			}
		}

		// The all-in-one entry point. It calls Begin itself but inlines the End, so the
		// scene has to be closed here, and it is flagged as an overlay pass throughout.
		void __cdecl hk_scene_render(game::br_actor* world, game::br_actor* camera, void* colour, void* depth)
		{
			const auto self = brender_inject::get();
			if (self) {
				self->set_overlay_scene(true);
			}

			const int64_t start = now_ticks();
			o_scene_render(world, camera, colour, depth);
			const int64_t elapsed = now_ticks() - start;

			if (self) {
				self->end_scene();
				self->set_overlay_scene(false);
				self->add_overlay_ticks(elapsed);
			}
		}

		void __cdecl hk_model_render(game::br_actor* actor, game::br_model* model, void* material, void* env,
		                             uint32_t style, uint32_t bounds, uint32_t use_custom)
		{
			const auto self = brender_inject::get();
			bool injected = false;

			if (self && model)
			{
				const int64_t start = now_ticks();
				injected = self->capture_model(actor, model, static_cast<game::br_material*>(material), style);
				self->profile().capture_ticks += now_ticks() - start;
			}

			// The original transforms and lights this model on the CPU and hands the result to
			// nGlide, which Remix then discards as pre-transformed. Once the model has been
			// injected in model space, none of that reaches the screen.
			if (injected && shared::common::config::get().optimization.suppress_game_render) {
				return;
			}

			const int64_t start = now_ticks();
			o_model_render(actor, model, material, env, style, bounds, use_custom);

			if (self) {
				self->profile().game_render_ticks += now_ticks() - start;
			}
		}

		void __cdecl hk_model_update(game::br_model* model, uint16_t flags)
		{
			if (const auto self = brender_inject::get(); self && model)
			{
				const int64_t start = now_ticks();

				// Must run first: the original frees the authored face array on the way out.
				self->learn_materials(model);
				self->invalidate_geometry(model);
				self->note_model_rebuilt(model);

				self->profile().capture_ticks += now_ticks() - start;
				++self->profile().model_updates;
			}

			o_model_update(model, flags);
		}

		game::BrMaterialUpdate_t o_material_update = nullptr;

		void __cdecl hk_material_update(game::br_material* material, uint16_t flags)
		{
			if (const auto self = brender_inject::get(); self && material) {
				self->on_material_update(material, flags);
			}

			o_material_update(material, flags);
		}

		game::Frontend_Setup_t o_frontend_setup = nullptr;

		void __fastcall hk_frontend_setup(void* self_ptr, void* unused)
		{
			if (const auto self = brender_inject::get()) {
				self->on_frontend_entered();
			}

			o_frontend_setup(self_ptr, unused);
		}

		bool install(const uint32_t addr, void* stub, void** original, const char* name)
		{
			if (shared::utils::hook::detour(game::rebase(addr), stub, original)) {
				return true;
			}

			shared::common::log("BRender", std::format("failed to hook {} @ 0x{:08X}", name, addr),
				shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
			return false;
		}
	}

	brender_inject::brender_inject()
	{
		p_this = this;

		m_queue.reserve(2048);

		LARGE_INTEGER frequency{};
		QueryPerformanceFrequency(&frequency);
		m_ticks_per_ms = static_cast<double>(frequency.QuadPart) / 1000.0;

		bool ok = install(game::ADDR_BrZbSceneRenderBegin, hk_scene_begin,
			reinterpret_cast<void**>(&o_scene_begin), "BrZbSceneRenderBegin");
		ok &= install(game::ADDR_SceneSetupCameraMatrices, hk_setup_camera,
			reinterpret_cast<void**>(&o_setup_camera), "SceneSetupCameraMatrices");
		ok &= install(game::ADDR_BrZbSceneRenderEnd, hk_scene_end,
			reinterpret_cast<void**>(&o_scene_end), "BrZbSceneRenderEnd");
		ok &= install(game::ADDR_BrZbSceneRender, hk_scene_render,
			reinterpret_cast<void**>(&o_scene_render), "BrZbSceneRender");
		ok &= install(game::ADDR_BrZbModelRender, hk_model_render,
			reinterpret_cast<void**>(&o_model_render), "BrZbModelRender");
		ok &= install(game::ADDR_BrModelUpdate, hk_model_update,
			reinterpret_cast<void**>(&o_model_update), "BrModelUpdate");
		ok &= install(game::ADDR_BrMaterialUpdate, hk_material_update,
			reinterpret_cast<void**>(&o_material_update), "BrMaterialUpdate");
		ok &= install(game::ADDR_Frontend_Setup, hk_frontend_setup,
			reinterpret_cast<void**>(&o_frontend_setup), "Frontend_Setup");

		if (ok)
		{
			const auto& cfg = shared::common::config::get();
			shared::common::log("BRender", std::format(
				"Hooked the BRender scene walk - model-space injection armed. Static world {},"
				" frustum culling {}, game render {}, translucent pass {}, texture transform {},"
				" sparks {}, vertex colour {}, material opacity {}, fog {}, decal offset {:.3f},"
				" spark width {:.3f}.",
				cfg.optimization.static_world ? "on" : "OFF",
				cfg.culling.disable_frustum ? "DISABLED" : "on",
				cfg.optimization.suppress_game_render ? "suppressed" : "ON",
				cfg.effects.translucent_pass ? "on" : "OFF",
				cfg.effects.texture_transform ? "on" : "OFF",
				cfg.effects.sparks ? "on" : "OFF",
				cfg.effects.vertex_colour ? "on" : "OFF",
				cfg.effects.material_opacity ? "on" : "OFF",
				cfg.effects.fog ? "on" : "OFF",
				cfg.effects.decal_offset, cfg.effects.spark_width),
				shared::common::LOG_TYPE::LOG_TYPE_GREEN, true);
		}
	}

	brender_inject::~brender_inject()
	{
		release_chunks();
		release_transient();

		for (auto& [model, geometry] : m_geometry) {
			release_geometry(geometry);
		}

		for (auto& [pm, cached] : m_textures) {
			if (cached.texture) { cached.texture->Release(); }
		}

		for (auto& [rgb, texture] : m_spark_textures) {
			if (texture) { texture->Release(); }
		}

		for (auto& [rgb, texture] : m_colour_textures) {
			if (texture) { texture->Release(); }
		}

		if (m_white_texture) { m_white_texture->Release(); }
		if (m_vertex_decl) { m_vertex_decl->Release(); }
		if (m_saved_state) { m_saved_state->Release(); }
	}

	// Deferred because the renderer object does not exist until BrRendererBegin has run,
	// which is long after this module is constructed.
	void brender_inject::install_bounds_test_hook()
	{
		if (m_bounds_hook_attempted) {
			return;
		}

		void* rend = game::get_renderer();
		if (!rend) {
			return;
		}

		m_bounds_hook_attempted = true;

		const auto dispatch = *reinterpret_cast<uint8_t**>(rend);
		const auto target = *reinterpret_cast<void**>(dispatch + game::RD_BOUNDS_TEST);

		if (shared::utils::hook::detour(reinterpret_cast<DWORD>(target), hk_bounds_test,
			reinterpret_cast<void**>(&o_bounds_test)))
		{
			MH_EnableHook(target);
			const auto& culling = shared::common::config::get().culling;
			shared::common::log("BRender", std::format("bounds test hooked at {:#010x} - {}",
				reinterpret_cast<uint32_t>(target),
				culling.disable_frustum ? "frustum culling disabled"
					: std::format("bubble radius {:.1f}", culling.bubble_radius)),
				shared::common::LOG_TYPE::LOG_TYPE_GREEN, true);
		}
		else {
			shared::common::log("BRender", "failed to hook the renderer bounds test - game culling stays on",
				shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
		}
	}

	void brender_inject::learn_materials(game::br_model* model)
	{
		const auto faces = static_cast<const game::br_face*>(model->faces);
		if (!faces || !model->nfaces) {
			return;
		}

		for (uint16_t f = 0; f < model->nfaces; ++f)
		{
			game::br_material* mat = faces[f].material;
			if (!mat) {
				continue;
			}

			// Key on both, because it is the stored token the prepared group records and the
			// material pointer that a differently-built group would hold.
			m_materials[reinterpret_cast<uint32_t>(mat)] = mat;
			if (mat->stored) {
				m_materials[mat->stored] = mat;
			}
		}
	}

	void brender_inject::invalidate_geometry(game::br_model* model)
	{
		// Erasing here would destroy a map node that the current scene's queue may already
		// point at -- BrModelUpdate runs mid-scene for deforming cars. The rebuild is
		// deferred to the next capture of this model instead.
		if (const auto it = m_geometry.find(model); it != m_geometry.end()) {
			it->second.dirty = true;
		}

		// An actor baked with the old shape must not keep showing it. Cars never land here
		// -- they are never baked -- but a crushed noncar does.
		for (auto& [actor, record] : m_actors)
		{
			if (record.baked && record.model == model) {
				if (unbake_actor(record)) { ++m_demotions.deformed; }
			}
		}
	}

	void brender_inject::release_geometry(model_geometry& geometry)
	{
		if (geometry.vertex_buffer) { geometry.vertex_buffer->Release(); }
		if (geometry.index_buffer) { geometry.index_buffer->Release(); }
		geometry.vertex_buffer = nullptr;
		geometry.index_buffer = nullptr;
		geometry.vertex_bytes = 0;
		geometry.index_bytes = 0;
	}

	void brender_inject::evict_stale_geometry()
	{
		for (auto it = m_geometry.begin(); it != m_geometry.end(); )
		{
			if (m_scenes_submitted - it->second.last_used_scene > GEOMETRY_EVICT_AFTER_SCENES)
			{
				release_geometry(it->second);
				it = m_geometry.erase(it);
			}
			else {
				++it;
			}
		}
	}

	/*
	 * Uploads a model's prepared geometry once and merges its groups by material.
	 *
	 * BRender splits a model into one group per material run, which for track blocks means
	 * up to 37 groups averaging a dozen vertices. Submitting those individually cost ~4000
	 * draws per frame for ~50k vertices, and DrawIndexedPrimitiveUP marshalled the vertex
	 * data through the Remix bridge on every one of them. Static buffers keep the geometry
	 * hash stable so Remix reuses its acceleration structures, and merging collapses the
	 * groups into one draw per distinct material.
	 */
	brender_inject::model_geometry* brender_inject::geometry_for(IDirect3DDevice9* dev,
		game::br_model* model, game::br_material* fallback_material)
	{
		const auto it = m_geometry.find(model);
		if (it != m_geometry.end())
		{
			model_geometry& cached = it->second;
			cached.last_used_scene = m_scenes_submitted;

			// The pointer is the same but the mesh behind it is not, so whatever this entry
			// holds belongs to a model the game has since freed.
			if (!(cached.identity == identify(model))) {
				cached.dirty = true;
			}

			if (!cached.dirty) {
				return cached.vertex_buffer ? &cached : nullptr;
			}

			// The game rewrites this model between renders of it and the queue is already
			// holding the previous instance. Overwriting the buffers now would give every
			// instance in the scene the last one's contents.
			if (cached.queued_scene == m_scene_walks) {
				return transient_geometry(dev, model, fallback_material);
			}

			++m_profile.rebuilds;
		}

		model_geometry& slot = m_geometry[model];
		return build_geometry(dev, model, fallback_material, slot) ? &slot : nullptr;
	}

	bool brender_inject::build_geometry(IDirect3DDevice9* dev, game::br_model* model,
		game::br_material* fallback_material, model_geometry& into)
	{
		std::vector<ffp_vertex> vertices;
		std::vector<geometry_part> parts;
		std::vector<uint32_t> indices;

		model_geometry geometry{};
		geometry.identity = identify(model);
		geometry.last_used_scene = m_scenes_submitted;
		geometry.queued_scene = into.queued_scene;

		// 16-bit indices are enough for any single model this game ships; anything larger is
		// corrupt data rather than a real mesh.
		if (!extract_geometry(dev, model, fallback_material, vertices, parts, indices)
			|| vertices.size() > 0xFFFF)
		{
			release_geometry(into);
			into = std::move(geometry);
			return false;
		}

		const UINT vertex_bytes = static_cast<UINT>(vertices.size() * sizeof(ffp_vertex));
		const UINT index_bytes = static_cast<UINT>(indices.size() * sizeof(uint16_t));

		/*
		 * Damage reshapes a car through BrModelUpdate without changing how big it is, so a
		 * rebuild nearly always wants the buffers it already has. Trading a managed pair for
		 * an identical pair every time churns the pool and stalls on the driver; refilling
		 * one that already fits does neither.
		 */
		if (into.vertex_bytes == vertex_bytes && into.index_bytes == index_bytes)
		{
			geometry.vertex_buffer = std::exchange(into.vertex_buffer, nullptr);
			geometry.index_buffer = std::exchange(into.index_buffer, nullptr);
		}
		else
		{
			release_geometry(into);

			if (FAILED(dev->CreateVertexBuffer(vertex_bytes, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED,
				&geometry.vertex_buffer, nullptr))
				|| FAILED(dev->CreateIndexBuffer(index_bytes, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16,
					D3DPOOL_MANAGED, &geometry.index_buffer, nullptr)))
			{
				release_geometry(geometry);
				into = std::move(geometry);
				return false;
			}
		}

		geometry.vertex_bytes = vertex_bytes;
		geometry.index_bytes = index_bytes;

		void* mapped = nullptr;
		if (SUCCEEDED(geometry.vertex_buffer->Lock(0, vertex_bytes, &mapped, 0)))
		{
			memcpy(mapped, vertices.data(), vertex_bytes);
			geometry.vertex_buffer->Unlock();
		}

		if (SUCCEEDED(geometry.index_buffer->Lock(0, index_bytes, &mapped, 0)))
		{
			auto dst = static_cast<uint16_t*>(mapped);
			for (size_t i = 0; i < indices.size(); ++i) {
				dst[i] = static_cast<uint16_t>(indices[i]);
			}
			geometry.index_buffer->Unlock();
		}

		geometry.parts = std::move(parts);
		geometry.vertex_count = static_cast<uint32_t>(vertices.size());

		into = std::move(geometry);
		return into.vertex_buffer != nullptr;
	}

	/*
	 * A copy of a model's geometry that belongs to this scene alone.
	 *
	 * Carmageddon 2's smoke draws thirty-five particles from one four-vertex quad, rewriting
	 * its vertex colours between each one (0x004FB289) -- the model is a stencil, not a mesh.
	 * Every instance therefore needs its own buffers, but allocating them per particle per
	 * frame would churn the managed pool, so the entries are pooled and recycled at the start
	 * of each scene. Their buffers are reused in place whenever the size matches, which for a
	 * fixed-size stencil is always.
	 */
	brender_inject::model_geometry* brender_inject::transient_geometry(IDirect3DDevice9* dev,
		game::br_model* model, game::br_material* fallback_material)
	{
		if (m_transient_used == m_transient.size()) {
			m_transient.emplace_back();
		}

		model_geometry& slot = m_transient[m_transient_used++];
		++m_profile.rebuilds;
		return build_geometry(dev, model, fallback_material, slot) ? &slot : nullptr;
	}

	void brender_inject::release_transient()
	{
		for (auto& geometry : m_transient) {
			release_geometry(geometry);
		}
		m_transient.clear();
		m_transient_used = 0;
	}

	/*
	 * Re-reads the one piece of material state the game animates per frame.
	 *
	 * The funkotronic system rewrites br_material::map_transform to pick one cell out of a
	 * texture atlas -- that is how a car's rear-light panel switches between off, braking,
	 * reversing and both -- so it cannot be resolved once when the geometry is built.
	 * Translucency stays with the geometry, because it decides how far the vertices are
	 * lifted off the surface they overlay.
	 *
	 * Only ever called from capture_model, which runs inside BrZbModelRender: the materials
	 * this model renders with are necessarily still alive there. Submit works off the values
	 * left behind and never touches the game's memory.
	 */
	void brender_inject::refresh_part_state(model_geometry& geometry) const
	{
		const auto& effects = shared::common::config::get().effects;

		geometry.has_opaque = false;
		geometry.has_blended = false;

		for (auto& part : geometry.parts)
		{
			if (part.material && readable(part.material, sizeof(game::br_material)))
			{
				if (effects.texture_transform)
				{
					part.texture_transform_active =
						build_texture_matrix(part.material->map_transform, part.texture_transform);
				}

				if (effects.material_opacity) {
					part.opacity = material_opacity(part.material);
				}
			}

			if (part_is_blended(part)) { geometry.has_blended = true; }
			else { geometry.has_opaque = true; }
		}
	}

	bool brender_inject::extract_geometry(IDirect3DDevice9* dev, game::br_model* model,
		game::br_material* fallback_material, std::vector<ffp_vertex>& vertices,
		std::vector<geometry_part>& parts, std::vector<uint32_t>& indices)
	{
		const auto prepared = model->prepared;
		if (!prepared || !prepared->groups || !prepared->ngroups) {
			return false;
		}

		// Resolve every group's material up front so groups can be ordered by it.
		struct group_ref
		{
			const game::v1_group* group;
			game::br_material* material;
			uint32_t vertex_base;
			bool needs_alpha;
			bool prelit;
			uint8_t opacity;
		};
		std::vector<group_ref> groups;
		groups.reserve(prepared->ngroups);

		const uint32_t vertex_base = static_cast<uint32_t>(vertices.size());
		uint32_t total_vertices = 0;

		for (uint16_t g = 0; g < prepared->ngroups; ++g)
		{
			const game::v1_group& group = prepared->groups[g];
			if (!group.vertices || !group.faces || !group.nvertices || !group.nfaces) {
				continue;
			}

			// BrModelUpdate writes a zero token for faces that carry no material of their
			// own -- wheels and car shells among them. Those inherit whatever material the
			// render call was given, which is the argument BrZbModelRender received.
			game::br_material* material = fallback_material;
			if (group.material_token != 0) {
				if (const auto it = m_materials.find(group.material_token); it != m_materials.end()) {
					material = it->second;
				}
			}

			const bool live_material = material && readable(material, sizeof(game::br_material));
			const bool needs_alpha = live_material && game::material_needs_alpha(material);

			// With BR_MATF_PRELIT the authored vertex colours are the surface colour and
			// BRender uses them as they stand. Without it BRender lights the model itself,
			// and carrying its result over would double up with Remix's own lighting.
			const auto& effects = shared::common::config::get().effects;
			const bool prelit = live_material
				&& (material->flags & game::BR_MATF_PRELIT)
				&& effects.vertex_colour;

			const uint8_t opacity = live_material && effects.material_opacity
				? material_opacity(material) : uint8_t{ 255 };

			if (prelit || opacity < 255) {
				note_shaded_material(material, prelit, opacity);
			}

			groups.push_back({ &group, material, vertex_base + total_vertices,
			                   needs_alpha, prelit, opacity });
			total_vertices += group.nvertices;
		}

		if (groups.empty() || total_vertices == 0) {
			return false;
		}

		vertices.resize(vertex_base + total_vertices);

		// Tyre tracks, shadows and impact smears are quads laid flat on the surface they mark.
		// BRender kept them out of it by depth-sorting them into a bucket drawn after the
		// road; path tracing has no draw order to lean on, so they are lifted clear of the
		// surface instead. Only the game's own decal quads are moved: displacing a mesh along
		// its own normals deforms it, which on a closed shell like a windscreen shrinks the
		// glass out of the frame it is supposed to fill.
		const float lift = is_decal_model(model)
			? shared::common::config::get().effects.decal_offset
			: 0.0f;

		for (const auto& ref : groups)
		{
			// BrModelUpdate fills this from br_vertex::red/green/blue whenever it is asked
			// for BR_MODU_VERTEX_COLOURS (0x0051FB66), which is every time the smoke system
			// recolours a particle. Null means the model was prepared without colours.
			const uint32_t* colours = ref.prelit && readable(ref.group->vertex_colours,
				sizeof(uint32_t) * ref.group->nvertices) ? ref.group->vertex_colours : nullptr;

			for (uint16_t v = 0; v < ref.group->nvertices; ++v)
			{
				const game::v1_online_vertex& src = ref.group->vertices[v];
				ffp_vertex& dst = vertices[ref.vertex_base + v];
				// BrModelUpdate subtracts the pivot when it builds the prepared block;
				// adding it back restores true model space.
				dst.x = src.px + model->pivot.v[0] + src.nx * lift;
				dst.y = src.py + model->pivot.v[1] + src.ny * lift;
				dst.z = src.pz + model->pivot.v[2] + src.nz * lift;
				dst.nx = src.nx;
				dst.ny = src.ny;
				dst.nz = src.nz;
				dst.u = src.u;
				dst.v = src.v;
				dst.diffuse = colours ? to_d3d_colour(colours[v]) : 0xFFFFFFFFu;
			}
		}

		// Emit indices grouped by material so each material forms one contiguous draw.
		std::vector<group_ref> ordered;
		for (const auto& ref : groups)
		{
			const auto seen = std::find_if(ordered.begin(), ordered.end(),
				[&](const group_ref& o) { return o.material == ref.material; });

			if (seen == ordered.end()) {
				ordered.push_back(ref);
			}
		}

		for (const group_ref& entry : ordered)
		{
			game::br_material* material = entry.material;
			IDirect3DTexture9* texture = texture_for(dev, material);

			// An untextured material is not a failure -- it is a flat colour, held in
			// br_material::colour. Feeding Remix a solid swatch gives it a real albedo and
			// a distinct hash per colour, so the swatch stays replaceable.
			if ((!texture || texture == m_white_texture) && material)
			{
				texture = solid_colour_texture(dev, material->colour & 0x00FFFFFFu);
				note_flat_colour(model, material);
			}
			else if (!texture || texture == m_white_texture) {
				note_untextured(model, nullptr);
			}

			geometry_part part{};
			part.texture = texture;
			part.material = material;
			part.has_alpha = entry.needs_alpha;
			part.texture_transform = IDENTITY_MATRIX;
			part.opacity = entry.opacity;
			part.index_start = static_cast<uint32_t>(indices.size());

			for (const auto& ref : groups)
			{
				if (ref.material != material) {
					continue;
				}

				for (uint16_t f = 0; f < ref.group->nfaces; ++f)
				{
					const game::v1_online_face& face = ref.group->faces[f];
					indices.push_back(ref.vertex_base + face.v[0]);
					indices.push_back(ref.vertex_base + face.v[1]);
					indices.push_back(ref.vertex_base + face.v[2]);
				}
			}

			part.triangle_count = (static_cast<uint32_t>(indices.size()) - part.index_start) / 3u;
			if (part.triangle_count) {
				parts.push_back(part);
			}
		}

		return !parts.empty();
	}

	brender_inject::model_identity brender_inject::identify(const game::br_model* model)
	{
		return { model->prepared, model->vertices, model->faces,
		         model->nvertices, model->nfaces };
	}

	brender_inject::pixelmap_identity brender_inject::identify(const game::br_pixelmap* pm)
	{
		return { pm->pixels, pm->map, pm->row_bytes, pm->width, pm->height, pm->type };
	}

	// The two things about an actor that follow from its model rather than from what it is
	// doing: which chunks it may join, and whether it may join any at all.
	void brender_inject::classify_actor(actor_record& record, const game::br_actor* actor,
		game::br_model* model) const
	{
		record.model = model;
		record.noncar = model->identifier && model->identifier[0] == '&';
		record.bakeable = !vanishes_outright(actor, model);
	}

	/*
	 * Follows one actor's placement and keeps the chunks in step with it.
	 *
	 * The only property that decides whether geometry may bake is whether the game is
	 * writing its placement, measured per scene: an actor that holds one for
	 * STATIC_PROMOTE_SIGHTINGS scenes bakes, and one whose transform changes is taken
	 * back out and starts that count again. Nothing is excluded by name or by category
	 * except what the game deletes outright, which no amount of holding still would make
	 * safe to bake.
	 */
	brender_inject::dynamic_reason brender_inject::track_static_actor(game::br_actor* actor,
		game::br_model* model, game::br_material* fallback_material,
		const game::br_matrix34& world)
	{
		const auto [it, fresh] = m_actors.try_emplace(actor);
		actor_record& record = it->second;

		if (fresh)
		{
			classify_actor(record, actor, model);
			record.last_seen_scene = m_scene_walks;

			if (!record.bakeable) {
				return dynamic_reason::vanishing;
			}

			const placement_id seed = placement_fingerprint(actor);
			record.placement = seed.where;
			record.parent_chain = seed.parent;
			record.sightings = 1;
			return dynamic_reason::probation;
		}

		/*
		 * An actor drawn more than once in one scene is a stencil the game re-places
		 * between draws -- the smoke quad, the spark emitter, the decal pools all render
		 * dozens of instances through a single actor. Its placement is whatever the last
		 * draw left behind, so counting draws as stillness let one frame satisfy the
		 * probation and bake a particle into the world.
		 */
		if (record.last_seen_scene == m_scene_walks)
		{
			if (unbake_actor(record)) { ++m_demotions.instanced; }
			record.bakeable = false;
			return dynamic_reason::instanced;
		}

		record.last_seen_scene = m_scene_walks;

		if (record.model != model)
		{
			note_placement_drift(model, "model swapped");
			if (unbake_actor(record)) { ++m_demotions.swapped; }
			classify_actor(record, actor, model);
		}

		// An actor the game deletes rather than moves can never enter a chunk, so there is
		// no placement to follow: skipping the chain walk keeps the pickups, the decal
		// pools and every model that failed extraction off the per-frame hashing path.
		if (!record.bakeable) {
			return record.bakes ? dynamic_reason::unbakeable : dynamic_reason::vanishing;
		}

		const placement_id placement = placement_fingerprint(actor);

		if (record.placement != placement.where)
		{
			note_placement_drift(model, "transform bytes changed");
			if (unbake_actor(record)) { ++m_demotions.moved; }
		}
		else if (record.parent_chain != placement.parent)
		{
			// Relinked without moving. The game regroups scenery as the city streams, and
			// where an actor hangs in the hierarchy says nothing about where it stands.
			++m_relinks_absorbed;
		}

		record.placement = placement.where;
		record.parent_chain = placement.parent;

		const uint32_t settle = record.demoted ? STATIC_REBAKE_SIGHTINGS
		                                      : STATIC_PROMOTE_SIGHTINGS;
		const bool moved = record.sightings == 0;

		if (!record.baked && ++record.sightings >= settle)
		{
			if (bake_actor(record, model, fallback_material, world))
			{
				record.baked = true;
				++record.bakes;
				m_have_unsealed = true;
				m_last_promotion_scene = m_scenes_submitted;
				if (record.demoted) { ++m_repromotions; }
			}
			else
			{
				// Failing to bake is a property of the model, not of this moment, so
				// retrying it every few scenes would only repeat the extraction.
				record.bakeable = false;
			}
		}

		if (record.live) {
			return dynamic_reason::chunked;
		}

		if (record.baked) {
			return dynamic_reason::unsealed;
		}

		return moved ? dynamic_reason::moving : dynamic_reason::probation;
	}

	/*
	 * Extracts an actor's model once, bakes its placement into the vertices and appends
	 * the result to the accumulating chunks.
	 *
	 * Everything the chunks need is copied here, so the game's memory is never read again
	 * for this actor -- models are freed between races, and a sealed chunk must not depend
	 * on them still being alive.
	 */
	bool brender_inject::bake_actor(actor_record& record, game::br_model* model,
		game::br_material* fallback_material, const game::br_matrix34& world)
	{
		const auto dev = shared::globals::d3d_device;
		if (!dev) {
			return false;
		}

		std::vector<ffp_vertex> vertices;
		std::vector<geometry_part> parts;
		std::vector<uint32_t> indices;

		if (!extract_geometry(dev, model, fallback_material, vertices, parts, indices))
		{
			note_placement_drift(model, "bake failed: no geometry");
			return false;
		}

		// A funk-animated material needs its UV transform re-read every frame, which only
		// the dynamic path does.
		for (const auto& part : parts)
		{
			if (m_animated_materials.contains(part.material))
			{
				note_placement_drift(model, "kept dynamic: animated material");
				return false;
			}
		}

		// The chunks draw with WORLD = identity, so the placement lives in the data.
		for (auto& v : vertices)
		{
			const float x = v.x, y = v.y, z = v.z;
			v.x = x * world.m[0][0] + y * world.m[1][0] + z * world.m[2][0] + world.m[3][0];
			v.y = x * world.m[0][1] + y * world.m[1][1] + z * world.m[2][1] + world.m[3][1];
			v.z = x * world.m[0][2] + y * world.m[1][2] + z * world.m[2][2] + world.m[3][2];

			const float nx = v.nx, ny = v.ny, nz = v.nz;
			v.nx = nx * world.m[0][0] + ny * world.m[1][0] + nz * world.m[2][0];
			v.ny = nx * world.m[0][1] + ny * world.m[1][1] + nz * world.m[2][1];
			v.nz = nx * world.m[0][2] + ny * world.m[1][2] + nz * world.m[2][2];
		}

		record.ranges.clear();
		record.materials.clear();
		for (const auto& part : parts)
		{
			append_part_to_chunk(part, vertices, indices, record);
			record.materials.push_back(part.material);
		}

		return !record.ranges.empty();
	}

	// Copies one material run into the open chunk for its (texture, blend) pair, remapping
	// the vertices it references into the chunk and recording where the indices landed so
	// the actor can be punched back out if it ever moves.
	void brender_inject::append_part_to_chunk(const geometry_part& part,
		const std::vector<ffp_vertex>& vertices, const std::vector<uint32_t>& indices,
		actor_record& record)
	{
		const uint32_t index_end = part.index_start + part.triangle_count * 3u;

		std::vector<int32_t> remap(vertices.size(), -1);
		uint32_t unique = 0;
		for (uint32_t i = part.index_start; i < index_end; ++i)
		{
			if (remap[indices[i]] < 0) {
				remap[indices[i]] = static_cast<int32_t>(unique++);
			}
		}

		if (unique == 0 || unique > CHUNK_VERTEX_LIMIT) {
			return;
		}

		const uint64_t key = chunk_key(part.texture, part.has_alpha, record.noncar, part.opacity);
		size_t chunk_index = SIZE_MAX;
		if (const auto it = m_open_chunks.find(key); it != m_open_chunks.end())
		{
			const static_chunk& open = m_chunks[it->second];
			if (!open.sealed && open.vertices.size() + unique <= CHUNK_VERTEX_LIMIT) {
				chunk_index = it->second;
			}
		}

		if (chunk_index == SIZE_MAX)
		{
			static_chunk fresh{};
			fresh.texture = part.texture;
			fresh.has_alpha = part.has_alpha;
			fresh.opacity = part.opacity;
			m_chunks.push_back(std::move(fresh));
			chunk_index = m_chunks.size() - 1;
			m_open_chunks[key] = chunk_index;
		}

		static_chunk& chunk = m_chunks[chunk_index];
		const auto base = static_cast<uint32_t>(chunk.vertices.size());

		chunk.vertices.resize(base + unique);
		for (uint32_t i = part.index_start; i < index_end; ++i) {
			chunk.vertices[base + remap[indices[i]]] = vertices[indices[i]];
		}

		baked_range range{};
		range.chunk = static_cast<uint32_t>(chunk_index);
		range.index_start = static_cast<uint32_t>(chunk.indices.size());
		range.index_count = index_end - part.index_start;

		for (uint32_t i = part.index_start; i < index_end; ++i) {
			chunk.indices.push_back(static_cast<uint16_t>(base + remap[indices[i]]));
		}

		record.ranges.push_back(range);
	}

	/*
	 * Overwrites an actor's baked indices with degenerate triangles.
	 *
	 * A triangle that names vertex zero three times has no area, so neither the rasterizer
	 * nor a ray can hit it. For a sealed chunk this is the one narrow write the
	 * immutability rule allows: the alternative is rebuilding the chunk, which is exactly
	 * the hitch this design exists to avoid.
	 */
	void brender_inject::punch_out(const actor_record& record)
	{
		for (const baked_range& range : record.ranges)
		{
			static_chunk& chunk = m_chunks[range.chunk];

			if (!chunk.sealed)
			{
				std::fill_n(chunk.indices.begin() + range.index_start, range.index_count,
					static_cast<uint16_t>(0));
				continue;
			}

			if (!chunk.index_buffer) {
				continue;
			}

			void* mapped = nullptr;
			if (SUCCEEDED(chunk.index_buffer->Lock(range.index_start * sizeof(uint16_t),
				range.index_count * sizeof(uint16_t), &mapped, 0)))
			{
				std::memset(mapped, 0, range.index_count * sizeof(uint16_t));
				chunk.index_buffer->Unlock();
			}
		}
	}

	/*
	 * Takes an actor's geometry back out of the chunks and restarts its probation.
	 *
	 * The record stays, so an actor that stops again bakes again: in Carmageddon 2 wrecking
	 * the scenery is the game, and a lamppost that is knocked flat still spends the rest of
	 * the race lying perfectly still. Evicting it for good made the static world shrink for
	 * the whole race, and every actor it lost cost both a proxy draw and the game render
	 * that suppression only covers for chunk-backed geometry.
	 *
	 * Returns whether there was a bake to take back, so callers can attribute the loss.
	 */
	bool brender_inject::unbake_actor(actor_record& record)
	{
		punch_out(record);

		if (record.live && m_live_actors) {
			--m_live_actors;
		}

		const bool was_baked = record.baked;
		record.baked = false;
		record.live = false;
		record.demoted = record.demoted || was_baked;
		record.sightings = 0;
		record.ranges.clear();
		record.materials.clear();

		// The dead vertices a rebake leaves behind are never reclaimed, so an actor that
		// has used up its bakes has proved it is not scenery and stops trying.
		if (record.bakes >= STATIC_MAX_BAKES) {
			record.bakeable = false;
		}

		return was_baked;
	}

	void brender_inject::release_chunks()
	{
		for (auto& chunk : m_chunks)
		{
			if (chunk.vertex_buffer) { chunk.vertex_buffer->Release(); }
			if (chunk.index_buffer) { chunk.index_buffer->Release(); }
		}
		m_chunks.clear();
		m_open_chunks.clear();
	}

	void brender_inject::reset_static_world(const char* reason)
	{
		if (m_chunks.empty() && m_actors.empty()) {
			return;
		}

		release_chunks();
		m_actors.clear();
		m_animated_materials.clear();
		m_material_state.clear();
		m_have_unsealed = false;
		m_live_actors = 0;

		// The tallies describe how one race's static world held up; carrying them into the
		// next one would read as a world that started the race already decayed.
		m_demotions = {};
		m_relinks_absorbed = 0;
		m_repromotions = 0;

		shared::common::log("BRender", std::format("static world reset - {}", reason),
			shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
	}

	/*
	 * Leaving the race. Whether that means a new track or only the pause menu is not
	 * knowable yet, so nothing is dropped here -- the counting starts instead.
	 */
	void brender_inject::on_frontend_entered()
	{
		m_in_frontend = true;
		m_frontend_models.clear();

		// Whatever camera the race was measured against is finished with. Keeping it would
		// let a recycled camera pointer in the next track pass for the race view, and the
		// first submit back establishes the real one anyway.
		m_race_camera = nullptr;
	}

	/*
	 * Decides, at the first race scene back, whether a track was loaded while we were away.
	 *
	 * Loading a level runs BrModelUpdate over every model it reads, in the hundreds. The
	 * frontend rebuilds a handful for its rotating car previews, and resuming from the
	 * pause menu rebuilds none at all: the track is still loaded and every pointer in the
	 * module still describes what it did before the menu opened. Dropping the static world
	 * on a pause is a rebuild the game never asked for.
	 *
	 * Only the static world is track-scoped. The geometry and texture caches carry the
	 * identity of what they were built from and re-check it on every lookup, so a recycled
	 * pointer cannot return the wrong object whatever this decides; releasing them here is
	 * about not holding the previous track's uploads for the rest of the session.
	 */
	void brender_inject::resolve_frontend_return()
	{
		m_in_frontend = false;

		const size_t distinct = m_frontend_models.size();
		m_frontend_models.clear();

		if (distinct < TRACK_LOAD_DISTINCT_MODELS)
		{
			shared::common::log("BRender", std::format(
				"back in the race after {} distinct model rebuilds - same track, nothing dropped",
				distinct), shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, true);
			return;
		}

		reset_static_world("a new track was loaded");

		// The queue and the transient pool hold pointers into m_geometry, so they go first.
		m_queue.clear();
		m_lines.clear();
		release_transient();

		for (auto& [model, geometry] : m_geometry) {
			release_geometry(geometry);
		}
		m_geometry.clear();

		for (auto& [pixelmap, cached] : m_textures)
		{
			if (cached.texture) { cached.texture->Release(); }
		}
		m_textures.clear();

		// m_materials keeps what it has: the loader taught it this track's stored tokens on
		// the way in, and nothing re-teaches them once the race is running. The flat and
		// spark swatches are keyed on colour rather than a game pointer, so they stay valid.

		m_textures_ok = 0;
		m_textures_failed = 0;
		m_flat_probes = 0;
		m_logged_spark_geometry = false;
		m_unsupported_types.clear();
		m_unsupported_styles.clear();
		m_skipped_models.clear();
		m_shaded_materials.clear();

		// The world pointer is re-established by the check that follows this call, and the
		// frame clock would otherwise report the whole load as one scene.
		m_submitted_world = nullptr;
		m_last_scene_ticks = 0;
		m_window_worst = {};

		shared::common::log("BRender", std::format(
			"track state flushed after {} distinct model rebuilds - geometry and textures re-upload",
			distinct), shared::common::LOG_TYPE::LOG_TYPE_GREEN, true);
	}

	/*
	 * Marks a material animated once its appearance is updated in two different scenes.
	 *
	 * One update proves nothing: level loading calls BrMaterialUpdate with BR_MATU_ALL for
	 * every material it touches, all while the scene counter stands still -- an entire
	 * load burst carries one timestamp. Animation is what spans scenes: scrolling water
	 * updates its UV transform every frame, a flashing sign every state change, and a
	 * fading sprite its opacity. A sealed chunk bakes all three, so anything that moves
	 * mid-race has to stay on the dynamic path where it is re-read every capture.
	 *
	 * The flag alone is not the signal for opacity. The game re-publishes a material for
	 * reasons the injection does not render -- BR_MATU_MATERIAL rides along with lighting
	 * and index-range updates -- and taking those at face value flagged ROAD, 0RDSDTOP and
	 * the terrain as animated, which is most of a level's surface area. Only a value that
	 * actually moved counts.
	 */
	void brender_inject::on_material_update(game::br_material* material, const uint16_t flags)
	{
		const auto& effects = shared::common::config::get().effects;
		const auto [state, first] = m_material_state.try_emplace(material);

		// The baseline is read from the material on first sight, whatever this update
		// carries. Leaving it to the first opacity-flagged update means an entry created
		// by a UV-only update holds a default no material ever has, and the next ordinary
		// update reads as a fade -- which marked every road it touched as animated.
		if (first) {
			state->second.opacity = material_opacity(material);
		}

		const bool uv_moved = (flags & game::BR_MATU_MAP_TRANSFORM) != 0;
		bool opacity_moved = false;
		uint8_t opacity_was = state->second.opacity;
		uint8_t opacity_now = opacity_was;

		if ((flags & (game::BR_MATU_MATERIAL | game::BR_MATU_EXTRA)) && effects.material_opacity)
		{
			opacity_now = material_opacity(material);
			opacity_moved = !first && opacity_now != opacity_was;
			state->second.opacity = opacity_now;
		}

		if (!uv_moved && !opacity_moved) {
			return;
		}

		if (first || state->second.scene == m_scenes_submitted)
		{
			state->second.scene = m_scenes_submitted;
			return;
		}
		state->second.scene = m_scenes_submitted;

		if (!m_animated_materials.insert(material).second) {
			return;
		}

		// Anything already baked with it is showing a frozen frame of the animation.
		uint32_t evicted = 0;
		for (auto& [actor, record] : m_actors)
		{
			if (record.baked
				&& std::find(record.materials.begin(), record.materials.end(), material)
					!= record.materials.end())
			{
				if (unbake_actor(record)) { ++evicted; }
			}
		}

		if (!evicted) {
			return;
		}

		m_demotions.animated += evicted;

		// One animated material can take a large share of the static world with it, and the
		// trigger is named because the two have different failure modes: a UV transform is
		// the funk system and almost certainly genuine, while an opacity move on something
		// like a road would mean the byte is being misread and the eviction is the bug.
		shared::common::log("BRender", std::format(
			"material '{}' animates ({}) - {} baked actors returned to the dynamic path",
			material->identifier && readable(material->identifier, 1)
				? material->identifier : "<null>",
			uv_moved && opacity_moved
				? std::format("UV transform, opacity {} -> {}", opacity_was, opacity_now)
			: uv_moved ? "UV transform"
			: std::format("opacity {} -> {}", opacity_was, opacity_now),
			evicted),
			shared::common::LOG_TYPE::LOG_TYPE_WARN, false);
	}

	/*
	 * Uploads every accumulating chunk into buffers that will never change again.
	 *
	 * Sealing waits for promotions to go quiet so the whole discovery burst -- with
	 * frustum culling disabled that is essentially the entire level, a few scenes in --
	 * lands in one set of buffers. Remix hashes each buffer once and keeps the
	 * acceleration structure it builds, so a sealed chunk costs nothing per frame.
	 */
	void brender_inject::seal_chunks(IDirect3DDevice9* dev)
	{
		uint32_t sealed = 0, vertices = 0, triangles = 0;
		bool all_ok = true;

		for (auto& chunk : m_chunks)
		{
			if (chunk.sealed) {
				continue;
			}

			if (chunk.vertices.empty() || chunk.indices.empty())
			{
				chunk.sealed = true;
				continue;
			}

			const UINT vertex_bytes = static_cast<UINT>(chunk.vertices.size() * sizeof(ffp_vertex));
			const UINT index_bytes = static_cast<UINT>(chunk.indices.size() * sizeof(uint16_t));

			if (FAILED(dev->CreateVertexBuffer(vertex_bytes, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED,
				&chunk.vertex_buffer, nullptr))
				|| FAILED(dev->CreateIndexBuffer(index_bytes, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16,
					D3DPOOL_MANAGED, &chunk.index_buffer, nullptr)))
			{
				if (chunk.vertex_buffer) { chunk.vertex_buffer->Release(); chunk.vertex_buffer = nullptr; }
				all_ok = false;
				continue;
			}

			void* mapped = nullptr;
			if (SUCCEEDED(chunk.vertex_buffer->Lock(0, vertex_bytes, &mapped, 0)))
			{
				memcpy(mapped, chunk.vertices.data(), vertex_bytes);
				chunk.vertex_buffer->Unlock();
			}
			if (SUCCEEDED(chunk.index_buffer->Lock(0, index_bytes, &mapped, 0)))
			{
				memcpy(mapped, chunk.indices.data(), index_bytes);
				chunk.index_buffer->Unlock();
			}

			chunk.vertex_count = static_cast<uint32_t>(chunk.vertices.size());
			chunk.triangle_count = static_cast<uint32_t>(chunk.indices.size()) / 3u;
			chunk.sealed = true;
			chunk.vertices = {};
			chunk.indices = {};

			++sealed;
			vertices += chunk.vertex_count;
			triangles += chunk.triangle_count;
		}

		// Actors go live only once every chunk they live in is sealed.
		for (auto& [actor, record] : m_actors)
		{
			if (!record.baked || record.live) {
				continue;
			}

			const bool ready = std::all_of(record.ranges.begin(), record.ranges.end(),
				[&](const baked_range& range) { return m_chunks[range.chunk].sealed; });

			if (ready)
			{
				record.live = true;
				++m_live_actors;
			}
		}

		if (all_ok) {
			m_have_unsealed = false;
		}
		else
		{
			// Retry after another quiet window instead of every scene.
			m_last_promotion_scene = m_scenes_submitted;
			shared::common::log("BRender", "chunk buffer creation failed - will retry",
				shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
		}

		// Later promotions open fresh chunks; the sealed ones are closed for good.
		m_open_chunks.clear();

		shared::common::log("BRender", std::format(
			"static world sealed: {} chunks (+{} this pass), {} verts, {} tris, {} live actors",
			m_chunks.size(), sealed, vertices, triangles, m_live_actors),
			shared::common::LOG_TYPE::LOG_TYPE_GREEN, true);
	}

	void brender_inject::begin_scene(game::br_actor* world, game::br_actor* camera)
	{
		if (const auto& culling = shared::common::config::get().culling;
			culling.disable_frustum || culling.bubble_radius > 0.0f)
		{
			install_bounds_test_hook();
		}

		m_world = world;
		m_camera = camera;
		m_camera_valid = false;
		m_capturing = !m_overlay_scene;
		++m_scene_walks;
		m_scene_models = 0;
		m_dynamic_reasons = {};
		m_profile = {};
		forget_readable_regions();
		m_queue.clear();
		m_lines.clear();

		// The queue held pointers into these until the previous scene submitted.
		m_transient_used = 0;
	}

	void brender_inject::capture_camera()
	{
		if (!m_capturing) {
			return;
		}

		// Remix needs WORLD and VIEW apart, but BRender only ever tracks the concatenation.
		// Snapshotting world_to_view here lets model_to_world fall out by inverting it.
		m_camera_valid = query_model_to_view(m_world_to_view)
			&& invert34(m_world_to_view, m_view_inverse);
	}

	bool brender_inject::capture_model(game::br_actor* actor, game::br_model* model,
		game::br_material* fallback_material, const uint32_t style)
	{
		if (!m_capturing || !m_camera_valid) {
			return false;
		}

		// Only the race scene is ever submitted to Remix, so only its models may be
		// suppressed or tracked as scenery. The 3D HUD widgets run their models through
		// the same incremental API under their own cameras; the game's own render is all
		// those have.
		const bool race_scene = m_race_camera && m_camera == m_race_camera;

		const uint32_t effective_style = style & 0xFFu;
		const auto prepared = model->prepared;
		if (!prepared || !prepared->groups || !prepared->ngroups)
		{
			// Never becomes a draw at all. Anything visible in-game but missing from our
			// batch is coming from nGlide's rasterized output instead, so these names are
			// worth having when something looks untextured rather than absent.
			const std::string name = model->identifier ? model->identifier : "<null>";
			const char* reason = (model->flags & 0x20) ? "custom render callback" : "no prepared geometry";
			if (m_skipped_models.try_emplace(name, reason).second) {
				shared::common::log("BRender", std::format("skipped: '{}' - {}", name, reason),
					shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
			}
			return false;
		}

		game::br_matrix34 model_to_view{};
		if (!query_model_to_view(model_to_view))
		{
			note_not_injected(model, "renderer would not give a model_to_view");
			return false;
		}

		game::br_matrix34 model_to_world{};
		mul34(model_to_view, m_view_inverse, model_to_world);

		// Sparks and other streaks. Their faces are not triangles, so they never go through
		// the vertex buffer path -- and they are rebuilt several times per frame, which that
		// path's per-model caching cannot represent anyway.
		if (effective_style == game::BR_RSTYLE_EDGES)
		{
			if (!race_scene || !shared::common::config::get().effects.sparks) {
				return false;
			}

			capture_lines(model, model_to_world);
			return true;
		}

		// DEFAULT resolves to FACES. POINTS and the bounding-volume styles draw something
		// other than the model's faces, and NONE draws nothing at all; nothing in the shipped
		// game reaches here with any of them, so the log is there to say so if that changes.
		if (effective_style != game::BR_RSTYLE_DEFAULT && effective_style != game::BR_RSTYLE_FACES)
		{
			note_unsupported_style(model, effective_style);
			return false;
		}

		++m_scene_models;

		/*
		 * Scenery holds one placement for as long as nothing hits it, so it can be baked
		 * into the chunks whatever its transform. Only things that are actually moving --
		 * cars, wheels, spinning powerups -- need a draw of their own. An actor has to hold
		 * still for several frames first: baking on sight would bake every car at its
		 * starting position and leave a ghost there the moment it drove off.
		 */
		dynamic_reason reason = dynamic_reason::overlay;
		if (race_scene && shared::common::config::get().optimization.static_world && actor)
		{
			reason = model->flags & 0x20
				? dynamic_reason::callback
				: track_static_actor(actor, model, fallback_material, model_to_world);

			if (reason == dynamic_reason::chunked) {
				return true;
			}
		}

		++m_dynamic_reasons[static_cast<size_t>(reason)];

		queued_model queued{};
		queued.geometry = nullptr;
		queued.world = to_d3d(model_to_world);
		queued.model_name = model->identifier ? model->identifier : "<null>";

		// The device is needed to build the buffers, and it exists by the time any scene
		// runs; storing the model here and resolving in submit would need a second lookup.
		const auto dev = shared::globals::d3d_device;
		if (!dev) {
			return false;
		}

		// Anything that reaches here and fails is geometry the game will still draw and Remix
		// will only ever rasterize. Naming it is the difference between knowing the injection's
		// coverage gap and guessing at it from what looks wrong on screen.
		const auto geometry = geometry_for(dev, model, fallback_material);
		if (!geometry)
		{
			note_not_injected(model, "geometry could not be built");
			return false;
		}

		refresh_part_state(*geometry);
		geometry->queued_scene = m_scene_walks;
		queued.geometry = geometry;
		m_queue.push_back(queued);

		/*
		 * A sealed chunk is proof that the injection carries this geometry, so its game
		 * render always goes. A dynamic model has no such proof: not everything a model
		 * draws passes through this hook -- pedestrian limbs are drawn inside the ped's own
		 * render call, and Remix composites that rasterized stream -- so dropping it can
		 * take geometry off the screen that nothing else replaces. In a race dense enough
		 * to drop frames the dynamics are most of what BRender still transforms on the CPU,
		 * which is why the trade is offered rather than decided here.
		 */
		return shared::common::config::get().optimization.suppress_dynamics;
	}

	/*
	 * Turns an EDGES-style model into world-space line segments.
	 *
	 * BRender's edge renderer walks each face's three edges, which lets the game encode a
	 * single line as one face with two coincident indices -- Carmageddon 2 builds its sparks
	 * that way, rewriting one shared two-vertex model for every streak it draws. As a
	 * triangle that face has zero area and draws nothing, which is why sparks went missing.
	 *
	 * Colour lives in the authored vertices rather than the material: gLine_material is plain
	 * white and BR_MATF_PRELIT tells BRender to take the vertex colours as final. The two
	 * ends differ -- 0x004F7CB0 writes ff0000 into one and ffff00 into the other, which is
	 * where a spark's orange comes from -- so both are read.
	 */
	void brender_inject::capture_lines(const game::br_model* model, const game::br_matrix34& model_to_world)
	{
		const auto prepared = model->prepared;
		const game::br_vertex* authored = model->vertices;
		if (!readable(authored, sizeof(game::br_vertex) * model->nvertices)) {
			authored = nullptr;
		}

		const auto to_world = [&](const game::v1_online_vertex& src, float out[3])
		{
			const float x = src.px + model->pivot.v[0];
			const float y = src.py + model->pivot.v[1];
			const float z = src.pz + model->pivot.v[2];

			for (int col = 0; col < 3; ++col)
			{
				out[col] = x * model_to_world.m[0][col]
				         + y * model_to_world.m[1][col]
				         + z * model_to_world.m[2][col]
				         + model_to_world.m[3][col];
			}
		};

		for (uint16_t g = 0; g < prepared->ngroups; ++g)
		{
			const game::v1_group& group = prepared->groups[g];
			if (!group.vertices || !group.faces || !group.nvertices || !group.nfaces) {
				continue;
			}

			const auto colour_of = [&](const uint16_t v) -> uint32_t
			{
				if (!authored || !group.vertex_src_index) {
					return 0xFFFFFFu;
				}

				const uint16_t source = group.vertex_src_index[v];
				if (source >= model->nvertices) {
					return 0xFFFFFFu;
				}

				const game::br_vertex& vertex = authored[source];
				return (static_cast<uint32_t>(vertex.red) << 16)
				     | (static_cast<uint32_t>(vertex.green) << 8)
				     | vertex.blue;
			};

			for (uint16_t f = 0; f < group.nfaces; ++f)
			{
				const game::v1_online_face& face = group.faces[f];
				uint32_t emitted[3]{};
				int emitted_count = 0;

				for (int edge = 0; edge < 3; ++edge)
				{
					const uint16_t from = face.v[edge];
					const uint16_t to = face.v[(edge + 1) % 3];
					if (from == to || from >= group.nvertices || to >= group.nvertices) {
						continue;
					}

					// A face encoding a single line repeats one index, so the same pair
					// comes round twice -- once each way.
					const uint32_t key = from < to
						? (static_cast<uint32_t>(from) << 16) | to
						: (static_cast<uint32_t>(to) << 16) | from;

					if (std::find(emitted, emitted + emitted_count, key) != emitted + emitted_count) {
						continue;
					}
					emitted[emitted_count++] = key;

					line_segment segment{};
					to_world(group.vertices[from], segment.a);
					to_world(group.vertices[to], segment.b);
					segment.rgb_a = colour_of(from);
					segment.rgb_b = colour_of(to);
					m_lines.push_back(segment);
				}
			}
		}
	}

	bool brender_inject::build_projection(D3DMATRIX& out) const
	{
		if (!m_camera || !m_camera->type_data) {
			return false;
		}

		const auto cam = static_cast<const game::br_camera*>(m_camera->type_data);
		if (cam->yon_z <= cam->hither_z || cam->field_of_view == 0) {
			return false;
		}

		// BRender is right-handed with the camera looking down -Z, which is why
		// BrCameraToScreenMatrix4 negates hither and yon before building its matrix.
		const float fov_y = static_cast<float>(cam->field_of_view) * BR_ANGLE_TO_RADIANS;
		const float aspect = (cam->aspect > 0.0f) ? cam->aspect : 4.0f / 3.0f;

		D3DXMatrixPerspectiveFovRH(reinterpret_cast<D3DXMATRIX*>(&out), fov_y, aspect, cam->hither_z, cam->yon_z);
		return true;
	}

	void brender_inject::end_scene()
	{
		const bool was_capturing = m_capturing;
		m_capturing = false;

		if (!was_capturing) {
			return;
		}

		// The game opens several Begin/End scenes per frame against the same 640x480 buffer:
		// the race view plus one-model 3D HUD widgets such as the opponent-car icon.
		// Forwarding a widget hands Remix a second camera with a ~35 unit far plane, and it
		// path-traces the icon instead of the track.
		// Counts every model the scene walked, not just the queued dynamic ones -- level
		// geometry goes to the static batches and would otherwise make a race look like a
		// one-model widget scene.
		if (m_scene_models < MIN_WORLD_SCENE_MODELS) {
			return;
		}

		if (const auto dev = shared::globals::d3d_device; dev) {
			submit(dev);
		}
	}

	void brender_inject::submit(IDirect3DDevice9* dev)
	{
		D3DMATRIX projection{};
		if (!build_projection(projection)) {
			return;
		}

		// This scene proved itself the race view, so its camera is the one models are
		// measured against from now on.
		m_race_camera = m_camera;

		if (m_in_frontend) {
			resolve_frontend_return();
		}

		// A world actor swap is not something the game is known to do -- one world actor is
		// reused across races -- but it would mean the same thing as a track load.
		if (m_world != m_submitted_world)
		{
			if (m_submitted_world) {
				reset_static_world("the world actor changed");
			}
			m_submitted_world = m_world;
		}

		LARGE_INTEGER start{};
		QueryPerformanceCounter(&start);

		// Everything the device has drawn this frame before we add anything. nGlide's
		// screen-space output is not discarded by Remix -- a decl carrying POSITIONT comes
		// back as RtxGeometryStatus::Rasterized, which preserves the draw and rasterizes it --
		// so both streams cross the bridge and both end up on screen.
		m_profile.glide_draws = shared::common::ffp_state::get().draw_call_count();

		const D3DMATRIX view = to_d3d(m_world_to_view);

		// nGlide owns the device for the rest of the frame and sets its state lazily, so
		// everything this replay touches is captured and put back afterwards. The block is
		// built once and re-captured: creating and destroying a D3DSBT_ALL block every scene
		// meant allocating and freeing the whole device state across the bridge every frame.
		if (!m_saved_state && FAILED(dev->CreateStateBlock(D3DSBT_ALL, &m_saved_state))) {
			return;
		}

		if (FAILED(m_saved_state->Capture())) {
			return;
		}

		dev->SetVertexShader(nullptr);
		dev->SetPixelShader(nullptr);

		if (!m_vertex_decl) {
			dev->CreateVertexDeclaration(INJECT_DECL, &m_vertex_decl);
		}
		dev->SetVertexDeclaration(m_vertex_decl);

		dev->SetTransform(D3DTS_VIEW, &view);
		dev->SetTransform(D3DTS_PROJECTION, &projection);

		dev->SetRenderState(D3DRS_LIGHTING, FALSE);
		dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
		dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
		dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
		dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);

		// Discards fully transparent texels outright rather than blending them in, which
		// keeps a decal's cut-out area from contributing anything at all.
		dev->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATER);
		dev->SetRenderState(D3DRS_ALPHAREF, 0);

		if (shared::common::config::get().effects.fog) {
			apply_fog(dev);
		}

		ensure_white_texture(dev);

		// The two things BRender modulates a surface by, in the two places D3D9 can carry
		// them: the prelit vertex colour is per vertex and rides in the buffer, while
		// opacity is one number per material that the game rewrites between draws, so it
		// rides in the texture factor. Both are identity (white, opaque) for ordinary lit
		// geometry, which leaves it exactly as the plain texture select did.
		dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
		dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
		dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
		dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
		dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
		dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_TFACTOR);
		dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);

		// Remix reads the texcoord set from stage 0's TEXCOORDINDEX and then only accepts a
		// declaration element whose UsageIndex matches (d3d9_rtx.cpp:1126 and :252). Leaving
		// whatever nGlide last set here makes it reject TEXCOORD0 and report the mesh as
		// having no UVs. A stale texture transform would corrupt the coordinates just as badly.
		dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
		dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);

		// Carmageddon 2 addresses well outside 0..1 - track pieces reach u = 7.0.
		dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
		dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
		dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
		dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);

		// Stale bindings from the Glide path would otherwise be captured as this geometry's
		// material by Remix.
		for (DWORD stage = 1; stage < 8; ++stage) {
			dev->SetTexture(stage, nullptr);
		}

		if (m_have_unsealed
			&& m_scenes_submitted - m_last_promotion_scene >= STATIC_SEAL_QUIET_SCENES)
		{
			seal_chunks(dev);
		}

		uint32_t vertices = 0;
		uint32_t sealed_chunks = 0;
		for (const auto& chunk : m_chunks)
		{
			if (chunk.sealed && chunk.vertex_buffer)
			{
				vertices += chunk.vertex_count;
				++sealed_chunks;
			}
		}
		for (const auto& queued : m_queue) {
			vertices += queued.geometry->vertex_count;
		}

		const uint32_t draws = (shared::common::config::get().effects.translucent_pass
				? draw_pass(dev, pass_kind::opaque) + draw_pass(dev, pass_kind::blended)
				: draw_pass(dev, pass_kind::combined))
			+ submit_lines(dev);

		m_saved_state->Apply();

		// Restoring the block reverts VIEW and PROJECTION to whatever nGlide last had, which
		// is nothing -- it draws exclusively with pre-transformed vertices and never touches
		// the fixed-function transforms. Leaving our camera installed costs nGlide nothing
		// and keeps it visible to Remix for the rest of the frame.
		dev->SetTransform(D3DTS_VIEW, &view);
		dev->SetTransform(D3DTS_PROJECTION, &projection);

		LARGE_INTEGER end{};
		QueryPerformanceCounter(&end);

		frame_stats stats{};
		stats.draws = draws;
		stats.vertices = vertices;
		stats.models = static_cast<uint32_t>(m_queue.size()) + m_live_actors;
		stats.baked = m_live_actors;
		stats.chunks = sealed_chunks;
		stats.demotions = m_demotions;
		stats.relinks_absorbed = m_relinks_absorbed;
		stats.repromotions = m_repromotions;
		stats.scene = m_scenes_submitted;
		stats.dynamic_reasons = m_dynamic_reasons;
		stats.segments = static_cast<uint32_t>(m_lines.size());
		stats.model_updates = m_profile.model_updates;
		stats.rebuilds = m_profile.rebuilds;
		stats.glide_draws = m_profile.glide_draws;
		stats.capture_ms = static_cast<double>(m_profile.capture_ticks) / m_ticks_per_ms;
		stats.bounds_ms = static_cast<double>(m_profile.bounds_ticks) / m_ticks_per_ms;
		stats.game_render_ms = static_cast<double>(m_profile.game_render_ticks) / m_ticks_per_ms;
		stats.scene_end_ms = static_cast<double>(m_profile.scene_end_ticks) / m_ticks_per_ms;
		stats.submit_ms = static_cast<double>(end.QuadPart - start.QuadPart) / m_ticks_per_ms;

		// Present, the overlay passes and the frame's full draw count all belong to the stretch
		// between the previous submit and this one, so they describe the frame just gone. Over
		// a steady stretch that is the same thing as this frame.
		stats.present_ms = shared::common::ffp_state::get().last_present_ms();
		stats.frame_draws = shared::common::ffp_state::get().last_frame_draw_count();
		stats.overlay_ms = static_cast<double>(m_overlay_ticks) / m_ticks_per_ms;
		m_overlay_ticks = 0;
		stats.frame_ms = m_last_scene_ticks
			? static_cast<double>(start.QuadPart - m_last_scene_ticks) / m_ticks_per_ms
			: 0.0;
		m_last_scene_ticks = start.QuadPart;

		log_performance(stats);

		++m_scenes_submitted;
		if ((m_scenes_submitted % 600) == 0) {
			evict_stale_geometry();
		}
	}

	/*
	 * The race's depth cue, restated as D3D9 fixed-function fog.
	 *
	 * The game has no scene fog of its own -- the race TXT's depth-cue block is baked into
	 * each material's fog fields and pushed to the Glide driver as a fog table, none of
	 * which survives nGlide's shader path onto the D3D9 device. Remix's legacy fog
	 * remapping reads D3DRS_FOGENABLE / FOGCOLOR / FOGSTART / FOGEND off the first fogged
	 * draw of a frame to derive its volumetric transmittance colour and distance, so the
	 * injected draws are where the depth cue re-enters the pipeline. The state block
	 * restore at the end of submit keeps it off nGlide's stream.
	 *
	 * The depth cue is always distance-linear (BrMaterialUpdate emits BRT_FOG_T = LINEAR,
	 * 0x005210FE), so linear vertex fog with the game's own min/max is the whole mapping.
	 * The sky needs no exclusion here: the game fogs HORIZON.MAT through a shade table
	 * rather than a fog flag, and the horizon never passes through this injection.
	 */
	void brender_inject::apply_fog(IDirect3DDevice9* dev)
	{
		const game::scene_fog fog = game::read_scene_fog();

		if (fog.enabled != m_logged_fog.enabled
			|| fog.colour != m_logged_fog.colour
			|| fog.min_distance != m_logged_fog.min_distance
			|| fog.max_distance != m_logged_fog.max_distance)
		{
			// The camera's world position rides along because Remix's volumetric
			// atmosphere is a sphere anchored to world y = 0: whether this world's
			// vertical range fits inside its default 30 m shell is exactly what
			// decides if "Atmosphere Enabled" works or blacks the level out.
			shared::common::log("BRender", fog.enabled
				? std::format("depth cue: fog colour {:06X}, {:.2f} to {:.2f} world units,"
					" camera world position ({:.1f}, {:.1f}, {:.1f})",
					fog.colour, fog.min_distance, fog.max_distance,
					m_view_inverse.m[3][0], m_view_inverse.m[3][1], m_view_inverse.m[3][2])
				: std::string("depth cue: none"));
			m_remix_fog_synced = false;
			m_logged_fog = fog;
		}

		if (!m_remix_fog_synced) {
			m_remix_fog_synced = push_fog_to_remix(fog);
		}

		dev->SetRenderState(D3DRS_FOGENABLE, fog.enabled ? TRUE : FALSE);
		if (!fog.enabled) {
			return;
		}

		const auto as_dword = [](const float value)
		{
			DWORD out;
			std::memcpy(&out, &value, sizeof(out));
			return out;
		};

		dev->SetRenderState(D3DRS_FOGTABLEMODE, D3DFOG_NONE);
		dev->SetRenderState(D3DRS_FOGVERTEXMODE, D3DFOG_LINEAR);
		dev->SetRenderState(D3DRS_FOGCOLOR, fog.colour);
		dev->SetRenderState(D3DRS_FOGSTART, as_dword(fog.min_distance));
		dev->SetRenderState(D3DRS_FOGEND, as_dword(fog.max_distance));
		dev->SetRenderState(D3DRS_FOGDENSITY, as_dword(1.0f));
	}

	/*
	 * Retargets Remix's volumetric medium at the track's fog colour.
	 *
	 * The authored depth-cue colour is a display fade target -- at fog_max the vanilla
	 * framebuffer pixel simply *becomes* that colour -- but Remix reads the same value as
	 * a physical medium. Two things break under that reinterpretation. Sky light, this
	 * port's only illumination, survives transmittance^5 no matter what the distances
	 * are set to, so a dark cue (the "dark" mode, the red tracks) extinguishes the level
	 * outright. And a genuinely red medium scatters the complementary hue -- cyan glow,
	 * reddened background -- when the game wants the glow itself red.
	 *
	 * So the colour is decomposed into the terms Remix actually has for it: the
	 * saturation goes to the single-scattering albedo, which is the colour of the fog's
	 * own glow and cannot darken anything; a whisper of hue (FogTint) goes to the
	 * transmittance, bounded so the weakest channel keeps most of the sky; and the raw
	 * colour keeps riding the FOGCOLOR render state, where the multiscattering term
	 * picks it up. Pushed once per depth-cue change, not per frame -- these are global
	 * Remix options crossing the bridge.
	 */
	bool brender_inject::push_fog_to_remix(const game::scene_fog& fog) const
	{
		if (!shared::common::config::get().effects.fog_volumetrics) {
			return true;
		}
		if (!shared::common::remix_api::is_initialized()) {
			return false;
		}

		const auto& bridge = shared::common::remix_api::get().m_bridge;
		if (!bridge.SetConfigVariable) {
			return true;
		}

		// The neutral presets from rtx.conf, restored whenever the depth cue is off.
		float albedo[3] = { 0.95f, 0.95f, 0.95f };
		float transmittance[3] = { 0.93f, 0.93f, 0.93f };

		if (fog.enabled)
		{
			const float channels[3] = {
				static_cast<float>((fog.colour >> 16) & 0xFF) / 255.0f,
				static_cast<float>((fog.colour >> 8) & 0xFF) / 255.0f,
				static_cast<float>(fog.colour & 0xFF) / 255.0f,
			};
			const float max_channel = std::max({ channels[0], channels[1], channels[2] });

			if (max_channel <= 0.0f)
			{
				// "dark" mode fades to black: no hue to preserve, so a dim neutral
				// glow and slightly heavier extinction stand in for the darkening.
				for (auto& a : albedo) { a = 0.35f; }
				for (auto& t : transmittance) { t = 0.90f; }
			}
			else
			{
				const float tint = std::clamp(
					shared::common::config::get().effects.fog_tint, 0.0f, 0.5f);

				for (int i = 0; i < 3; ++i)
				{
					// Brightness-normalized hue: the fade colour's darkness encoded
					// distance in the vanilla renderer and means nothing to a medium.
					const float hue = channels[i] / max_channel;

					albedo[i] = 0.25f + 0.70f * hue;
					transmittance[i] = 0.97f - tint * (1.0f - hue);
				}
			}
		}

		const auto push = [&](const char* key, const float(&value)[3])
		{
			const std::string formatted =
				std::format("{:.3f}, {:.3f}, {:.3f}", value[0], value[1], value[2]);

			if (bridge.SetConfigVariable(key, formatted.c_str()) == REMIXAPI_ERROR_CODE_SUCCESS) {
				shared::common::log("BRender", std::format("{} = {}", key, formatted));
			}
			else {
				shared::common::log("BRender", std::format("failed to set {}", key),
					shared::common::LOG_TYPE::LOG_TYPE_WARN);
			}
		};

		push("rtx.volumetrics.singleScatteringAlbedo", albedo);
		push("rtx.volumetrics.transmittanceColor", transmittance);
		return true;
	}

	/*
	 * Draws either every opaque run or every translucent one.
	 *
	 * Splitting the two is what stops a tyre track's cut-out area from punching a hole in the
	 * road: translucent geometry goes down after everything opaque and with depth writes off,
	 * so its fully transparent texels can no longer claim depth that the surface underneath
	 * then fails against. It is also the render state Remix reads to recognise a decal.
	 */
	uint32_t brender_inject::draw_pass(IDirect3DDevice9* dev, const pass_kind kind)
	{
		const bool combined = kind == pass_kind::combined;
		const bool blended = kind == pass_kind::blended;

		// In a combined pass blending is a property of each run, so it is set as they go.
		if (!combined) {
			dev->SetRenderState(D3DRS_ALPHABLENDENABLE, blended ? TRUE : FALSE);
		}
		dev->SetRenderState(D3DRS_ALPHATESTENABLE, blended ? TRUE : FALSE);
		dev->SetRenderState(D3DRS_ZWRITEENABLE, blended ? FALSE : TRUE);
		dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);

		bool transform_active = false;
		uint32_t draws = 0;

		// Every one of these is a bridge round trip, and the queue repeats the same texture and
		// the same blend mode across long runs of models. Tracking what is already bound turns
		// those into nothing at all.
		IDirect3DTexture9* bound_texture = nullptr;
		bool texture_bound = false;
		int bound_blend = -1;
		int bound_opacity = -1;
		const auto bind_texture = [&](IDirect3DTexture9* texture)
		{
			// Nothing is assumed about what the device already holds -- nGlide left it in an
			// unknown state -- so the first bind of a pass always goes through.
			if (!texture_bound || texture != bound_texture)
			{
				dev->SetTexture(0, texture);
				bound_texture = texture;
				texture_bound = true;
			}
		};
		const auto bind_blend = [&](const bool alpha)
		{
			if (const int wanted = alpha ? 1 : 0; wanted != bound_blend)
			{
				dev->SetRenderState(D3DRS_ALPHABLENDENABLE, alpha ? TRUE : FALSE);
				bound_blend = wanted;
			}
		};
		const auto bind_opacity = [&](const uint8_t opacity)
		{
			if (opacity != bound_opacity)
			{
				dev->SetRenderState(D3DRS_TEXTUREFACTOR,
					0x00FFFFFFu | (static_cast<uint32_t>(opacity) << 24));
				bound_opacity = opacity;
			}
		};

		dev->SetTransform(D3DTS_WORLD, &IDENTITY_MATRIX);

		for (const auto& chunk : m_chunks)
		{
			if (!chunk.sealed || !chunk.vertex_buffer || !chunk.triangle_count) {
				continue;
			}

			const bool chunk_blended = chunk.has_alpha || chunk.opacity < 255;
			if (!combined && chunk_blended != blended) {
				continue;
			}

			dev->SetStreamSource(0, chunk.vertex_buffer, 0, sizeof(ffp_vertex));
			dev->SetIndices(chunk.index_buffer);
			bind_texture(chunk.texture);
			bind_opacity(chunk.opacity);
			if (combined) {
				bind_blend(chunk_blended);
			}

			if (SUCCEEDED(dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0,
				chunk.vertex_count, 0, chunk.triangle_count)))
			{
				++draws;
			}
		}

		for (const auto& queued : m_queue)
		{
			const model_geometry& geometry = *queued.geometry;
			if (!combined && (blended ? !geometry.has_blended : !geometry.has_opaque)) {
				continue;
			}

			dev->SetTransform(D3DTS_WORLD, &queued.world);
			dev->SetStreamSource(0, geometry.vertex_buffer, 0, sizeof(ffp_vertex));
			dev->SetIndices(geometry.index_buffer);

			for (const auto& part : geometry.parts)
			{
				const bool part_blended = part_is_blended(part);
				if (!combined && part_blended != blended) {
					continue;
				}

				bind_texture(part.texture);
				bind_opacity(part.opacity);
				if (combined) {
					bind_blend(part_blended);
				}

				// Off for all but a handful of runs, so the stage state is only touched when
				// it actually has to change.
				if (part.texture_transform_active)
				{
					dev->SetTransform(D3DTS_TEXTURE0, &part.texture_transform);
					dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT2);
					transform_active = true;
				}
				else if (transform_active)
				{
					dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
					transform_active = false;
				}

				if (SUCCEEDED(dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0,
					geometry.vertex_count, part.index_start, part.triangle_count)))
				{
					++draws;
				}
			}
		}

		if (transform_active) {
			dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
		}

		return draws;
	}

	/*
	 * Expands the scene's line segments into camera-facing quads.
	 *
	 * BRender drew these as one-pixel screen-space lines, so their apparent thickness never
	 * depended on how far away they were. A fixed world-space width cannot reproduce that:
	 * sparks are struck against the player's own bodywork, a few units from the camera, where
	 * any width wide enough to survive at a distance reads as a solid slab. The half-width is
	 * therefore a fraction of the distance to the segment, which holds the on-screen thickness
	 * roughly constant the way the original did.
	 *
	 * The vertices are already in world space and change completely every frame, so there is
	 * nothing to cache: they go straight down as user-pointer draws, one per distinct colour.
	 */
	uint32_t brender_inject::submit_lines(IDirect3DDevice9* dev)
	{
		if (m_lines.empty()) {
			return 0;
		}

		const float width_per_unit = shared::common::config::get().effects.spark_width;
		if (width_per_unit <= 0.0f) {
			return 0;
		}

		// The inverse view's translation row is the camera's position in world space.
		const float camera[3] = { m_view_inverse.m[3][0], m_view_inverse.m[3][1], m_view_inverse.m[3][2] };

		log_spark_geometry(camera);

		// Sparks are light, not surface: they add to whatever is behind them and occlude
		// nothing. Additive blending also makes them read as emissive under path tracing
		// before any material tagging, and is what marks them out as particles to Remix.
		dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
		dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
		dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
		dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
		dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);

		// A streak's brightness is the texture's alone; the last material's opacity must not
		// carry over into it.
		dev->SetRenderState(D3DRS_TEXTUREFACTOR, 0xFFFFFFFFu);
		dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
		dev->SetTransform(D3DTS_WORLD, &IDENTITY_MATRIX);

		// Batched by the pair of end colours, since that is what picks the streak texture.
		const auto shade_of = [](const line_segment& s) {
			return (static_cast<uint64_t>(s.rgb_a) << 32) | s.rgb_b;
		};

		std::sort(m_lines.begin(), m_lines.end(),
			[&](const line_segment& a, const line_segment& b) { return shade_of(a) < shade_of(b); });

		uint32_t draws = 0;

		for (size_t i = 0; i < m_lines.size(); )
		{
			const uint64_t shade = shade_of(m_lines[i]);
			m_line_vertices.clear();

			for (; i < m_lines.size() && shade_of(m_lines[i]) == shade; ++i)
			{
				const line_segment& segment = m_lines[i];

				float along[3] = { segment.b[0] - segment.a[0],
				                   segment.b[1] - segment.a[1],
				                   segment.b[2] - segment.a[2] };

				const float towards_camera[3] = {
					camera[0] - (segment.a[0] + segment.b[0]) * 0.5f,
					camera[1] - (segment.a[1] + segment.b[1]) * 0.5f,
					camera[2] - (segment.a[2] + segment.b[2]) * 0.5f };

				float side[3];
				cross(along, towards_camera, side);

				// A segment pointing straight at the camera has no meaningful width. It is a
				// dot on screen either way, so anything perpendicular will do.
				if (!normalize(side))
				{
					const float up[3] = { 0.0f, 1.0f, 0.0f };
					cross(along, up, side);
					if (!normalize(side)) {
						continue;
					}
				}

				float normal[3];
				cross(side, along, normal);
				if (!normalize(normal)) {
					continue;
				}

				const float distance = sqrtf(towards_camera[0] * towards_camera[0]
					+ towards_camera[1] * towards_camera[1]
					+ towards_camera[2] * towards_camera[2]);

				const float half_width = width_per_unit * distance * 0.5f;

				for (int axis = 0; axis < 3; ++axis) {
					side[axis] *= half_width;
				}

				const ffp_vertex corners[4] = {
					{ segment.a[0] - side[0], segment.a[1] - side[1], segment.a[2] - side[2],
					  normal[0], normal[1], normal[2], 0.0f, 0.0f },
					{ segment.a[0] + side[0], segment.a[1] + side[1], segment.a[2] + side[2],
					  normal[0], normal[1], normal[2], 1.0f, 0.0f },
					{ segment.b[0] + side[0], segment.b[1] + side[1], segment.b[2] + side[2],
					  normal[0], normal[1], normal[2], 1.0f, 1.0f },
					{ segment.b[0] - side[0], segment.b[1] - side[1], segment.b[2] - side[2],
					  normal[0], normal[1], normal[2], 0.0f, 1.0f },
				};

				m_line_vertices.insert(m_line_vertices.end(), { corners[0], corners[1], corners[2] });
				m_line_vertices.insert(m_line_vertices.end(), { corners[0], corners[2], corners[3] });
			}

			if (m_line_vertices.empty()) {
				continue;
			}

			dev->SetTexture(0, spark_texture(dev,
				static_cast<uint32_t>(shade >> 32), static_cast<uint32_t>(shade)));

			if (SUCCEEDED(dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST,
				static_cast<UINT>(m_line_vertices.size() / 3), m_line_vertices.data(), sizeof(ffp_vertex))))
			{
				++draws;
			}
		}

		return draws;
	}

	const char* brender_inject::dynamic_reason_name(const dynamic_reason reason)
	{
		switch (reason)
		{
		case dynamic_reason::overlay:    return "overlay";
		case dynamic_reason::callback:   return "callback";
		case dynamic_reason::vanishing:  return "vanishing";
		case dynamic_reason::instanced:  return "instanced";
		case dynamic_reason::unbakeable: return "unbakeable";
		case dynamic_reason::moving:     return "moving";
		case dynamic_reason::probation:  return "probation";
		case dynamic_reason::unsealed:   return "unsealed";
		default:                         return "chunked";
		}
	}

	/*
	 * Reports a scene every 600, and the worst frame of each window alongside it.
	 *
	 * The worst frame is what is worth diagnosing and a periodic sample almost never lands
	 * on one -- a dip lasting a couple of seconds falls entirely between two samples. An
	 * all-time worst does not work either: one load stall early in the run holds the record
	 * for the whole session and every dip after it goes unreported.
	 */
	void brender_inject::log_performance(const frame_stats& stats)
	{
		if (stats.frame_ms > m_window_worst.frame_ms && stats.frame_ms < 1000.0) {
			m_window_worst = stats;
		}

		if (m_scenes_submitted != 0 && (m_scenes_submitted % 600) != 0) {
			return;
		}

		log_frame_stats("scene", stats);
		if (m_window_worst.scene != stats.scene) {
			log_frame_stats("worst since last report, scene", m_window_worst);
		}

		m_window_worst = {};
	}

	void brender_inject::log_frame_stats(const char* label, const frame_stats& stats)
	{
		const double fps = stats.frame_ms > 0.0 ? 1000.0 / stats.frame_ms : 0.0;

		// Whatever nothing above accounts for: game logic, physics, AI and nGlide's own work.
		// Negative only if the scene straddled a stall, so it is left signed.
		const double other_ms = stats.frame_ms - (stats.capture_ms + stats.bounds_ms
			+ stats.game_render_ms + stats.scene_end_ms + stats.submit_ms
			+ stats.present_ms + stats.overlay_ms);

		std::string dynamics;
		for (size_t i = 0; i < stats.dynamic_reasons.size(); ++i)
		{
			if (stats.dynamic_reasons[i]) {
				dynamics += std::format("{}{} {}", dynamics.empty() ? "" : ", ",
					dynamic_reason_name(static_cast<dynamic_reason>(i)), stats.dynamic_reasons[i]);
			}
		}

		shared::common::log("BRender", std::format(
			"{} {}: {:.1f} fps ({:.1f} ms) | {} models ({} baked in {} chunks),"
			" {} demoted (moved {}, swapped {}, deformed {}, animated {}, instanced {};"
			" {} relinks absorbed, {} rebaked) | dynamic: {} |"
			" {} draws (+{} glide, {} frame), {} verts, {} segments |"
			" game {:.2f} sceneend {:.2f} overlay {:.2f} capture {:.2f} bounds {:.2f}"
			" submit {:.2f} present {:.2f} other {:.2f} ms | {} updates, {} rebuilds |"
			" geometry cached {}",
			label, stats.scene, fps, stats.frame_ms, stats.models, stats.baked, stats.chunks,
			stats.demotions.total(), stats.demotions.moved, stats.demotions.swapped,
			stats.demotions.deformed, stats.demotions.animated, stats.demotions.instanced,
			stats.relinks_absorbed, stats.repromotions,
			dynamics.empty() ? "none" : dynamics,
			stats.draws, stats.glide_draws, stats.frame_draws,
			stats.vertices, stats.segments, stats.game_render_ms, stats.scene_end_ms,
			stats.overlay_ms, stats.capture_ms, stats.bounds_ms, stats.submit_ms,
			stats.present_ms, other_ms, stats.model_updates, stats.rebuilds, m_geometry.size()),
			shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
	}

	/*
	 * Names every material whose surface the injection now shades rather than taking the
	 * texture as final: prelit vertex colours, partial opacity, or both.
	 *
	 * These two are the only material state that can darken or fade geometry that used to
	 * come through untouched, so if a scene comes out wrong the log already says which
	 * materials were involved -- and whether VertexColour or MaterialOpacity is the switch
	 * to try. One line per distinct material name.
	 */
	void brender_inject::note_shaded_material(const game::br_material* material,
		const bool prelit, const uint8_t opacity)
	{
		const std::string name = material->identifier ? material->identifier : "<null>";
		const std::string state = std::format("{}{}{}",
			prelit ? "prelit" : "",
			prelit && opacity < 255 ? ", " : "",
			opacity < 255 ? std::format("opacity {}/255", opacity) : "");

		if (m_shaded_materials.try_emplace(name, state).second)
		{
			shared::common::log("BRender", std::format("shaded material: '{}' - {}", name, state),
				shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
		}
	}

	void brender_inject::note_untextured(const game::br_model* model, const game::br_material* material)
	{
		const char* reason = "texture upload failed";
		if (!material) {
			reason = "no material";
		}
		else if (!material->colour_map) {
			reason = "material has no colour_map";
		}

		const std::string name = model->identifier ? model->identifier : "<null>";
		if (m_untextured_models.try_emplace(name, reason).second) {
			shared::common::log("BRender", std::format("untextured: '{}' - {}", name, reason),
				shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
		}
	}

	void brender_inject::note_placement_drift(const game::br_model* model, const char* reason)
	{
		const std::string name = model->identifier ? model->identifier : "<null>";
		if (m_drift_models.try_emplace(name, reason).second) {
			shared::common::log("BRender", std::format("placement drift: '{}' - {}", name, reason),
				shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
		}
	}

	void brender_inject::note_not_injected(const game::br_model* model, const char* reason)
	{
		const std::string name = model->identifier ? model->identifier : "<null>";
		if (m_skipped_models.try_emplace(name, reason).second) {
			shared::common::log("BRender", std::format("not injected: '{}' - {}", name, reason),
				shared::common::LOG_TYPE::LOG_TYPE_WARN, false);
		}
	}

	void brender_inject::note_unsupported_style(const game::br_model* model, const uint32_t style)
	{
		if (!m_unsupported_styles.insert(style).second) {
			return;
		}

		shared::common::log("BRender", std::format("render style {} not injected - first seen on '{}'",
			style, model->identifier ? model->identifier : "<null>"),
			shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
	}

	IDirect3DTexture9* brender_inject::texture_for(IDirect3DDevice9* dev, const game::br_material* material)
	{
		if (!material || !material->colour_map) {
			return m_white_texture;
		}

		const auto pm = static_cast<const game::br_pixelmap*>(material->colour_map);
		if (!readable(pm, sizeof(*pm))) {
			return m_white_texture;
		}

		const pixelmap_identity identity = identify(pm);
		const auto it = m_textures.find(pm);
		if (it != m_textures.end())
		{
			if (it->second.identity == identity) {
				return it->second.texture ? it->second.texture : m_white_texture;
			}

			// A different image at the same address: the previous track's pixelmap was
			// freed and this one was allocated over it.
			if (it->second.texture) { it->second.texture->Release(); }
			m_textures.erase(it);
		}

		IDirect3DTexture9* texture = upload_pixelmap(dev, pm);
		m_textures[pm] = { texture, identity };

		if (texture) { ++m_textures_ok; }
		else { ++m_textures_failed; }

		return texture ? texture : m_white_texture;
	}

	IDirect3DTexture9* brender_inject::upload_pixelmap(IDirect3DDevice9* dev, const game::br_pixelmap* pm)
	{
		const uint32_t w = pm->width;
		const uint32_t h = pm->height;

		if (w == 0 || h == 0 || w > 4096 || h > 4096
			|| !readable(pm->pixels, static_cast<size_t>(pm->row_bytes) * h)) {
			return nullptr;
		}

		// An indexed pixelmap is only half the image; the palette it names holds the colours.
		// BRender attaches it as br_pixelmap::map, which is what InitSmokeStuff does when it
		// hands SMOKE.PIX the DRRENDER.PAL pixelmap (0x004FA340).
		std::array<uint32_t, 256> palette{};
		if (pm->type == game::BR_PMT_INDEX_8 && !build_palette(pm->map, palette))
		{
			if (m_unsupported_types.insert(pm->type).second) {
				shared::common::log("BRender", std::format(
					"indexed pixelmap '{}' has no usable palette", pm->identifier ? pm->identifier : "<null>"),
					shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
			}
			return nullptr;
		}

		// Bytes per pixel implied by the type must agree with row_bytes, otherwise the
		// layout has been misread and walking pixels would read arbitrary game memory.
		uint32_t bpp = 0;
		switch (pm->type)
		{
		case game::BR_PMT_INDEX_8:   bpp = 1; break;
		case game::BR_PMT_RGB_555:
		case game::BR_PMT_RGB_565:
		case game::BR_PMT_RGBA_4444: bpp = 2; break;
		case game::BR_PMT_RGB_888:   bpp = 3; break;
		case game::BR_PMT_RGBX_888:
		case game::BR_PMT_RGBA_8888: bpp = 4; break;
		default:
			if (m_unsupported_types.insert(pm->type).second) {
				shared::common::log("BRender", std::format("unhandled pixelmap type {:#04x} ({}x{}, row_bytes={})",
					pm->type, w, h, pm->row_bytes), shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
			}
			return nullptr;
		}

		if (pm->row_bytes < w * bpp) {
			return nullptr;
		}

		IDirect3DTexture9* tex = nullptr;
		if (FAILED(dev->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr)) || !tex) {
			return nullptr;
		}

		D3DLOCKED_RECT rect{};
		if (FAILED(tex->LockRect(0, &rect, nullptr, 0)))
		{
			tex->Release();
			return nullptr;
		}

		const auto src_base = static_cast<const uint8_t*>(pm->pixels);
		for (uint32_t y = 0; y < h; ++y)
		{
			const uint8_t* src = src_base + static_cast<size_t>(y) * pm->row_bytes;
			auto dst = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(rect.pBits) + static_cast<size_t>(y) * rect.Pitch);

			for (uint32_t x = 0; x < w; ++x, src += bpp)
			{
				uint32_t r = 0, g = 0, b = 0, a = 255;

				switch (pm->type)
				{
				case game::BR_PMT_INDEX_8:
				{
					const uint32_t entry = palette[*src];
					a = (entry >> 24) & 0xFF;
					r = (entry >> 16) & 0xFF;
					g = (entry >> 8) & 0xFF;
					b = entry & 0xFF;
					break;
				}
				case game::BR_PMT_RGB_555:
				{
					const uint16_t p = *reinterpret_cast<const uint16_t*>(src);
					r = ((p >> 10) & 0x1F) * 255 / 31;
					g = ((p >> 5) & 0x1F) * 255 / 31;
					b = (p & 0x1F) * 255 / 31;
					break;
				}
				case game::BR_PMT_RGB_565:
				{
					const uint16_t p = *reinterpret_cast<const uint16_t*>(src);
					r = ((p >> 11) & 0x1F) * 255 / 31;
					g = ((p >> 5) & 0x3F) * 255 / 63;
					b = (p & 0x1F) * 255 / 31;
					break;
				}
				case game::BR_PMT_RGBA_4444:
				{
					// Alpha occupies the high nibble: 'WATER' reads 0xA063 as 67% opaque
					// dark green, which is what the texture should be.
					const uint16_t p = *reinterpret_cast<const uint16_t*>(src);
					a = ((p >> 12) & 0xF) * 17;
					r = ((p >> 8) & 0xF) * 17;
					g = ((p >> 4) & 0xF) * 17;
					b = (p & 0xF) * 17;
					break;
				}
				case game::BR_PMT_RGB_888:
				case game::BR_PMT_RGBX_888:
					b = src[0]; g = src[1]; r = src[2];
					break;
				case game::BR_PMT_RGBA_8888:
					b = src[0]; g = src[1]; r = src[2]; a = src[3];
					break;
				default:
					break;
				}

				dst[x] = (a << 24) | (r << 16) | (g << 8) | b;
			}
		}

		tex->UnlockRect(0);
		return tex;
	}

	IDirect3DTexture9* brender_inject::solid_colour_texture(IDirect3DDevice9* dev, const uint32_t rgb)
	{
		if (const auto it = m_colour_textures.find(rgb); it != m_colour_textures.end()) {
			return it->second;
		}

		IDirect3DTexture9* tex = nullptr;
		if (FAILED(dev->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr)) || !tex) {
			return m_white_texture;
		}

		D3DLOCKED_RECT rect{};
		if (SUCCEEDED(tex->LockRect(0, &rect, nullptr, 0)))
		{
			*static_cast<uint32_t*>(rect.pBits) = 0xFF000000u | rgb;
			tex->UnlockRect(0);
		}

		m_colour_textures[rgb] = tex;
		return tex;
	}

	/*
	 * Paints a streak: the segment's own colour gradient along its length, over a bright core
	 * that falls off to nothing at both long edges.
	 *
	 * The gradient is the game's -- one end is written ff0000 and the other ffff00, and it is
	 * that pair, not either colour alone, that makes a spark look orange. The falloff across
	 * the width is the proxy's, and exists only because a quad has hard edges where BRender
	 * had a one-pixel line; it lives in alpha so the additive blend does the rest. Brightness
	 * along the length is left alone, since dimming one end would bias the very colours this
	 * is reproducing.
	 */
	IDirect3DTexture9* brender_inject::spark_texture(IDirect3DDevice9* dev,
		const uint32_t rgb_a, const uint32_t rgb_b)
	{
		const uint64_t key = (static_cast<uint64_t>(rgb_a) << 32) | rgb_b;
		if (const auto it = m_spark_textures.find(key); it != m_spark_textures.end()) {
			return it->second;
		}

		constexpr UINT SIZE = 32;

		IDirect3DTexture9* tex = nullptr;
		if (FAILED(dev->CreateTexture(SIZE, SIZE, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr)) || !tex) {
			return m_white_texture;
		}

		D3DLOCKED_RECT rect{};
		if (SUCCEEDED(tex->LockRect(0, &rect, nullptr, 0)))
		{
			const auto rows = static_cast<uint8_t*>(rect.pBits);

			for (UINT y = 0; y < SIZE; ++y)
			{
				const auto row = reinterpret_cast<uint32_t*>(rows + y * rect.Pitch);

				// v runs from end a to end b, which is how the quad lays its corners out.
				const float v = (static_cast<float>(y) + 0.5f) / SIZE;

				uint32_t shade = 0;
				for (int shift = 16; shift >= 0; shift -= 8)
				{
					const float from = static_cast<float>((rgb_a >> shift) & 0xFF);
					const float to = static_cast<float>((rgb_b >> shift) & 0xFF);
					const auto channel = static_cast<uint32_t>(from + (to - from) * v + 0.5f);
					shade |= channel << shift;
				}

				for (UINT x = 0; x < SIZE; ++x)
				{
					// u runs across the width, so this is the core falling off to the edges.
					const float u = (static_cast<float>(x) + 0.5f) / SIZE;
					const float offset = fabsf(u - 0.5f) * 2.0f;
					const float across = 1.0f - offset * offset;

					const auto a = static_cast<uint32_t>(across * 255.0f + 0.5f);
					row[x] = (a << 24) | shade;
				}
			}

			tex->UnlockRect(0);
		}

		m_spark_textures[key] = tex;
		return tex;
	}

	/*
	 * Reports the shape of one scene's worth of sparks, once per session.
	 *
	 * Segment length and camera distance are what the width setting has to be judged against,
	 * and neither is knowable from outside the process.
	 */
	void brender_inject::log_spark_geometry(const float camera[3])
	{
		if (m_logged_spark_geometry || m_lines.empty()) {
			return;
		}
		m_logged_spark_geometry = true;

		float shortest = FLT_MAX, longest = 0.0f, total = 0.0f;
		float nearest = FLT_MAX, furthest = 0.0f;
		std::set<uint64_t> colours;

		for (const auto& segment : m_lines)
		{
			const float length = sqrtf(
				powf(segment.b[0] - segment.a[0], 2.0f) +
				powf(segment.b[1] - segment.a[1], 2.0f) +
				powf(segment.b[2] - segment.a[2], 2.0f));

			const float distance = sqrtf(
				powf(camera[0] - segment.a[0], 2.0f) +
				powf(camera[1] - segment.a[1], 2.0f) +
				powf(camera[2] - segment.a[2], 2.0f));

			shortest = std::min(shortest, length);
			longest = std::max(longest, length);
			total += length;
			nearest = std::min(nearest, distance);
			furthest = std::max(furthest, distance);
			colours.insert((static_cast<uint64_t>(segment.rgb_a) << 32) | segment.rgb_b);
		}

		std::string swatches;
		for (const uint64_t shade : colours)
		{
			if (swatches.size() > 60) { swatches += " ..."; break; }
			swatches += std::format(" {:06x}->{:06x}",
				static_cast<uint32_t>(shade >> 32), static_cast<uint32_t>(shade));
		}

		shared::common::log("BRender", std::format(
			"sparks: {} segments, length {:.3f}/{:.3f}/{:.3f} min/avg/max,"
			" camera distance {:.2f}-{:.2f}, {} colours:{}",
			m_lines.size(), shortest, total / m_lines.size(), longest,
			nearest, furthest, colours.size(), swatches),
			shared::common::LOG_TYPE::LOG_TYPE_GREEN, true);
	}

	void brender_inject::note_flat_colour(const game::br_model* model, const game::br_material* material)
	{
		const std::string name = model->identifier ? model->identifier : "<null>";
		if (m_untextured_models.try_emplace(name, "flat colour").second)
		{
			shared::common::log("BRender", std::format("flat colour: '{}' material '{}' = {:#08x}",
				name, readable(material->identifier, 1) ? material->identifier : "<null>",
				material->colour & 0x00FFFFFFu),
				shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
		}
	}

	void brender_inject::ensure_white_texture(IDirect3DDevice9* dev)
	{
		if (m_white_texture) {
			return;
		}

		if (FAILED(dev->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &m_white_texture, nullptr))) {
			m_white_texture = nullptr;
			return;
		}

		D3DLOCKED_RECT rect{};
		if (SUCCEEDED(m_white_texture->LockRect(0, &rect, nullptr, 0)))
		{
			*static_cast<uint32_t*>(rect.pBits) = 0xFFFFFFFFu;
			m_white_texture->UnlockRect(0);
		}
	}
}
