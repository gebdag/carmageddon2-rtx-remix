#include "std_include.hpp"
#include "level_footprint.hpp"

namespace comp
{
	namespace
	{
		// Ground cell size in world units. Track pieces sit ~60 units apart and the fog ends
		// hundreds of units out, so an 8-unit step is well inside what the eye can tell.
		constexpr float CELL = 8.0f;

		// A single actor wider than this many cells (a skybox-sized backdrop, a bad matrix)
		// marks only its corners rather than flooding the grid.
		constexpr int32_t MAX_FILL = 512;

		// While the city streams in, something bakes nearly every frame; the edge is rebuilt
		// at most this often (in queries, one per frame) rather than scanning the grid each time.
		constexpr uint32_t REBUILD_INTERVAL = 30;

		int32_t cell_of(const float v) { return static_cast<int32_t>(std::floor(v / CELL)); }
	}

	void level_footprint::add_bounds(const game::br_vector3& min, const game::br_vector3& max,
		const game::br_matrix34& model_to_world)
	{
		float lo[3] = { std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity(),
			std::numeric_limits<float>::infinity() };
		float hi[3] = { -lo[0], -lo[1], -lo[2] };

		// BRender vectors are rows: p' = p * M, with the translation in row 3.
		for (int corner = 0; corner < 8; ++corner)
		{
			const float p[3] = {
				(corner & 1 ? max : min).v[0],
				(corner & 2 ? max : min).v[1],
				(corner & 4 ? max : min).v[2],
			};
			for (int i = 0; i < 3; ++i)
			{
				const float w = p[0] * model_to_world.m[0][i] + p[1] * model_to_world.m[1][i]
					+ p[2] * model_to_world.m[2][i] + model_to_world.m[3][i];
				if (!std::isfinite(w)) {
					return;
				}
				lo[i] = std::min(lo[i], w);
				hi[i] = std::max(hi[i], w);
			}
		}

		m_min_y = std::min(m_min_y, lo[1]);
		m_max_y = std::max(m_max_y, hi[1]);

		const int32_t x0 = cell_of(lo[0]), x1 = cell_of(hi[0]);
		const int32_t z0 = cell_of(lo[2]), z1 = cell_of(hi[2]);
		if (x1 - x0 > MAX_FILL || z1 - z0 > MAX_FILL)
		{
			m_cells.insert({ key(x0, z0), key(x0, z1), key(x1, z0), key(x1, z1) });
		}
		else
		{
			for (int32_t x = x0; x <= x1; ++x) {
				for (int32_t z = z0; z <= z1; ++z) {
					m_cells.insert(key(x, z));
				}
			}
		}
		m_edge_dirty = true;
	}

	void level_footprint::clear()
	{
		m_cells.clear();
		m_edge.clear();
		m_edge_dirty = false;
		m_queries_since_rebuild = 0;
		m_min_y = std::numeric_limits<float>::infinity();
		m_max_y = -std::numeric_limits<float>::infinity();
	}

	void level_footprint::rebuild_edge()
	{
		m_edge.clear();
		for (const int64_t k : m_cells)
		{
			const int32_t x = static_cast<int32_t>(k >> 32);
			const int32_t z = static_cast<int32_t>(static_cast<uint32_t>(k));
			if (m_cells.contains(key(x + 1, z)) && m_cells.contains(key(x - 1, z))
				&& m_cells.contains(key(x, z + 1)) && m_cells.contains(key(x, z - 1))) {
				continue;
			}
			m_edge.emplace_back((x + 0.5f) * CELL, (z + 0.5f) * CELL);
		}
		m_edge_dirty = false;
		m_queries_since_rebuild = 0;
	}

	float level_footprint::farthest_in_view(const float position[3], const float forward[3], const float half_angle)
	{
		++m_queries_since_rebuild;
		if (m_edge_dirty && (m_edge.empty() || m_queries_since_rebuild >= REBUILD_INTERVAL)) {
			rebuild_edge();
		}

		const float flat = std::sqrt(forward[0] * forward[0] + forward[2] * forward[2]);
		const bool looking_straight_down = flat < 1e-3f;
		const float fx = looking_straight_down ? 0.0f : forward[0] / flat;
		const float fz = looking_straight_down ? 0.0f : forward[2] / flat;
		const float cos_limit = std::cos(std::min(half_angle, 3.1415926f));

		float farthest = 0.0f;
		for (const auto& [cx, cz] : m_edge)
		{
			const float dx = cx - position[0];
			const float dz = cz - position[2];
			const float distance = std::sqrt(dx * dx + dz * dz);

			// A cell counts when any part of it is in view: its centre's direction is widened
			// by the angle the cell spans at that distance.
			if (!looking_straight_down && distance > CELL)
			{
				const float cos_to = (dx * fx + dz * fz) / distance;
				const float spread = std::atan(CELL / distance);
				if (std::acos(std::clamp(cos_to, -1.0f, 1.0f)) - spread > std::acos(cos_limit)) {
					continue;
				}
			}
			farthest = std::max(farthest, distance + CELL * 0.71f);
		}

		if (farthest <= 0.0f) {
			return 0.0f;
		}
		const float dy = std::max(std::abs(position[1] - m_min_y), std::abs(position[1] - m_max_y));
		return std::sqrt(farthest * farthest + dy * dy);
	}
}
