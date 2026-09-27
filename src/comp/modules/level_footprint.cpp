#include "std_include.hpp"
#include "level_footprint.hpp"

namespace comp
{
	void level_footprint::add_bounds(const game::br_vector3& min, const game::br_vector3& max,
		const game::br_matrix34& model_to_world)
	{
		// BRender vectors are rows: p' = p * M, with the translation in row 3.
		for (int corner = 0; corner < 8; ++corner)
		{
			const float p[3] = {
				(corner & 1 ? max : min).v[0],
				(corner & 2 ? max : min).v[1],
				(corner & 4 ? max : min).v[2],
			};
			float w[3];
			for (int i = 0; i < 3; ++i) {
				w[i] = p[0] * model_to_world.m[0][i] + p[1] * model_to_world.m[1][i]
					+ p[2] * model_to_world.m[2][i] + model_to_world.m[3][i];
			}
			if (!std::isfinite(w[0]) || !std::isfinite(w[1]) || !std::isfinite(w[2])) {
				return;
			}
			m_pending.push_back({ w[0], w[2] });
			m_min_y = std::min(m_min_y, w[1]);
			m_max_y = std::max(m_max_y, w[1]);
		}
	}

	void level_footprint::clear()
	{
		m_hull.clear();
		m_pending.clear();
		m_min_y = std::numeric_limits<float>::infinity();
		m_max_y = -std::numeric_limits<float>::infinity();
	}

	// Andrew's monotone chain over the old hull and the new corners.
	void level_footprint::rebuild_hull()
	{
		std::vector<point> points = std::move(m_pending);
		m_pending.clear();
		points.insert(points.end(), m_hull.begin(), m_hull.end());
		std::sort(points.begin(), points.end(), [](const point& a, const point& b)
		{
			return a.x < b.x || (a.x == b.x && a.z < b.z);
		});

		const auto cross = [](const point& o, const point& a, const point& b)
		{
			return (a.x - o.x) * (b.z - o.z) - (a.z - o.z) * (b.x - o.x);
		};

		std::vector<point> hull(points.size() * 2);
		size_t k = 0;
		for (const point& p : points)
		{
			while (k >= 2 && cross(hull[k - 2], hull[k - 1], p) <= 0.0f) { --k; }
			hull[k++] = p;
		}
		for (size_t i = points.size() - 1, lower = k + 1; i-- > 0;)
		{
			while (k >= lower && cross(hull[k - 2], hull[k - 1], points[i]) <= 0.0f) { --k; }
			hull[k++] = points[i];
		}
		hull.resize(k > 1 ? k - 1 : k);
		m_hull = std::move(hull);
	}

	float level_footprint::farthest_from(const float position[3])
	{
		if (!m_pending.empty()) {
			rebuild_hull();
		}

		const float dy = std::max(std::abs(position[1] - m_min_y), std::abs(position[1] - m_max_y));
		float farthest = 0.0f;
		for (const point& p : m_hull)
		{
			const float dx = p.x - position[0];
			const float dz = p.z - position[2];
			farthest = std::max(farthest, dx * dx + dz * dz);
		}
		return std::sqrt(farthest + dy * dy);
	}
}
