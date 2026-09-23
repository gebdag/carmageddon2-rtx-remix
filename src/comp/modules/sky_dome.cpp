#include "std_include.hpp"
#include "sky_dome.hpp"

namespace comp
{
	namespace
	{
		// The panorama is equirectangular: x is azimuth over a full turn, y is elevation from
		// the zenith (row 0) to the nadir. The widest horizon texture is 256 texels repeated
		// seven times, 1792 around, so 2048 keeps every source texel.
		constexpr uint32_t PANORAMA_WIDTH = 2048;
		constexpr uint32_t PANORAMA_HEIGHT = 1024;

		// The shell is a lat/long sphere, along whose rows and columns an equirectangular
		// panorama's UV is exactly linear. 32 rings keep a pole triangle within a texel of
		// the direction it stands for.
		constexpr uint32_t SHELL_COLUMNS = 64;
		constexpr uint32_t SHELL_RINGS = 32;

		// How far past the top (or bottom) of the texture its edge row is smeared into one
		// colour, as a fraction of the way to the pole.
		constexpr double POLE_FADE = 0.75;

		constexpr double PI = 3.14159265358979323846;
		constexpr double DEG_TO_RAD = PI / 180.0;

		struct shell_vertex
		{
			float x, y, z;
			float nx, ny, nz;
			float u, v;
		};

		constexpr D3DVERTEXELEMENT9 SHELL_DECL[] =
		{
			{ 0, 0,  D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
			{ 0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL,   0 },
			{ 0, 24, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
			D3DDECL_END()
		};

		/*
		 * The world direction a panorama azimuth and elevation stand for.
		 *
		 * The game's own layout (DrawHorizon, 0x00445CB0): azimuth 0 faces world -Z and
		 * grows clockwise seen from above, towards +X. BRender is right-handed with +Y up,
		 * so that is left to right on screen for a camera looking down -Z.
		 */
		void direction(const double azimuth_deg, const double elevation_deg, float out[3])
		{
			const double a = azimuth_deg * DEG_TO_RAD;
			const double e = elevation_deg * DEG_TO_RAD;
			out[0] = static_cast<float>(std::sin(a) * std::cos(e));
			out[1] = static_cast<float>(std::sin(e));
			out[2] = static_cast<float>(-std::cos(a) * std::cos(e));
		}

		struct rgb
		{
			double r = 0.0, g = 0.0, b = 0.0;

			rgb operator+(const rgb& o) const { return { r + o.r, g + o.g, b + o.b }; }
			rgb operator-(const rgb& o) const { return { r - o.r, g - o.g, b - o.b }; }
			rgb operator*(const double s) const { return { r * s, g * s, b * s }; }
		};

		rgb unpack(const uint32_t argb)
		{
			return { static_cast<double>((argb >> 16) & 0xFF),
			         static_cast<double>((argb >> 8) & 0xFF),
			         static_cast<double>(argb & 0xFF) };
		}

		uint32_t pack(const rgb& c)
		{
			const auto channel = [](const double v) {
				return static_cast<uint32_t>(std::clamp(v, 0.0, 255.0) + 0.5);
			};
			return 0xFF000000u | (channel(c.r) << 16) | (channel(c.g) << 8) | channel(c.b);
		}

		/*
		 * Samples the horizon texture in texel coordinates, wrapping around the horizon and
		 * clamping at the top and bottom rows.
		 */
		class source_image
		{
		public:
			explicit source_image(const sky_dome::horizon& h)
				: m_pixels(h.pixels), m_width(h.width), m_height(h.height)
			{
				// Prefix sums of the top and bottom rows, so an edge row can be box-filtered
				// over any stretch of azimuth in constant time.
				m_top_sums = row_prefix_sums(0);
				m_bottom_sums = row_prefix_sums(m_height - 1);
			}

			uint32_t width() const { return m_width; }
			uint32_t height() const { return m_height; }

			rgb bilinear(const double x, const double y) const
			{
				const double fx = x - 0.5, fy = y - 0.5;
				const double x0 = std::floor(fx), y0 = std::floor(fy);
				const double ax = fx - x0, ay = fy - y0;

				const auto at = [this](const double tx, const double ty) {
					const int64_t w = m_width;
					const int64_t cx = ((static_cast<int64_t>(tx) % w) + w) % w;
					const int64_t cy = std::clamp<int64_t>(static_cast<int64_t>(ty), 0, m_height - 1);
					return unpack(m_pixels[cy * m_width + cx]);
				};

				return at(x0, y0) * ((1.0 - ax) * (1.0 - ay)) + at(x0 + 1, y0) * (ax * (1.0 - ay))
					+ at(x0, y0 + 1) * ((1.0 - ax) * ay) + at(x0 + 1, y0 + 1) * (ax * ay);
			}

			/*
			 * The top or bottom row, averaged over `radius` texels either side of `x`.
			 * A radius of half the width is the whole row: the colour of the pole.
			 */
			rgb edge_row(const bool top, const double x, const double radius) const
			{
				if (radius < 1.0) {
					return bilinear(x, top ? 0.5 : m_height - 0.5);
				}

				const auto& sums = top ? m_top_sums : m_bottom_sums;
				const int64_t first = static_cast<int64_t>(std::floor(x - radius));
				const int64_t last = static_cast<int64_t>(std::floor(x + radius));
				return (cumulative(sums, last + 1) - cumulative(sums, first))
					* (1.0 / static_cast<double>(last - first + 1));
			}

		private:
			std::vector<rgb> row_prefix_sums(const uint32_t row) const
			{
				std::vector<rgb> sums(m_width + 1);
				for (uint32_t x = 0; x < m_width; ++x) {
					sums[x + 1] = sums[x] + unpack(m_pixels[static_cast<size_t>(row) * m_width + x]);
				}
				return sums;
			}

			// Sum of texels [0, k) along a row repeated forever in both directions.
			rgb cumulative(const std::vector<rgb>& sums, const int64_t k) const
			{
				const int64_t w = m_width;
				const int64_t turns = (k >= 0) ? k / w : -((-k + w - 1) / w);
				return sums[m_width] * static_cast<double>(turns) + sums[static_cast<size_t>(k - turns * w)];
			}

			const uint32_t* m_pixels;
			uint32_t m_width;
			uint32_t m_height;
			std::vector<rgb> m_top_sums;
			std::vector<rgb> m_bottom_sums;
		};

		double smoothstep(const double t)
		{
			const double c = std::clamp(t, 0.0, 1.0);
			return c * c * (3.0 - 2.0 * c);
		}

		/*
		 * Fills one panorama row.
		 *
		 * Between the texture's top and bottom rows this is the game's own layout: the
		 * texture `repetitions` times around the horizon, `height / degrees` texels per
		 * degree of elevation, row `horizon_row` at elevation 0. Beyond them the texture has
		 * nothing to say, and simply clamping would draw its edge row up to the pole as
		 * streaks converging on the zenith. Instead the edge row is box-filtered over a
		 * widening stretch of azimuth until, three quarters of the way to the pole, it is
		 * one colour: the average of the whole row.
		 */
		void bake_row(const source_image& src, const sky_dome::horizon& h, const uint32_t y, uint32_t* out)
		{
			const double elevation = 90.0 - (static_cast<double>(y) + 0.5) / PANORAMA_HEIGHT * 180.0;
			const double texels_per_degree_v = static_cast<double>(src.height()) / h.degrees;
			const double texels_per_degree_u = static_cast<double>(src.width()) * h.repetitions / 360.0;

			const double top_elevation = h.horizon_row / texels_per_degree_v;
			const double bottom_elevation = (h.horizon_row - src.height()) / texels_per_degree_v;
			const double v = h.horizon_row - elevation * texels_per_degree_v;

			const bool above = elevation > top_elevation;
			const bool below = elevation < bottom_elevation;
			double radius = 0.0;
			if (above) {
				radius = smoothstep((elevation - top_elevation) / ((90.0 - top_elevation) * POLE_FADE));
			}
			else if (below) {
				radius = smoothstep((bottom_elevation - elevation) / ((90.0 + bottom_elevation) * POLE_FADE));
			}
			radius *= src.width() * 0.5;

			for (uint32_t x = 0; x < PANORAMA_WIDTH; ++x)
			{
				const double azimuth = (static_cast<double>(x) + 0.5) / PANORAMA_WIDTH * 360.0;
				const double u = azimuth * texels_per_degree_u;
				out[x] = pack(above || below ? src.edge_row(above, u, radius) : src.bilinear(u, v));
			}
		}

		// Halves a level with a 2x2 box, wrapping in azimuth and clamping at the poles.
		std::vector<uint32_t> downsample(const std::vector<uint32_t>& level, const uint32_t w, const uint32_t h)
		{
			const uint32_t nw = std::max(w / 2, 1u), nh = std::max(h / 2, 1u);
			std::vector<uint32_t> out(static_cast<size_t>(nw) * nh);

			for (uint32_t y = 0; y < nh; ++y)
			{
				const uint32_t y0 = std::min(y * 2, h - 1), y1 = std::min(y * 2 + 1, h - 1);
				for (uint32_t x = 0; x < nw; ++x)
				{
					const uint32_t x0 = (x * 2) % w, x1 = (x * 2 + 1) % w;
					const rgb sum = unpack(level[y0 * w + x0]) + unpack(level[y0 * w + x1])
						+ unpack(level[y1 * w + x0]) + unpack(level[y1 * w + x1]);
					out[y * nw + x] = pack(sum * 0.25);
				}
			}

			return out;
		}
	}

	bool sky_dome::bake(IDirect3DDevice9* dev, const horizon& source, const source_key& key)
	{
		if (!source.pixels || source.width == 0 || source.height == 0
			|| source.repetitions <= 0.0f || source.degrees <= 0.0f) {
			return false;
		}

		const source_image src(source);
		std::vector<uint32_t> level(static_cast<size_t>(PANORAMA_WIDTH) * PANORAMA_HEIGHT);
		for (uint32_t y = 0; y < PANORAMA_HEIGHT; ++y) {
			bake_row(src, source, y, level.data() + static_cast<size_t>(y) * PANORAMA_WIDTH);
		}

		uint32_t levels = 1;
		for (uint32_t w = PANORAMA_WIDTH, h = PANORAMA_HEIGHT; w > 1 || h > 1; w = std::max(w / 2, 1u), h = std::max(h / 2, 1u)) {
			++levels;
		}

		IDirect3DTexture9* texture = nullptr;
		if (FAILED(dev->CreateTexture(PANORAMA_WIDTH, PANORAMA_HEIGHT, levels, 0, D3DFMT_A8R8G8B8,
			D3DPOOL_MANAGED, &texture, nullptr)) || !texture) {
			return false;
		}

		uint32_t w = PANORAMA_WIDTH, h = PANORAMA_HEIGHT;
		for (uint32_t lvl = 0; lvl < levels; ++lvl)
		{
			if (lvl > 0)
			{
				level = downsample(level, w, h);
				w = std::max(w / 2, 1u);
				h = std::max(h / 2, 1u);
			}

			D3DLOCKED_RECT rect{};
			if (FAILED(texture->LockRect(lvl, &rect, nullptr, 0)))
			{
				texture->Release();
				return false;
			}
			for (uint32_t y = 0; y < h; ++y)
			{
				std::memcpy(static_cast<uint8_t*>(rect.pBits) + static_cast<size_t>(y) * rect.Pitch,
					level.data() + static_cast<size_t>(y) * w, static_cast<size_t>(w) * sizeof(uint32_t));
			}
			texture->UnlockRect(lvl);
		}

		if (m_panorama) {
			m_panorama->Release();
		}
		m_panorama = texture;
		m_key = key;
		return true;
	}

	/*
	 * Builds the unit sphere once. It never changes, so it is one mesh with one hash on
	 * every track; the eye position and radius travel in WORLD, which Remix does not hash.
	 */
	bool sky_dome::ensure_shell(IDirect3DDevice9* dev)
	{
		if (m_vertex_buffer && m_index_buffer && m_vertex_decl) {
			return true;
		}

		std::vector<shell_vertex> vertices;
		vertices.reserve((SHELL_COLUMNS + 1) * (SHELL_RINGS + 1));
		for (uint32_t ring = 0; ring <= SHELL_RINGS; ++ring)
		{
			const double v = static_cast<double>(ring) / SHELL_RINGS;
			const bool pole = ring == 0 || ring == SHELL_RINGS;

			for (uint32_t column = 0; column <= SHELL_COLUMNS; ++column)
			{
				// Every pole vertex is its own copy, so each takes the middle of its
				// column's azimuth rather than one edge of it.
				const double u = (static_cast<double>(column) + (pole ? 0.5 : 0.0)) / SHELL_COLUMNS;

				shell_vertex vertex{};
				float p[3];
				direction(u * 360.0, 90.0 - v * 180.0, p);
				vertex.x = p[0]; vertex.y = p[1]; vertex.z = p[2];
				vertex.nx = -p[0]; vertex.ny = -p[1]; vertex.nz = -p[2];
				vertex.u = static_cast<float>(u);
				vertex.v = static_cast<float>(v);
				vertices.push_back(vertex);
			}
		}

		std::vector<uint16_t> indices;
		indices.reserve(SHELL_COLUMNS * SHELL_RINGS * 6);
		const uint32_t stride = SHELL_COLUMNS + 1;
		for (uint32_t ring = 0; ring < SHELL_RINGS; ++ring)
		{
			for (uint32_t column = 0; column < SHELL_COLUMNS; ++column)
			{
				const auto a = static_cast<uint16_t>(ring * stride + column);
				const auto b = static_cast<uint16_t>(a + 1);
				const auto c = static_cast<uint16_t>(a + stride);
				const auto d = static_cast<uint16_t>(c + 1);
				indices.insert(indices.end(), { a, c, d, a, d, b });
			}
		}

		const UINT vertex_bytes = static_cast<UINT>(vertices.size() * sizeof(shell_vertex));
		const UINT index_bytes = static_cast<UINT>(indices.size() * sizeof(uint16_t));

		if (FAILED(dev->CreateVertexBuffer(vertex_bytes, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &m_vertex_buffer, nullptr))
			|| FAILED(dev->CreateIndexBuffer(index_bytes, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &m_index_buffer, nullptr))
			|| FAILED(dev->CreateVertexDeclaration(SHELL_DECL, &m_vertex_decl)))
		{
			release();
			return false;
		}

		void* data = nullptr;
		if (FAILED(m_vertex_buffer->Lock(0, 0, &data, 0)))
		{
			release();
			return false;
		}
		std::memcpy(data, vertices.data(), vertex_bytes);
		m_vertex_buffer->Unlock();

		if (FAILED(m_index_buffer->Lock(0, 0, &data, 0)))
		{
			release();
			return false;
		}
		std::memcpy(data, indices.data(), index_bytes);
		m_index_buffer->Unlock();

		m_vertex_count = static_cast<uint32_t>(vertices.size());
		m_triangle_count = static_cast<uint32_t>(indices.size() / 3);
		return true;
	}

	void sky_dome::draw(IDirect3DDevice9* dev, const float eye[3], const float radius)
	{
		if (!m_panorama || !ensure_shell(dev)) {
			return;
		}

		D3DMATRIX world{};
		world.m[0][0] = radius;
		world.m[1][1] = radius;
		world.m[2][2] = radius;
		world.m[3][0] = eye[0];
		world.m[3][1] = eye[1];
		world.m[3][2] = eye[2];
		world.m[3][3] = 1.0f;
		dev->SetTransform(D3DTS_WORLD, &world);

		// A viewport at depth 1 is what makes this sky to Remix (rtx.skyMinZThreshold).
		D3DVIEWPORT9 viewport{};
		dev->GetViewport(&viewport);
		D3DVIEWPORT9 sky_viewport = viewport;
		sky_viewport.MinZ = 1.0f;
		sky_viewport.MaxZ = 1.0f;
		dev->SetViewport(&sky_viewport);

		// A background: nothing to test against and nothing to occlude, whatever the
		// radius. Unfogged and unblended, and seen from inside, so never culled.
		dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
		dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
		dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
		dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
		dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
		dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);

		dev->SetTexture(0, m_panorama);
		dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
		dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
		dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
		dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
		dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
		dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
		dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);

		// Wrap around the horizon, clamp at the poles so the zenith row never bleeds into
		// the nadir. Anisotropic, because the panorama's polar rows are squeezed hard in u.
		dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
		dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
		dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
		dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_ANISOTROPIC);
		dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
		dev->SetSamplerState(0, D3DSAMP_MAXANISOTROPY, 8);

		// A real vertex and index buffer. Remix replays a sky draw into its matte and its six
		// probe faces against the buffers bound when the draw is committed, which a
		// DrawIndexedPrimitiveUP would already have unbound.
		dev->SetVertexDeclaration(m_vertex_decl);
		dev->SetStreamSource(0, m_vertex_buffer, 0, sizeof(shell_vertex));
		dev->SetIndices(m_index_buffer);
		dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, m_vertex_count, 0, m_triangle_count);

		dev->SetViewport(&viewport);
	}

	void sky_dome::release()
	{
		for (IUnknown** resource : std::initializer_list<IUnknown**>{
			reinterpret_cast<IUnknown**>(&m_panorama),
			reinterpret_cast<IUnknown**>(&m_vertex_buffer),
			reinterpret_cast<IUnknown**>(&m_index_buffer),
			reinterpret_cast<IUnknown**>(&m_vertex_decl) })
		{
			if (*resource)
			{
				(*resource)->Release();
				*resource = nullptr;
			}
		}
	}
}
