#pragma once

namespace comp
{
	/*
	 * The level's outline seen from above, for putting the fog at its far side.
	 *
	 * Every actor that bakes into the static world adds the corners of its world-space
	 * bounds. The outline is their convex hull on the ground plane plus the level's height
	 * range, and the point of the level farthest from any position is always one of the
	 * hull's corners, at the top or the bottom of that range.
	 *
	 * The city streams its scenery in as the car drives, and baked chunks keep drawing
	 * after the game drops their actors, so the outline only grows -- as the drawn world
	 * does. It is cleared with the static world.
	 */
	class level_footprint
	{
	public:
		void add_bounds(const game::br_vector3& min, const game::br_vector3& max,
			const game::br_matrix34& model_to_world);
		void clear();

		bool empty() const { return m_hull.empty() && m_pending.empty(); }

		// Distance from `position` (world space) to the level's farthest point.
		float farthest_from(const float position[3]);

	private:
		struct point
		{
			float x, z;
		};

		void rebuild_hull();

		std::vector<point> m_hull;      // counter-clockwise, no repeated first point
		std::vector<point> m_pending;   // corners added since the hull was last built
		float m_min_y = std::numeric_limits<float>::infinity();
		float m_max_y = -std::numeric_limits<float>::infinity();
	};
}
