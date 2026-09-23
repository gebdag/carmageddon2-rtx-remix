#pragma once

namespace comp
{
	/*
	 * A sky Remix can rasterize: a sphere around the camera, drawn with a panorama baked
	 * from the track's horizon texture.
	 *
	 * The game's own horizon is a band of geometry pinned to the camera and scrolled with
	 * its yaw. It only covers what the screen can see, so it says nothing about the sky
	 * overhead, behind or below -- and Remix needs all of it. A draw Remix classifies as sky
	 * is replayed into a screen-sized sky matte (what a primary ray sees when it misses)
	 * and into a cube probe around the camera (what lights the level from the sky), then
	 * hidden from the acceleration structure. A closed shell centred on the eye makes sense
	 * from the camera in every direction at once, which is what the six probe faces need.
	 *
	 * The shell is classified by its viewport rather than its texture: a viewport whose
	 * MinZ is at or above rtx.skyMinZThreshold (default 1) is sky to Remix whatever it
	 * draws, so no texture hash has to be tagged per track.
	 */
	class sky_dome
	{
	public:
		/*
		 * The track's horizon texture and how the race TXT says to lay it out.
		 *
		 * The texture wraps the horizon `repetitions` times, spans `degrees` of elevation
		 * from its top row to its bottom row, and its row `horizon_row` lies on the
		 * horizon. Pixels are tightly packed A8R8G8B8, `width * height` of them.
		 */
		struct horizon
		{
			const uint32_t* pixels;
			uint32_t width;
			uint32_t height;
			float repetitions;
			float degrees;
			float horizon_row;
		};

		// What a baked panorama was made from. A bake is redone only when this changes.
		struct source_key
		{
			const void* pixels;
			uint32_t width;
			uint32_t height;
			float repetitions;
			float degrees;
			float horizon_row;

			bool operator==(const source_key&) const = default;
		};

		~sky_dome() { release(); }

		// Whether `key` differs from what the current panorama was baked from.
		bool needs_bake(const source_key& key) const { return !m_panorama || key != m_key; }

		// Replaces the panorama. Returns false, keeping the old one, if the bake failed.
		bool bake(IDirect3DDevice9* dev, const horizon& source, const source_key& key);

		/*
		 * Draws the shell as sky, if a panorama exists.
		 *
		 * Changes the vertex declaration, WORLD, the viewport, stream 0, the index buffer,
		 * texture stage 0 and sampler 0, and the depth, blend, alpha-test, cull and fog
		 * render states. The viewport is put back; the caller owns the rest.
		 *
		 * Args:
		 *   eye: camera position in world space.
		 *   radius: shell radius; must lie between the projection's near and far planes.
		 */
		void draw(IDirect3DDevice9* dev, const float eye[3], float radius);

		void release();

	private:
		bool ensure_shell(IDirect3DDevice9* dev);

		IDirect3DTexture9* m_panorama = nullptr;
		source_key m_key{};

		IDirect3DVertexBuffer9* m_vertex_buffer = nullptr;
		IDirect3DIndexBuffer9* m_index_buffer = nullptr;
		IDirect3DVertexDeclaration9* m_vertex_decl = nullptr;
		uint32_t m_vertex_count = 0;
		uint32_t m_triangle_count = 0;
	};
}
