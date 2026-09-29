#pragma once

namespace comp
{
	/*
	 * The level's footprint seen from above, for putting the fog at its edge.
	 *
	 * Every actor that bakes into the static world marks the ground cells its world-space
	 * bounds cover. The cells with an empty neighbour are the level's edge -- concave bays
	 * and notches included, which a convex outline would paper over. The fog asks how far
	 * the edge reaches inside the camera's view: looking across the level that is its far
	 * side, looking at a nearby boundary it is that boundary.
	 *
	 * The city streams its scenery in as the car drives, and baked chunks keep drawing
	 * after the game drops their actors, so the footprint only grows -- as the drawn world
	 * does. It is cleared with the static world.
	 */
	class level_footprint
	{
	public:
		void add_bounds(const game::br_vector3& min, const game::br_vector3& max,
			const game::br_matrix34& model_to_world);
		void clear();

		bool empty() const { return m_cells.empty(); }

		/*
		 * Distance from `position` to the farthest edge cell within `half_angle` radians of
		 * `forward` on the ground plane, height range included. 0 when no edge lies in view.
		 *
		 * Args:
		 *   position: the camera, world space.
		 *   forward: the camera's view direction, world space; only its ground-plane part counts.
		 *   half_angle: half the view's horizontal angle, margin included.
		 */
		float farthest_in_view(const float position[3], const float forward[3], float half_angle);

	private:
		static int64_t key(int32_t x, int32_t z) { return (static_cast<int64_t>(x) << 32) | static_cast<uint32_t>(z); }
		void rebuild_edge();

		std::unordered_set<int64_t> m_cells;
		std::vector<std::pair<float, float>> m_edge;   // centres of the edge cells, x and z
		bool m_edge_dirty = false;
		uint32_t m_queries_since_rebuild = 0;
		float m_min_y = std::numeric_limits<float>::infinity();
		float m_max_y = -std::numeric_limits<float>::infinity();
	};
}
