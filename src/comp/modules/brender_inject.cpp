#include "std_include.hpp"
#include "brender_inject.hpp"
#include "shared/common/config.hpp"

namespace comp
{
	namespace
	{
		// Declared explicitly rather than via D3DFVF_XYZ|NORMAL|TEX1. Remix reports
		// "trying to bind a texture to a mesh without UVs" for the FVF path, so the
		// texcoord usage is spelled out here instead of inferred.
		constexpr D3DVERTEXELEMENT9 INJECT_DECL[] =
		{
			{ 0,  0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
			{ 0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL,   0 },
			{ 0, 24, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
			D3DDECL_END()
		};

		// A br_angle covers a full turn in 16 bits.
		constexpr float BR_ANGLE_TO_RADIANS = 6.283185307f / 65536.0f;

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

		bool same_placement(const game::br_matrix34& a, const game::br_matrix34& b)
		{
			// Loose on translation because the game's own maths jitters in the low bits;
			// anything that actually moves shifts far more than this.
			constexpr float eps = 1e-3f;
			for (int row = 0; row < 4; ++row)
			{
				for (int col = 0; col < 3; ++col)
				{
					if (fabsf(a.m[row][col] - b.m[row][col]) > eps) {
						return false;
					}
				}
			}
			return true;
		}

		uint64_t placement_key(const game::br_model* model, const game::br_matrix34& world)
		{
			uint64_t hash = 1469598103934665603ull;
			const auto mix = [&hash](const void* data, const size_t bytes)
			{
				const auto p = static_cast<const uint8_t*>(data);
				for (size_t i = 0; i < bytes; ++i) {
					hash = (hash ^ p[i]) * 1099511628211ull;
				}
			};

			mix(&model, sizeof(model));
			mix(&world, sizeof(world));
			return hash;
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

		// A pointer is only worth dereferencing if the whole span is committed and readable.
		bool readable(const void* p, const size_t bytes)
		{
			if (!p || reinterpret_cast<uintptr_t>(p) < 0x10000) {
				return false;
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
			return static_cast<const uint8_t*>(p) + bytes <= end;
		}

		// BrZbActorRender drops any actor this reports as outside, which for path tracing
		// removes exactly the geometry that should still occlude and bounce light: walls
		// behind and beside the camera. Anything within the bubble is downgraded to
		// PARTIAL rather than INSIDE so BRender still clips it correctly for its own
		// rasterized pass.
		int __cdecl hk_bounds_test(void* self, uint32_t* out_token, const float* bounds)
		{
			const int result = o_bounds_test(self, out_token, bounds);

			const float radius = shared::common::config::get().culling.bubble_radius;
			if (radius <= 0.0f || !out_token || *out_token != game::BRT_BOUNDS_OUTSIDE || !bounds) {
				return result;
			}

			game::br_matrix34 model_to_view{};
			if (!query_model_to_view(model_to_view)) {
				return result;
			}

			// bounds is min[3] then max[3] in model space. The camera sits at the origin in
			// view space, so the transformed centre is already the camera-relative position.
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
			if (distance - extent <= radius) {
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
				self->begin_scene(camera);
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

		void __cdecl hk_scene_end()
		{
			o_scene_end();

			if (const auto self = brender_inject::get(); self) {
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

			o_scene_render(world, camera, colour, depth);

			if (self) {
				self->end_scene();
				self->set_overlay_scene(false);
			}
		}

		void __cdecl hk_model_render(game::br_actor* actor, game::br_model* model, void* material, void* env,
		                             uint32_t style, uint32_t bounds, uint32_t use_custom)
		{
			if (const auto self = brender_inject::get(); self && model) {
				self->capture_model(model, static_cast<game::br_material*>(material));
			}

			o_model_render(actor, model, material, env, style, bounds, use_custom);
		}

		void __cdecl hk_model_update(game::br_model* model, uint16_t flags)
		{
			if (const auto self = brender_inject::get(); self && model)
			{
				// Must run first: the original frees the authored face array on the way out.
				self->learn_materials(model);
				self->invalidate_geometry(model);
			}

			o_model_update(model, flags);
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

		if (ok) {
			shared::common::log("BRender", std::format(
				"Hooked the BRender scene walk - model-space injection armed. Static merging {}.",
				shared::common::config::get().optimization.merge_static_geometry ? "ON" : "off"),
				shared::common::LOG_TYPE::LOG_TYPE_GREEN, true);
		}
	}

	brender_inject::~brender_inject()
	{
		release_static_batches();

		for (auto& [model, geometry] : m_geometry) {
			release_geometry(geometry);
		}

		for (auto& [pm, entry] : m_textures) {
			if (entry.texture) { entry.texture->Release(); }
		}

		for (auto& [rgb, texture] : m_colour_textures) {
			if (texture) { texture->Release(); }
		}

		if (m_white_texture) { m_white_texture->Release(); }
		if (m_vertex_decl) { m_vertex_decl->Release(); }
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
			shared::common::log("BRender", std::format("bounds test hooked at {:#010x} - bubble radius {:.1f}",
				reinterpret_cast<uint32_t>(target), shared::common::config::get().culling.bubble_radius),
				shared::common::LOG_TYPE::LOG_TYPE_GREEN, true);
		}
		else {
			shared::common::log("BRender", "failed to hook the renderer bounds test - bubble disabled",
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

		// Deliberately does not touch the static batches. BrModelUpdate fires on level
		// models routinely without their geometry changing, and rebuilding costs a full
		// re-upload of every static vertex plus a Remix acceleration structure rebuild.
		// A model that sits at the world origin with an identity transform is scenery; the
		// things that actually deform are cars, and they have real transforms.
	}

	void brender_inject::release_geometry(model_geometry& geometry)
	{
		if (geometry.vertex_buffer) { geometry.vertex_buffer->Release(); }
		if (geometry.index_buffer) { geometry.index_buffer->Release(); }
		geometry.vertex_buffer = nullptr;
		geometry.index_buffer = nullptr;
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
	const brender_inject::model_geometry* brender_inject::geometry_for(IDirect3DDevice9* dev,
		game::br_model* model, game::br_material* fallback_material)
	{
		if (const auto it = m_geometry.find(model); it != m_geometry.end())
		{
			it->second.last_used_scene = m_scenes_submitted;
			if (!it->second.dirty) {
				return it->second.vertex_buffer ? &it->second : nullptr;
			}

			// Rebuilt into the same node below; erasing would invalidate any pointer the
			// current scene's queue already holds.
			release_geometry(it->second);
		}

		std::vector<ffp_vertex> vertices;
		std::vector<geometry_part> parts;
		std::vector<uint32_t> indices;

		model_geometry geometry{};
		geometry.last_used_scene = m_scenes_submitted;

		// 16-bit indices are enough for any single model this game ships; anything larger is
		// corrupt data rather than a real mesh.
		if (!extract_geometry(dev, model, fallback_material, vertices, parts, indices)
			|| vertices.size() > 0xFFFF)
		{
			m_geometry[model] = geometry;
			return nullptr;
		}

		const UINT vertex_bytes = static_cast<UINT>(vertices.size() * sizeof(ffp_vertex));
		const UINT index_bytes = static_cast<UINT>(indices.size() * sizeof(uint16_t));

		if (FAILED(dev->CreateVertexBuffer(vertex_bytes, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED,
			&geometry.vertex_buffer, nullptr))
			|| FAILED(dev->CreateIndexBuffer(index_bytes, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16,
				D3DPOOL_MANAGED, &geometry.index_buffer, nullptr)))
		{
			release_geometry(geometry);
			m_geometry[model] = geometry;
			return nullptr;
		}

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

		auto& slot = m_geometry[model];
		slot = std::move(geometry);
		return slot.vertex_buffer ? &slot : nullptr;
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
		struct group_ref { const game::v1_group* group; game::br_material* material; uint32_t vertex_base; };
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

			groups.push_back({ &group, material, vertex_base + total_vertices });
			total_vertices += group.nvertices;
		}

		if (groups.empty() || total_vertices == 0) {
			return false;
		}

		vertices.resize(vertex_base + total_vertices);

		for (const auto& ref : groups)
		{
			for (uint16_t v = 0; v < ref.group->nvertices; ++v)
			{
				const game::v1_online_vertex& src = ref.group->vertices[v];
				ffp_vertex& dst = vertices[ref.vertex_base + v];
				// BrModelUpdate subtracts the pivot when it builds the prepared block;
				// adding it back restores true model space.
				dst.x = src.px + model->pivot.v[0];
				dst.y = src.py + model->pivot.v[1];
				dst.z = src.pz + model->pivot.v[2];
				dst.nx = src.nx;
				dst.ny = src.ny;
				dst.nz = src.nz;
				dst.u = src.u;
				dst.v = src.v;
			}
		}

		// Emit indices grouped by material so each material forms one contiguous draw.
		std::vector<game::br_material*> ordered;
		for (const auto& ref : groups)
		{
			if (std::find(ordered.begin(), ordered.end(), ref.material) == ordered.end()) {
				ordered.push_back(ref.material);
			}
		}

		for (game::br_material* material : ordered)
		{
			texture_entry tex = texture_for(dev, material);

			// An untextured material is not a failure -- it is a flat colour, held in
			// br_material::colour. Feeding Remix a solid swatch gives it a real albedo and
			// a distinct hash per colour, so the swatch stays replaceable.
			if ((!tex.texture || tex.texture == m_white_texture) && material)
			{
				tex = solid_colour_texture(dev, material->colour & 0x00FFFFFFu);
				note_flat_colour(model, material);
			}
			else if (!tex.texture || tex.texture == m_white_texture) {
				note_untextured(model, nullptr);
			}

			geometry_part part{};
			part.texture = tex.texture;
			part.alpha = tex.alpha;
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

	/*
	 * Extracts a model once, bakes its placement into the vertices and keeps the result.
	 *
	 * Everything the merged batches need is captured here, so a rebuild never touches the
	 * game's memory again -- models are freed between races and re-reading them would be a
	 * use-after-free.
	 */
	bool brender_inject::bake_static_model(game::br_model* model, game::br_material* fallback_material,
		const game::br_matrix34& world)
	{
		const auto dev = shared::globals::d3d_device;
		if (!dev) {
			return false;
		}

		static_instance instance{};
		instance.model = model;

		if (!extract_geometry(dev, model, fallback_material, instance.vertices, instance.parts, instance.indices)) {
			return false;
		}

		// The merged batches draw with WORLD = identity, so the placement lives in the data.
		for (auto& v : instance.vertices)
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

		m_static_models.insert_or_assign(placement_key(model, world), std::move(instance));
		m_static_dirty = true;
		return true;
	}

	// A model that turns up somewhere new was never scenery. Its baked copies have to go
	// immediately, hitch or not: leaving them would show the object in two places at once.
	void brender_inject::forget_static_model(game::br_model* model)
	{
		m_moving_models.insert(model);
		m_placements.erase(model);

		for (auto it = m_static_models.begin(); it != m_static_models.end(); )
		{
			if (it->second.model == model)
			{
				it = m_static_models.erase(it);
				m_static_dirty = true;
				m_static_urgent = true;
			}
			else {
				++it;
			}
		}
	}

	void brender_inject::release_static_batches()
	{
		for (auto& batch : m_static_batches)
		{
			if (batch.vertex_buffer) { batch.vertex_buffer->Release(); }
			if (batch.index_buffer) { batch.index_buffer->Release(); }
		}
		m_static_batches.clear();
	}

	/*
	 * Merges every static model into one buffer per texture.
	 *
	 * Frame time measured almost perfectly linear in draw count -- roughly 3.5 ms plus 8 us
	 * per draw -- and vertex count barely registered, so the level's ~800 world models at
	 * ~4 materials each were the entire cost. They all sit at the world origin with an
	 * identity transform, which means they can share draws. Merging them turns thousands of
	 * per-model draws into one per distinct texture, and since the merged buffers never
	 * change, Remix keeps the acceleration structure it builds for them.
	 *
	 * Everything static is submitted every frame regardless of visibility. That is cheaper
	 * than culling it and removes the light leak the bubble was working around.
	 */
	void brender_inject::rebuild_static_batches(IDirect3DDevice9* dev)
	{
		release_static_batches();

		struct accumulator
		{
			IDirect3DTexture9* texture;
			alpha_mode alpha;
			std::vector<ffp_vertex> vertices;
			std::vector<uint32_t> indices;
		};
		std::vector<accumulator> batches;

		for (const auto& [key, instance] : m_static_models)
		{
			const std::vector<ffp_vertex>& vertices = instance.vertices;
			const std::vector<uint32_t>& indices = instance.indices;

			for (const auto& part : instance.parts)
			{
				auto it = std::find_if(batches.begin(), batches.end(),
					[&](const accumulator& a) { return a.texture == part.texture && a.alpha == part.alpha; });

				if (it == batches.end())
				{
					batches.push_back({ part.texture, part.alpha, {}, {} });
					it = batches.end() - 1;
				}

				// Indices are model-local; rebase them onto this batch's vertex block.
				const uint32_t base = static_cast<uint32_t>(it->vertices.size());
				it->vertices.insert(it->vertices.end(), vertices.begin(), vertices.end());

				const uint32_t end = part.index_start + part.triangle_count * 3u;
				for (uint32_t i = part.index_start; i < end; ++i) {
					it->indices.push_back(base + indices[i]);
				}
			}
		}

		for (auto& acc : batches)
		{
			if (acc.vertices.empty() || acc.indices.empty()) {
				continue;
			}

			static_batch batch{};
			batch.texture = acc.texture;
			batch.alpha = acc.alpha;
			batch.vertex_count = static_cast<uint32_t>(acc.vertices.size());
			batch.triangle_count = static_cast<uint32_t>(acc.indices.size()) / 3u;

			const UINT vertex_bytes = batch.vertex_count * sizeof(ffp_vertex);
			const UINT index_bytes = static_cast<UINT>(acc.indices.size()) * sizeof(uint32_t);

			// A merged batch spans far more than 65535 vertices, so these are 32-bit.
			if (FAILED(dev->CreateVertexBuffer(vertex_bytes, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED,
				&batch.vertex_buffer, nullptr))
				|| FAILED(dev->CreateIndexBuffer(index_bytes, D3DUSAGE_WRITEONLY, D3DFMT_INDEX32,
					D3DPOOL_MANAGED, &batch.index_buffer, nullptr)))
			{
				if (batch.vertex_buffer) { batch.vertex_buffer->Release(); }
				if (batch.index_buffer) { batch.index_buffer->Release(); }
				continue;
			}

			void* mapped = nullptr;
			if (SUCCEEDED(batch.vertex_buffer->Lock(0, vertex_bytes, &mapped, 0)))
			{
				memcpy(mapped, acc.vertices.data(), vertex_bytes);
				batch.vertex_buffer->Unlock();
			}

			if (SUCCEEDED(batch.index_buffer->Lock(0, index_bytes, &mapped, 0)))
			{
				memcpy(mapped, acc.indices.data(), index_bytes);
				batch.index_buffer->Unlock();
			}

			m_static_batches.push_back(batch);
		}

		m_static_dirty = false;
		m_static_urgent = false;
		m_static_rebuilt_scene = m_scenes_submitted;

		uint32_t total_vertices = 0;
		for (const auto& batch : m_static_batches) {
			total_vertices += batch.vertex_count;
		}

		shared::common::log("BRender", std::format("static geometry merged: {} models -> {} draws, {} verts",
			m_static_models.size(), m_static_batches.size(), total_vertices),
			shared::common::LOG_TYPE::LOG_TYPE_GREEN, true);
	}

	void brender_inject::begin_scene(game::br_actor* camera)
	{
		if (shared::common::config::get().culling.bubble_radius > 0.0f) {
			install_bounds_test_hook();
		}

		m_camera = camera;
		m_camera_valid = false;
		m_capturing = !m_overlay_scene;
		m_scene_models = 0;
		m_queue.clear();
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

	void brender_inject::capture_model(game::br_model* model, game::br_material* fallback_material)
	{
		if (!m_capturing || !m_camera_valid) {
			return;
		}

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
			return;
		}

		game::br_matrix34 model_to_view{};
		if (!query_model_to_view(model_to_view)) {
			return;
		}

		++m_scene_models;

		game::br_matrix34 model_to_world{};
		mul34(model_to_view, m_view_inverse, model_to_world);

		// Scenery holds one placement for the whole race, so it can be baked into the merged
		// batches whatever its transform. Only things that actually move -- cars, wheels,
		// spinning powerups -- need an instance of their own. A model has to hold still for
		// several frames first: baking on sight would bake every car at its starting
		// position and leave a ghost there the moment it drove off.
		if (shared::common::config::get().optimization.merge_static_geometry
			&& !m_moving_models.contains(model))
		{
			auto [placement, first_sighting] = m_placements.try_emplace(
				model, placement_record{ model_to_world, 1, false });

			if (first_sighting) {
				// Falls through and draws dynamically until it has proved it is stationary.
			}
			else if (!same_placement(placement->second.world, model_to_world))
			{
				forget_static_model(model);
			}
			else
			{
				++placement->second.sightings;

				if (placement->second.sightings >= STATIC_PROMOTE_SIGHTINGS)
				{
					if (!placement->second.baked) {
						placement->second.baked = bake_static_model(model, fallback_material, model_to_world);
					}

					if (placement->second.baked) {
						return;
					}
				}
			}
		}

		queued_model queued{};
		queued.geometry = nullptr;
		queued.world = to_d3d(model_to_world);
		queued.model_name = model->identifier ? model->identifier : "<null>";

		// The device is needed to build the buffers, and it exists by the time any scene
		// runs; storing the model here and resolving in submit would need a second lookup.
		if (const auto dev = shared::globals::d3d_device; dev) {
			queued.geometry = geometry_for(dev, model, fallback_material);
		}

		if (queued.geometry) {
			m_queue.push_back(queued);
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

		LARGE_INTEGER start{};
		QueryPerformanceCounter(&start);

		const D3DMATRIX view = to_d3d(m_world_to_view);

		// nGlide owns the device for the rest of the frame, so every state this replay
		// touches is captured and put back afterwards.
		IDirect3DStateBlock9* saved = nullptr;
		if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &saved))) {
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
		dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
		dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
		dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
		dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
		dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);

		ensure_white_texture(dev);
		dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
		dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
		dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
		dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
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

		// Additions can wait for the next window, but a removal means something is currently
		// drawn in two places and has to be corrected now.
		if (m_static_dirty
			&& (m_static_urgent || m_scenes_submitted - m_static_rebuilt_scene >= STATIC_REBUILD_INTERVAL_SCENES))
		{
			rebuild_static_batches(dev);
		}

		uint32_t draws = 0;
		uint32_t vertices = 0;

		const D3DMATRIX identity = to_d3d(game::br_matrix34{ { {1,0,0}, {0,1,0}, {0,0,1}, {0,0,0} } });
		dev->SetTransform(D3DTS_WORLD, &identity);

		for (const auto& batch : m_static_batches)
		{
			dev->SetStreamSource(0, batch.vertex_buffer, 0, sizeof(ffp_vertex));
			dev->SetIndices(batch.index_buffer);
			dev->SetTexture(0, batch.texture);
			dev->SetRenderState(D3DRS_ALPHABLENDENABLE,
				batch.alpha == alpha_mode::Blend ? TRUE : FALSE);

			if (SUCCEEDED(dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0,
				batch.vertex_count, 0, batch.triangle_count)))
			{
				++draws;
				vertices += batch.vertex_count;
			}
		}

		for (const auto& queued : m_queue)
		{
			const model_geometry& geometry = *queued.geometry;

			dev->SetTransform(D3DTS_WORLD, &queued.world);
			dev->SetStreamSource(0, geometry.vertex_buffer, 0, sizeof(ffp_vertex));
			dev->SetIndices(geometry.index_buffer);

			for (const auto& part : geometry.parts)
			{
				dev->SetTexture(0, part.texture);
				dev->SetRenderState(D3DRS_ALPHABLENDENABLE,
					part.alpha == alpha_mode::Blend ? TRUE : FALSE);

				if (SUCCEEDED(dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0,
					geometry.vertex_count, part.index_start, part.triangle_count)))
				{
					++draws;
				}
			}

			vertices += geometry.vertex_count;
		}

		saved->Apply();
		saved->Release();

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
		stats.models = static_cast<uint32_t>(m_queue.size() + m_static_models.size());
		stats.submit_ms = static_cast<double>(end.QuadPart - start.QuadPart) / m_ticks_per_ms;
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
	 * Reports the slowest scene seen so far, and every scene that beats it.
	 *
	 * A periodic sample cannot catch a stall that lasts a handful of frames, which is
	 * exactly the case worth diagnosing. Tracking the worst frame instead means driving
	 * until it hitches leaves the offending numbers in the log.
	 */
	void brender_inject::log_performance(const frame_stats& stats)
	{
		const bool first = m_scenes_submitted == 0;
		const bool worse = stats.frame_ms > m_worst.frame_ms && stats.frame_ms < 1000.0;

		if (!first && !worse && (m_scenes_submitted % 600) != 0) {
			return;
		}

		if (worse) {
			m_worst = stats;
		}

		const double fps = stats.frame_ms > 0.0 ? 1000.0 / stats.frame_ms : 0.0;
		shared::common::log("BRender", std::format(
			"scene {}: {:.1f} fps ({:.1f} ms) | {} models, {} draws, {} verts | submit {:.2f} ms | geometry cached {}{}",
			m_scenes_submitted, fps, stats.frame_ms, stats.models, stats.draws, stats.vertices,
			stats.submit_ms, m_geometry.size(), worse && !first ? "  <-- new worst" : ""),
			worse && !first ? shared::common::LOG_TYPE::LOG_TYPE_WARN : shared::common::LOG_TYPE::LOG_TYPE_DEFAULT,
			false);
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

	brender_inject::texture_entry brender_inject::texture_for(IDirect3DDevice9* dev, game::br_material* material)
	{
		if (!material || !material->colour_map) {
			return { m_white_texture, alpha_mode::Opaque };
		}

		const auto pm = static_cast<const game::br_pixelmap*>(material->colour_map);
		if (const auto it = m_textures.find(pm); it != m_textures.end()) {
			return it->second.texture ? it->second : texture_entry{ m_white_texture, alpha_mode::Opaque };
		}

		// Only a texture that actually goes transparent somewhere needs blending. Water's
		// alpha sits uniformly high, so it renders as an ordinary opaque surface; smoke and
		// foliage fade to nothing and keep blending.
		uint32_t min_alpha = 255;
		texture_entry entry{ upload_pixelmap(dev, pm, min_alpha), alpha_mode::Opaque };
		if (min_alpha < ALPHA_BLEND_THRESHOLD) {
			entry.alpha = alpha_mode::Blend;
		}

		m_textures[pm] = entry;

		if (entry.texture)
		{
			++m_textures_ok;
			if (entry.alpha == alpha_mode::Blend) {
				++m_textures_blended;
			}
		}
		else {
			++m_textures_failed;
		}

		return entry.texture ? entry : texture_entry{ m_white_texture, alpha_mode::Opaque };
	}

	IDirect3DTexture9* brender_inject::upload_pixelmap(IDirect3DDevice9* dev, const game::br_pixelmap* pm,
		uint32_t& min_alpha)
	{
		min_alpha = 255;

		const uint32_t w = pm->width;
		const uint32_t h = pm->height;

		if (w == 0 || h == 0 || w > 4096 || h > 4096
			|| !readable(pm->pixels, static_cast<size_t>(pm->row_bytes) * h)) {
			return nullptr;
		}

		// Bytes per pixel implied by the type must agree with row_bytes, otherwise the
		// layout has been misread and walking pixels would read arbitrary game memory.
		uint32_t bpp = 0;
		switch (pm->type)
		{
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

				min_alpha = (std::min)(min_alpha, a);
				dst[x] = (a << 24) | (r << 16) | (g << 8) | b;
			}
		}

		tex->UnlockRect(0);
		return tex;
	}

	brender_inject::texture_entry brender_inject::solid_colour_texture(IDirect3DDevice9* dev, const uint32_t rgb)
	{
		if (const auto it = m_colour_textures.find(rgb); it != m_colour_textures.end()) {
			return { it->second, alpha_mode::Opaque };
		}

		IDirect3DTexture9* tex = nullptr;
		if (FAILED(dev->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr)) || !tex) {
			return { m_white_texture, alpha_mode::Opaque };
		}

		D3DLOCKED_RECT rect{};
		if (SUCCEEDED(tex->LockRect(0, &rect, nullptr, 0)))
		{
			*static_cast<uint32_t*>(rect.pBits) = 0xFF000000u | rgb;
			tex->UnlockRect(0);
		}

		m_colour_textures[rgb] = tex;
		return { tex, alpha_mode::Opaque };
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
