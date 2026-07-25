#include "std_include.hpp"
#include "brender_inject.hpp"

namespace comp
{
	namespace
	{
		// Declared explicitly rather than via D3DFVF_XYZ|NORMAL|TEX1. Remix reported
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

		void __cdecl hk_scene_begin(game::br_actor* world, game::br_actor* camera, void* colour, void* depth)
		{
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
			// Must run first: the original frees the authored face array on the way out.
			if (const auto self = brender_inject::get(); self && model) {
				self->learn_materials(model);
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

		m_vertices.reserve(65536);
		m_indices.reserve(65536);
		m_draws.reserve(4096);

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
			shared::common::log("BRender", "Hooked the BRender scene walk — model-space injection armed.",
				shared::common::LOG_TYPE::LOG_TYPE_GREEN, true);
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

	namespace
	{
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
	}

	// Reports each model that renders white the first time it is seen. Which of the three
	// causes applies decides where the fix belongs: a missing material means the group's
	// token never resolved, a missing colour_map means the material is untextured by design,
	// and a failed upload means an unhandled pixel format.
	void brender_inject::note_untextured(const batched_draw& draw)
	{
		const char* reason = "texture upload failed";
		if (!draw.material) {
			reason = "no material";
		}
		else if (!draw.material->colour_map) {
			reason = "material has no colour_map";
		}

		const std::string name = draw.model_name ? draw.model_name : "<null>";
		if (m_untextured_models.try_emplace(name, reason).second) {
			shared::common::log("BRender", std::format("untextured: '{}' - {}", name, reason),
				shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
		}
	}

	brender_inject::texture_entry brender_inject::texture_for(IDirect3DDevice9* dev, game::br_material* material)
	{
		if (!material || !material->colour_map) {
			return { m_white_texture, false };
		}

		const auto pm = static_cast<const game::br_pixelmap*>(material->colour_map);
		if (const auto it = m_textures.find(pm); it != m_textures.end()) {
			return it->second.texture ? it->second : texture_entry{ m_white_texture, false };
		}

		const texture_entry entry{ upload_pixelmap(dev, pm), pm->type == game::BR_PMT_RGBA_4444
			|| pm->type == game::BR_PMT_RGBA_8888 };
		m_textures[pm] = entry;

		if (entry.texture) { ++m_textures_ok; }
		else { ++m_textures_failed; }

		return entry.texture ? entry : texture_entry{ m_white_texture, false };
	}

	IDirect3DTexture9* brender_inject::upload_pixelmap(IDirect3DDevice9* dev, const game::br_pixelmap* pm)
	{
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
					b = src[0]; g = src[1]; r = src[2];
					break;
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

	void brender_inject::begin_scene(game::br_actor* camera)
	{
		m_camera = camera;
		m_camera_valid = false;
		m_capturing = m_enabled && !m_overlay_scene;

		m_vertices.clear();
		m_indices.clear();
		m_draws.clear();
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

		game::br_matrix34 model_to_world{};
		mul34(model_to_view, m_view_inverse, model_to_world);
		const D3DMATRIX world = to_d3d(model_to_world);

		for (uint16_t g = 0; g < prepared->ngroups; ++g)
		{
			const game::v1_group& group = prepared->groups[g];
			if (!group.vertices || !group.faces || !group.nvertices || !group.nfaces) {
				continue;
			}

			batched_draw draw{};
			draw.world = world;
			draw.model_name = model->identifier ? model->identifier : "<null>";

			// BrModelUpdate writes a zero token for faces that carry no material of their
			// own — wheels and car shells among them. Those inherit whatever material the
			// render call was given, which is the argument BrZbModelRender received.
			if (group.material_token == 0) {
				draw.material = fallback_material;
			}
			else if (const auto it = m_materials.find(group.material_token); it != m_materials.end()) {
				draw.material = it->second;
			}

			draw.vertex_offset = static_cast<uint32_t>(m_vertices.size());
			draw.vertex_count = group.nvertices;
			draw.index_offset = static_cast<uint32_t>(m_indices.size());
			draw.triangle_count = group.nfaces;

			for (uint16_t v = 0; v < group.nvertices; ++v)
			{
				const game::v1_online_vertex& src = group.vertices[v];
				ffp_vertex dst{};
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
				m_vertices.push_back(dst);
			}

			for (uint16_t f = 0; f < group.nfaces; ++f)
			{
				const game::v1_online_face& face = group.faces[f];
				m_indices.push_back(face.v[0]);
				m_indices.push_back(face.v[1]);
				m_indices.push_back(face.v[2]);
			}

			m_draws.push_back(draw);
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

		// One line per distinct failure so a silent scene is never ambiguous.
		if (m_draws.empty())
		{
			if (!m_warned_no_geometry) {
				m_warned_no_geometry = true;
				shared::common::log("BRender", "world scene rendered but no prepared geometry was captured",
					shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
			}
			return;
		}

		// The game opens several Begin/End scenes per frame against the same 640x480 buffer:
		// the race view (~450 draws) plus one-draw 3D HUD widgets such as the opponent-car
		// icon. Forwarding a widget hands Remix a second camera with a ~35 unit far plane,
		// and it path-traces the icon instead of the track. Only the race view is large
		// enough to clear this bar.
		if (m_draws.size() < MIN_WORLD_SCENE_DRAWS)
		{
			if (m_scenes_logged < 12)
			{
				++m_scenes_logged;
				shared::common::log("BRender", std::format("skipping {}-draw widget scene", m_draws.size()),
					shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
			}
			return;
		}

		const auto dev = shared::globals::d3d_device;
		if (!dev)
		{
			if (!m_warned_no_device) {
				m_warned_no_device = true;
				shared::common::log("BRender", "geometry captured but nGlide has not created a D3D9 device yet",
					shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
			}
			return;
		}

		submit(dev);
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

	void brender_inject::submit(IDirect3DDevice9* dev)
	{
		D3DMATRIX projection{};
		if (!build_projection(projection))
		{
			if (!m_warned_no_camera) {
				m_warned_no_camera = true;
				shared::common::log("BRender", "could not build a projection — camera actor has no usable br_camera",
					shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
			}
			return;
		}

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
		dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
		dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);

		// Remix treats an untextured draw as a candidate sky, which excludes it from camera
		// selection, so materials without a usable colour_map still get flat white.
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

		// Carmageddon 2 addresses well outside 0..1 — the log showed u = 7.0 on track pieces.
		dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
		dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
		dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
		dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);

		// Stale bindings from the Glide path would otherwise be captured as this geometry's
		// material by Remix.
		for (DWORD stage = 1; stage < 8; ++stage) {
			dev->SetTexture(stage, nullptr);
		}

		// Sampled here rather than after the state block is restored, which would only ever
		// report nGlide's state back.
		if (m_scenes_submitted == 0)
		{
			DWORD prev_texcoord_index = 0;
			dev->GetTextureStageState(0, D3DTSS_TEXCOORDINDEX, &prev_texcoord_index);

			IDirect3DVertexDeclaration9* bound_decl = nullptr;
			dev->GetVertexDeclaration(&bound_decl);
			shared::common::log("BRender", std::format("  decl bound={} | stage0 TEXCOORDINDEX now {}",
				bound_decl ? "yes" : "NO", prev_texcoord_index),
				shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
			if (bound_decl) { bound_decl->Release(); }
		}

		m_stat_draws = 0;
		m_stat_vertices = 0;

		for (const auto& draw : m_draws)
		{
			dev->SetTransform(D3DTS_WORLD, &draw.world);

			const texture_entry tex = texture_for(dev, draw.material);
			if (!tex.texture || tex.texture == m_white_texture) {
				note_untextured(draw);
			}

			dev->SetTexture(0, tex.texture);
			dev->SetRenderState(D3DRS_ALPHABLENDENABLE, tex.has_alpha ? TRUE : FALSE);

			const HRESULT hr = dev->DrawIndexedPrimitiveUP(
				D3DPT_TRIANGLELIST,
				0,
				draw.vertex_count,
				draw.triangle_count,
				&m_indices[draw.index_offset],
				D3DFMT_INDEX16,
				&m_vertices[draw.vertex_offset],
				sizeof(ffp_vertex));

			if (SUCCEEDED(hr))
			{
				++m_stat_draws;
				m_stat_vertices += draw.vertex_count;
			}
		}

		saved->Apply();
		saved->Release();

		// Restoring the block reverts VIEW and PROJECTION to whatever nGlide last had, which
		// is nothing — it draws exclusively with pre-transformed vertices and never touches
		// the fixed-function transforms. Leaving our camera installed costs nGlide nothing
		// and keeps it visible to Remix for the rest of the frame.
		dev->SetTransform(D3DTS_VIEW, &view);
		dev->SetTransform(D3DTS_PROJECTION, &projection);

		if (m_scenes_submitted == 0)
		{
			// Remix only path-traces when the render target it sees matches the presented
			// resolution, so the surface nGlide has bound is worth recording once.
			UINT rt_w = 0, rt_h = 0;
			if (IDirect3DSurface9* rt = nullptr; SUCCEEDED(dev->GetRenderTarget(0, &rt)) && rt)
			{
				D3DSURFACE_DESC desc{};
				if (SUCCEEDED(rt->GetDesc(&desc))) { rt_w = desc.Width; rt_h = desc.Height; }
				rt->Release();
			}

			const auto cam = static_cast<const game::br_camera*>(m_camera->type_data);
			shared::common::log("BRender", std::format(
				"first world scene: {} draws, {} verts | rendertarget {}x{} | camera=({:.1f},{:.1f},{:.1f}) | fov={:.1f}deg aspect={:.3f} hither={:.2f} yon={:.1f}",
				m_stat_draws, m_stat_vertices, rt_w, rt_h,
				m_view_inverse.m[3][0], m_view_inverse.m[3][1], m_view_inverse.m[3][2],
				static_cast<float>(cam->field_of_view) * BR_ANGLE_TO_RADIANS * 57.2957795f,
				cam->aspect, cam->hither_z, cam->yon_z),
				shared::common::LOG_TYPE::LOG_TYPE_GREEN, true);

			// The full view matrix, so handedness and orthonormality can be checked by eye.
			for (int r = 0; r < 4; ++r)
			{
				shared::common::log("BRender", std::format("  view[{}] = {:8.3f} {:8.3f} {:8.3f} {:8.3f}",
					r, view.m[r][0], view.m[r][1], view.m[r][2], view.m[r][3]),
					shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
			}
			shared::common::log("BRender", std::format("  proj diag = {:.3f} {:.3f} {:.3f} | m[2][3]={:.1f} m[3][2]={:.3f}",
				projection.m[0][0], projection.m[1][1], projection.m[2][2],
				projection.m[2][3], projection.m[3][2]),
				shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);

			size_t resolved = 0;
			for (const auto& d : m_draws) {
				if (d.material) { ++resolved; }
			}

			shared::common::log("BRender", std::format(
				"  materials: {}/{} draws resolved, {} known | textures: {} uploaded, {} failed",
				resolved, m_draws.size(), m_materials.size(), m_textures_ok, m_textures_failed),
				resolved ? shared::common::LOG_TYPE::LOG_TYPE_GREEN : shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);


		}
		else if ((m_scenes_submitted % 300) == 0)
		{
			shared::common::log("BRender", std::format("scene {}: {} draws, {} verts injected",
				m_scenes_submitted, m_stat_draws, m_stat_vertices),
				shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
		}

		++m_scenes_submitted;
	}
}
