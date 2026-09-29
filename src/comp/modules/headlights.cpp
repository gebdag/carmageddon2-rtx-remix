#include "std_include.hpp"
#include "headlights.hpp"
#include "lens_lights.hpp"

#include "shared/common/config.hpp"
#include "shared/common/remix_api.hpp"

namespace comp
{
	namespace
	{
		constexpr const char* INI_NAME = "carma2-headlights.ini";
		constexpr const char* INI_SECTION = "Headlights";

		// A car's drawn box only changes when it is crushed or sheds a part.
		constexpr uint32_t BOUNDS_LIFETIME_FRAMES = 120;

		constexpr uint32_t MAX_TREE_ACTORS = 256;
		constexpr uint32_t MAX_TREE_DEPTH = 8;

		constexpr int NOTICE_MS = 1500;

		constexpr float DEG_TO_RAD = 3.14159265f / 180.0f;
		constexpr float PI = 3.14159265f;

		// A crushed lamp turns its beam by at most this much; a lens folded further than that
		// no longer says where a lamp would point.
		constexpr float MAX_LAMP_TURN = 60.0f * DEG_TO_RAD;

		// A rotation needs three points off one line; a lamp lens has more.
		constexpr size_t MIN_LAMP_POINTS = 3;

		enum br_transform_type : uint16_t
		{
			BR_TRANSFORM_MATRIX34 = 0,
			BR_TRANSFORM_MATRIX34_LP = 1,
			BR_TRANSFORM_IDENTITY = 6,
		};

		const char* mode_name(const headlights::mode m)
		{
			switch (m)
			{
			case headlights::mode::player: return "player car";
			case headlights::mode::all_cars: return "all cars";
			default: return "off";
			}
		}

		constexpr game::br_matrix34 IDENTITY34 = { {
			{ 1.0f, 0.0f, 0.0f },
			{ 0.0f, 1.0f, 0.0f },
			{ 0.0f, 0.0f, 1.0f },
			{ 0.0f, 0.0f, 0.0f },
		} };

		// BRender vectors are rows: p' = p * M, with the translation in row 3.
		void transform_point(const game::br_matrix34& m, const float p[3], float out[3])
		{
			for (int i = 0; i < 3; ++i) {
				out[i] = p[0] * m.m[0][i] + p[1] * m.m[1][i] + p[2] * m.m[2][i] + m.m[3][i];
			}
		}

		void transform_direction(const game::br_matrix34& m, const float d[3], float out[3])
		{
			for (int i = 0; i < 3; ++i) {
				out[i] = d[0] * m.m[0][i] + d[1] * m.m[1][i] + d[2] * m.m[2][i];
			}
		}

		// a then b
		game::br_matrix34 concatenate(const game::br_matrix34& a, const game::br_matrix34& b)
		{
			game::br_matrix34 out{};
			for (int row = 0; row < 4; ++row)
			{
				for (int col = 0; col < 3; ++col)
				{
					out.m[row][col] = a.m[row][0] * b.m[0][col] + a.m[row][1] * b.m[1][col]
						+ a.m[row][2] * b.m[2][col] + (row == 3 ? b.m[3][col] : 0.0f);
				}
			}
			return out;
		}

		/*
		 * An actor's transform as a matrix. Every br_transform variant keeps its translation
		 * where a matrix keeps row 3, so the ones that are not matrices still place the actor;
		 * their rotation is dropped, which only loosens a bounding box.
		 */
		game::br_matrix34 local_matrix(const game::br_actor* actor)
		{
			if (actor->t_type <= BR_TRANSFORM_MATRIX34_LP) {
				return actor->t;
			}

			game::br_matrix34 out = IDENTITY34;
			if (actor->t_type != BR_TRANSFORM_IDENTITY) {
				std::memcpy(out.m[3], actor->t.m[3], sizeof(out.m[3]));
			}
			return out;
		}

		struct box
		{
			float min[3] = { FLT_MAX, FLT_MAX, FLT_MAX };
			float max[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };

			bool empty() const { return min[0] > max[0]; }

			void add(const float p[3])
			{
				for (int i = 0; i < 3; ++i)
				{
					min[i] = std::min(min[i], p[i]);
					max[i] = std::max(max[i], p[i]);
				}
			}
		};

		void add_model_bounds(box& out, const game::br_model* model, const game::br_matrix34& to_car)
		{
			for (int corner = 0; corner < 8; ++corner)
			{
				const float p[3] = {
					(corner & 1) ? model->bounds_max.v[0] : model->bounds_min.v[0],
					(corner & 2) ? model->bounds_max.v[1] : model->bounds_min.v[1],
					(corner & 4) ? model->bounds_max.v[2] : model->bounds_min.v[2],
				};

				float in_car[3];
				transform_point(to_car, p, in_car);
				out.add(in_car);
			}
		}

		// Everything drawn by `actor`, its siblings and their subtrees, in the car's own space.
		void add_tree_bounds(box& out, const game::br_actor* actor, const game::br_matrix34& parent_to_car,
			const uint32_t depth, uint32_t& budget)
		{
			for (; actor && budget; actor = actor->next)
			{
				if (!game::can_read(actor, sizeof(*actor))) {
					return;
				}
				--budget;

				if (actor->render_style == game::BR_RSTYLE_NONE) {
					continue;
				}

				const game::br_matrix34 to_car = concatenate(local_matrix(actor), parent_to_car);

				if (actor->type == 1 && game::can_read(actor->model, sizeof(game::br_model))) {
					add_model_bounds(out, actor->model, to_car);
				}

				if (actor->children && depth < MAX_TREE_DEPTH) {
					add_tree_bounds(out, actor->children, to_car, depth + 1, budget);
				}
			}
		}

		float length(const float v[3])
		{
			return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
		}

		using points = std::vector<std::array<float, 3>>;

		std::array<double, 3> centroid(const points& p)
		{
			std::array<double, 3> c{};
			for (const auto& v : p)
			{
				for (int i = 0; i < 3; ++i) {
					c[i] += v[i];
				}
			}
			for (double& x : c) {
				x /= static_cast<double>(p.size());
			}
			return c;
		}

		// Eigenvalues (diagonal of `a` after) and eigenvectors (columns of `v`) of a symmetric 4x4.
		void jacobi(double a[4][4], double v[4][4])
		{
			for (int i = 0; i < 4; ++i)
			{
				for (int j = 0; j < 4; ++j) {
					v[i][j] = i == j ? 1.0 : 0.0;
				}
			}
			for (int sweep = 0; sweep < 32; ++sweep)
			{
				double off = 0.0;
				for (int p = 0; p < 4; ++p)
				{
					for (int q = p + 1; q < 4; ++q) {
						off += std::abs(a[p][q]);
					}
				}
				if (off < 1e-14) {
					return;
				}
				for (int p = 0; p < 4; ++p)
				{
					for (int q = p + 1; q < 4; ++q)
					{
						if (std::abs(a[p][q]) < 1e-18) {
							continue;
						}
						const double theta = (a[q][q] - a[p][p]) / (2.0 * a[p][q]);
						const double t = (theta >= 0.0 ? 1.0 : -1.0) / (std::abs(theta) + std::sqrt(theta * theta + 1.0));
						const double c = 1.0 / std::sqrt(t * t + 1.0);
						const double s = t * c;
						for (int k = 0; k < 4; ++k)
						{
							const double kp = a[k][p], kq = a[k][q];
							a[k][p] = c * kp - s * kq;
							a[k][q] = s * kp + c * kq;
						}
						for (int k = 0; k < 4; ++k)
						{
							const double pk = a[p][k], qk = a[q][k];
							a[p][k] = c * pk - s * qk;
							a[q][k] = s * pk + c * qk;
						}
						for (int k = 0; k < 4; ++k)
						{
							const double kp = v[k][p], kq = v[k][q];
							v[k][p] = c * kp - s * kq;
							v[k][q] = s * kp + c * kq;
						}
					}
				}
			}
		}

		// The rotation that best carries `from` onto `to` about their centres (Horn's
		// quaternion method), as w, x, y, z with w >= 0.
		std::array<double, 4> best_rotation(const points& from, const std::array<double, 3>& from_centre,
			const points& to, const std::array<double, 3>& to_centre)
		{
			double m[3][3] = {};
			for (size_t k = 0; k < from.size(); ++k)
			{
				for (int i = 0; i < 3; ++i)
				{
					for (int j = 0; j < 3; ++j) {
						m[i][j] += (from[k][i] - from_centre[i]) * (to[k][j] - to_centre[j]);
					}
				}
			}

			double n[4][4] = {
				{ m[0][0] + m[1][1] + m[2][2], m[1][2] - m[2][1], m[2][0] - m[0][2], m[0][1] - m[1][0] },
				{ m[1][2] - m[2][1], m[0][0] - m[1][1] - m[2][2], m[0][1] + m[1][0], m[2][0] + m[0][2] },
				{ m[2][0] - m[0][2], m[0][1] + m[1][0], -m[0][0] + m[1][1] - m[2][2], m[1][2] + m[2][1] },
				{ m[0][1] - m[1][0], m[2][0] + m[0][2], m[1][2] + m[2][1], -m[0][0] - m[1][1] + m[2][2] },
			};
			double v[4][4];
			jacobi(n, v);

			int best = 0;
			for (int i = 1; i < 4; ++i)
			{
				if (n[i][i] > n[best][best]) {
					best = i;
				}
			}
			std::array<double, 4> q = { v[0][best], v[1][best], v[2][best], v[3][best] };
			const double length = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
			const double sign = q[0] < 0.0 ? -1.0 : 1.0;
			for (double& x : q) {
				x *= sign / length;
			}
			return q;
		}
	}

	std::string headlights::ini_path()
	{
		return shared::globals::root_path + "\\" + INI_NAME;
	}

	uint64_t headlights::lamp_key(const void* car_spec, const int side)
	{
		return (static_cast<uint64_t>(reinterpret_cast<uintptr_t>(car_spec)) << 1) | static_cast<uint64_t>(side);
	}

	// ------
	// settings

	void headlights::load()
	{
		const std::string path = ini_path();
		settings s{};

		const auto read_float = [&path](const char* key, float& value, const float lo, const float hi)
		{
			char buf[64];
			if (GetPrivateProfileStringA(INI_SECTION, key, "", buf, sizeof(buf), path.c_str()))
			{
				char* end = nullptr;
				const float parsed = std::strtof(buf, &end);
				if (end != buf) {
					value = std::clamp(parsed, lo, hi);
				}
			}
		};

		const auto read_bool = [&path](const char* key, const bool fallback) {
			return GetPrivateProfileIntA(INI_SECTION, key, fallback ? 1 : 0, path.c_str()) != 0;
		};

		const int start = GetPrivateProfileIntA(INI_SECTION, "StartMode", static_cast<int>(s.start_mode), path.c_str());
		s.start_mode = static_cast<mode>(std::clamp(start, 0, 2));

		read_float("Spacing", s.spacing, 0.0f, 1.5f);
		read_float("Height", s.height, 0.0f, 1.5f);
		read_float("Forward", s.forward, -0.5f, 1.0f);

		read_float("PitchDown", s.pitch_down, -30.0f, 45.0f);
		read_float("ToeOut", s.toe_out, -30.0f, 30.0f);
		read_float("ConeAngle", s.cone_angle, 1.0f, 90.0f);
		read_float("ConeSoftness", s.cone_softness, 0.0f, 1.0f);
		read_float("Focus", s.focus, 0.0f, 50.0f);
		// Kept with the proxy's settings, so a release can ship its own default.
		s.follow_damage = shared::common::config::get().get_bool("Lights", "HeadlightsFollowDamage", s.follow_damage);

		read_float("ColourR", s.colour[0], 0.0f, 1.0f);
		read_float("ColourG", s.colour[1], 0.0f, 1.0f);
		read_float("ColourB", s.colour[2], 0.0f, 1.0f);
		read_float("Brightness", s.brightness, 0.0f, 100000.0f);
		read_float("EmitterRadius", s.emitter_radius, 0.001f, 0.2f);
		read_float("VolumetricScale", s.volumetric_scale, 0.0f, 10.0f);

		read_float("OtherCarsBrightness", s.other_brightness, 0.0f, 4.0f);
		read_float("OtherCarsRange", s.other_range, 0.0f, 1000.0f);
		s.wasted_stay_lit = read_bool("WastedStayLit", s.wasted_stay_lit);
		read_float("CivilianCarsBrightness", s.civilian_brightness, 0.0f, 4.0f);
		read_float("CivilianCarsRange", s.civilian_range, 0.0f, 1000.0f);

		m_settings = s;
		m_saved = s;
	}

	void headlights::save()
	{
		const std::string path = ini_path();
		const settings& s = m_settings;

		const auto write = [&path](const char* key, const std::string& value) {
			return WritePrivateProfileStringA(INI_SECTION, key, value.c_str(), path.c_str()) != FALSE;
		};
		const auto number = [](const float value) { return std::format("{:.4f}", value); };

		bool ok = write("StartMode", std::to_string(static_cast<int>(s.start_mode)));
		ok &= write("Spacing", number(s.spacing));
		ok &= write("Height", number(s.height));
		ok &= write("Forward", number(s.forward));
		ok &= write("PitchDown", number(s.pitch_down));
		ok &= write("ToeOut", number(s.toe_out));
		ok &= write("ConeAngle", number(s.cone_angle));
		ok &= write("ConeSoftness", number(s.cone_softness));
		ok &= write("Focus", number(s.focus));
		ok &= write("ColourR", number(s.colour[0]));
		ok &= write("ColourG", number(s.colour[1]));
		ok &= write("ColourB", number(s.colour[2]));
		ok &= write("Brightness", number(s.brightness));
		ok &= write("EmitterRadius", number(s.emitter_radius));
		ok &= write("VolumetricScale", number(s.volumetric_scale));
		ok &= write("OtherCarsBrightness", number(s.other_brightness));
		ok &= write("OtherCarsRange", number(s.other_range));
		ok &= write("WastedStayLit", s.wasted_stay_lit ? "1" : "0");
		ok &= write("CivilianCarsBrightness", number(s.civilian_brightness));
		ok &= write("CivilianCarsRange", number(s.civilian_range));

		shared::common::config::get().set_bool("Lights", "HeadlightsFollowDamage", s.follow_damage);

		if (ok)
		{
			m_saved = s;
			shared::common::log("Headlights", std::format("saved {}", path));
		}
		else
		{
			shared::common::log("Headlights", std::format("could not write {}", path),
				shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
		}
	}

	// ------
	// mode

	void headlights::set_night(const bool night)
	{
		if (night == m_night) {
			return;
		}
		m_night = night;

		if (night && m_mode != mode::all_cars)
		{
			m_mode_before_night = m_mode;
			m_mode = mode::all_cars;
			m_night_switched = true;
			shared::common::log("Headlights", "night race: headlights on for all cars");
		}
		else if (!night && m_night_switched)
		{
			m_mode = m_mode_before_night;
			m_night_switched = false;
		}
	}

	void headlights::cycle_mode()
	{
		m_night_switched = false;
		m_mode = static_cast<mode>((static_cast<int>(m_mode) + 1) % 3);
		m_notice_pending = true;
		shared::common::log("Headlights", std::format("headlights: {}", mode_name(m_mode)));
	}

	// ------
	// remix

	bool headlights::remix_lights_available()
	{
		using shared::common::remix_api;

		if (!remix_api::is_initialized()) {
			return false;
		}

		const auto& bridge = remix_api::get().m_bridge;
		return bridge.CreateLight && bridge.DestroyLight && bridge.DrawLightInstance;
	}

	void headlights::destroy_all()
	{
		if (m_lamps.empty()) {
			return;
		}

		if (shared::common::remix_api::is_initialized())
		{
			const auto& bridge = shared::common::remix_api::get().m_bridge;
			for (auto& [key, l] : m_lamps)
			{
				if (l.handle) {
					bridge.DestroyLight(l.handle);
				}
			}
		}

		m_lamps.clear();
		m_lit_masters.clear();
		m_lit_cars = 0;
	}

	bool headlights::lights_actor(const game::br_actor* actor) const
	{
		// A car is a handful of levels deep: master, the loaded .ACT, its parts.
		constexpr int MAX_DEPTH = 16;
		for (int depth = 0; actor && depth < MAX_DEPTH; ++depth, actor = actor->parent)
		{
			if (m_lit_masters.contains(actor)) {
				return true;
			}
		}
		return false;
	}

	bool headlights::lamp_mount(const game::race_car& car, const int side, const bool rear,
		float position[3], float facing[3])
	{
		if (car.master->t_type > BR_TRANSFORM_MATRIX34_LP) {
			return false;
		}
		const car_bounds* bounds = measure(car);
		if (!bounds) {
			return false;
		}

		const settings& s = m_settings;
		const float sign = side == 0 ? -1.0f : 1.0f;
		const float centre_x = 0.5f * (bounds->min[0] + bounds->max[0]);
		const float half_width = 0.5f * (bounds->max[0] - bounds->min[0]);

		// A car faces down its local -Z: the front face of the box is min z, the back max z.
		const float local_pos[3] = {
			centre_x + sign * s.spacing * half_width,
			bounds->min[1] + s.height * (bounds->max[1] - bounds->min[1]),
			rear ? bounds->max[2] + s.forward : bounds->min[2] - s.forward,
		};
		const float local_dir[3] = { 0.0f, 0.0f, rear ? 1.0f : -1.0f };

		transform_point(car.master->t, local_pos, position);
		transform_direction(car.master->t, local_dir, facing);
		return true;
	}

	const headlights::car_bounds* headlights::measure(const game::race_car& car)
	{
		auto& cached = m_bounds[car.spec];
		if (cached.model == car.model && cached.measured_frame
			&& m_frame - cached.measured_frame < BOUNDS_LIFETIME_FRAMES)
		{
			return &cached;
		}

		box measured;
		uint32_t budget = MAX_TREE_ACTORS;
		add_tree_bounds(measured, car.master->children, IDENTITY34, 0, budget);

		if (measured.empty())
		{
			m_bounds.erase(car.spec);
			return nullptr;
		}

		std::memcpy(cached.min, measured.min, sizeof(cached.min));
		std::memcpy(cached.max, measured.max, sizeof(cached.max));
		cached.model = car.model;
		cached.measured_frame = m_frame;
		return &cached;
	}

	/*
	 * Remix has no call that moves a light. It derives the handle from the hash in the info
	 * and overwrites a matching entry, so describing the lamp again under the same hash is
	 * the update -- destroying it first would blink it out for a frame.
	 */
	void headlights::remember_lamp_origins()
	{
		const auto lens = lens_lights::get();
		if (!lens) {
			return;
		}
		for (const auto& car : m_cars)
		{
			if (car.master->t_type > BR_TRANSFORM_MATRIX34_LP) {
				continue;
			}
			const car_bounds* bounds = nullptr;
			for (int side = 0; side < 2; ++side)
			{
				auto lamps = lens->head_lamps(car.master, side);
				const auto key = std::make_tuple(static_cast<const void*>(car.spec), side, static_cast<const void*>(lamps.model));
				if (!lamps.model || lamps.points.size() < MIN_LAMP_POINTS || m_lamp_origins.contains(key)) {
					continue;
				}
				if (!bounds) {
					bounds = measure(car);
				}
				if (!bounds) {
					break;
				}
				lamp_origin& origin = m_lamp_origins[key];
				origin.points = std::move(lamps.points);
				origin.bounds = *bounds;
				origin.last = { &origin.bounds, {}, { { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f } }, 0.0f };
			}
		}
	}

	std::optional<headlights::lamp_damage> headlights::lamp_damage_on(const game::race_car& car, const int side,
		const game::br_model* model, const std::vector<std::array<float, 3>>& now)
	{
		if (!m_settings.follow_damage || !model) {
			return std::nullopt;
		}
		const auto it = m_lamp_origins.find({ car.spec, side, model });
		if (it == m_lamp_origins.end()) {
			return std::nullopt;
		}
		lamp_origin& origin = it->second;
		if (now.size() != origin.points.size()) {
			return origin.last;
		}

		const auto centre_then = centroid(origin.points);
		const auto centre_now = centroid(now);
		auto q = best_rotation(origin.points, centre_then, now, centre_now);

		lamp_damage damage{ &origin.bounds };
		for (int i = 0; i < 3; ++i) {
			damage.moved[i] = static_cast<float>(centre_now[i] - centre_then[i]);
		}

		const double half = std::acos(std::clamp(q[0], -1.0, 1.0));
		damage.angle = static_cast<float>(2.0 * half);
		if (damage.angle > MAX_LAMP_TURN)
		{
			const double scale = std::sin(MAX_LAMP_TURN * 0.5) / std::sin(half);
			q = { std::cos(MAX_LAMP_TURN * 0.5), q[1] * scale, q[2] * scale, q[3] * scale };
		}

		const double w = q[0], x = q[1], y = q[2], z = q[3];
		const double r[3][3] = {
			{ 1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y) },
			{ 2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x) },
			{ 2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y) },
		};
		for (int i = 0; i < 3; ++i)
		{
			for (int j = 0; j < 3; ++j) {
				damage.rotation[i][j] = static_cast<float>(r[i][j]);
			}
		}
		origin.last = damage;
		return damage;
	}

	void headlights::describe_lamp(const game::race_car& car, const car_bounds& current_bounds, const int side,
		const float brightness, const lamp_damage* damage)
	{
		const settings& s = m_settings;
		const float sign = side == 0 ? -1.0f : 1.0f;

		// A crushed car's box shrinks with it. A beam following its lamp is placed against the
		// box from before, so the crush moves it once, through the lamp.
		const car_bounds& bounds = damage ? *damage->bounds : current_bounds;

		// A car faces down its local -Z, so the front face of the box is min z.
		const float centre_x = 0.5f * (bounds.min[0] + bounds.max[0]);
		const float half_width = 0.5f * (bounds.max[0] - bounds.min[0]);
		float local_pos[3] = {
			centre_x + sign * s.spacing * half_width,
			bounds.min[1] + s.height * (bounds.max[1] - bounds.min[1]),
			bounds.min[2] - s.forward,
		};

		const float pitch = s.pitch_down * DEG_TO_RAD;
		const float yaw = sign * s.toe_out * DEG_TO_RAD;
		float local_dir[3] = {
			std::sin(yaw) * std::cos(pitch),
			-std::sin(pitch),
			-std::cos(yaw) * std::cos(pitch),
		};

		if (damage)
		{
			for (int i = 0; i < 3; ++i) {
				local_pos[i] += damage->moved[i];
			}
			const float dir[3] = { local_dir[0], local_dir[1], local_dir[2] };
			for (int i = 0; i < 3; ++i) {
				local_dir[i] = damage->rotation[i][0] * dir[0] + damage->rotation[i][1] * dir[1] + damage->rotation[i][2] * dir[2];
			}
		}

		const game::br_matrix34& car_to_world = car.master->t;

		float pos[3];
		float dir[3];
		transform_point(car_to_world, local_pos, pos);
		transform_direction(car_to_world, local_dir, dir);

		const float dir_length = length(dir);
		if (!(dir_length > 1e-6f)) {
			return;
		}

		const float radiance = brightness / (PI * s.emitter_radius * s.emitter_radius);

		remixapi_LightInfoSphereEXT sphere{};
		sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
		sphere.position = { pos[0], pos[1], pos[2] };
		sphere.radius = s.emitter_radius;
		sphere.shaping_hasvalue = TRUE;
		sphere.shaping_value.direction = { dir[0] / dir_length, dir[1] / dir_length, dir[2] / dir_length };
		sphere.shaping_value.coneAngleDegrees = s.cone_angle;
		sphere.shaping_value.coneSoftness = s.cone_softness;
		sphere.shaping_value.focusExponent = s.focus;
		sphere.volumetricRadianceScale = s.volumetric_scale;

		const uint64_t key = lamp_key(car.spec, side);
		lamp& l = m_lamps[key];

		// Remix Plus applies a destroy one scene frame late, so a lamp that went out and comes
		// back takes a new incarnation rather than its old hash.
		remixapi_LightInfo info{};
		info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
		info.pNext = &sphere;
		info.hash = shared::utils::string_hash64(std::format("carma2-headlight-{:x}-{}", key, l.incarnation));
		info.radiance = { s.colour[0] * radiance, s.colour[1] * radiance, s.colour[2] * radiance };

		// Remix puts a light it takes for static to sleep; a lamp on a parked car would be one.
		info.isDynamic = TRUE;

		const auto& bridge = shared::common::remix_api::get().m_bridge;
		remixapi_LightHandle handle = l.handle;
		if (bridge.CreateLight(&info, &handle) != REMIXAPI_ERROR_CODE_SUCCESS)
		{
			if (!m_create_failed)
			{
				m_create_failed = true;
				shared::common::log("Headlights", "Remix refused a headlight (CreateLight failed)",
					shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
			}
			return;
		}

		l.handle = handle;
		l.drawn_this_frame = true;
		bridge.DrawLightInstance(handle);
	}

	void headlights::on_race_frame(const float camera_pos[3])
	{
		++m_frame;
		m_lit_masters.clear();

		game::collect_race_cars(m_cars);
		remember_lamp_origins();

		if (m_mode == mode::off)
		{
			destroy_all();
			return;
		}

		if (!remix_lights_available()) {
			return;
		}

		for (auto& [key, l] : m_lamps) {
			l.drawn_this_frame = false;
		}

		m_lit_cars = 0;
		m_player_status = m_cars.empty() ? "no cars: the game is not in a race" : "not found";
		for (const auto& car : m_cars)
		{
			if (!car.is_player && m_mode != mode::all_cars) {
				continue;
			}

			// Why a car is dark is the first thing to know when its lights go missing, and
			// the player's car is the one being watched.
			const auto skip = [this, &car](const char* reason)
			{
				if (car.is_player) {
					m_player_status = reason;
				}
			};

			if (car.knackered && !m_settings.wasted_stay_lit)
			{
				skip("dark: the game has it flagged as wasted");
				continue;
			}
			if (car.master->render_style == game::BR_RSTYLE_NONE)
			{
				skip("dark: the game is not drawing the car");
				continue;
			}
			if (car.master->t_type > BR_TRANSFORM_MATRIX34_LP)
			{
				skip("dark: the car's actor carries no matrix");
				continue;
			}

			// Its lights are on: the lens shows lit at any distance, even where the Remix
			// light is left out for range.
			m_lit_masters.insert(car.master);

			float brightness = m_settings.brightness;
			if (!car.is_player)
			{
				const float to_car[3] = {
					car.master->t.m[3][0] - camera_pos[0],
					car.master->t.m[3][1] - camera_pos[1],
					car.master->t.m[3][2] - camera_pos[2],
				};
				if (length(to_car) > (car.civilian ? m_settings.civilian_range : m_settings.other_range)) {
					continue;
				}
				brightness *= car.civilian ? m_settings.civilian_brightness : m_settings.other_brightness;
			}

			if (!(brightness > 0.0f)) {
				continue;
			}

			const car_bounds* bounds = measure(car);
			if (!bounds)
			{
				skip("dark: nothing under the car's actor could be measured");
				continue;
			}

			if (car.is_player)
			{
				m_player_status = std::format("lit - car {:.2f} x {:.2f} x {:.2f} units",
					bounds->max[0] - bounds->min[0], bounds->max[1] - bounds->min[1],
					bounds->max[2] - bounds->min[2]);
			}

			// A smashed lamp casts no beam. The capture has reported this scene's lamps by now.
			const auto lens = lens_lights::get();
			for (int side = 0; side < 2; ++side)
			{
				const auto lamps = lens ? lens->head_lamps(car.master, side) : lens_lights::head_lamp_side{};
				const auto damage = lamp_damage_on(car, side, lamps.model, lamps.points);
				if (car.is_player)
				{
					m_player_status += std::format(", {} lamps {}/{} intact", side == 0 ? "left" : "right", lamps.intact, lamps.total);
					if (damage)
					{
						m_player_status += std::format(" (moved {:.3f}, turned {:.0f} deg)", length(damage->moved),
							damage->angle / DEG_TO_RAD);
					}
				}
				if (lamps.total == 0 || lamps.intact > 0) {
					describe_lamp(car, *bounds, side, brightness, damage ? &*damage : nullptr);
				}
			}
			++m_lit_cars;
		}

		// Lamps stay in the map while the race lasts so their incarnation survives going out.
		const auto& bridge = shared::common::remix_api::get().m_bridge;
		for (auto& [key, l] : m_lamps)
		{
			if (!l.drawn_this_frame && l.handle)
			{
				bridge.DestroyLight(l.handle);
				l.handle = nullptr;
				++l.incarnation;
			}
		}
	}

	void headlights::on_frame_without_race()
	{
		// Car specs and actors are recycled by the next race, so nothing measured in this
		// one may be carried into it.
		destroy_all();
		m_bounds.clear();
		m_lamp_origins.clear();
	}

	// ------
	// menu

	void headlights::post_mode_notice()
	{
		if (!m_notice_pending) {
			return;
		}
		m_notice_pending = false;
		game::show_headup_message(std::format("Headlights: {}", mode_name(m_mode)).c_str(), NOTICE_MS);
	}

	void headlights::draw_menu()
	{
		settings& s = m_settings;

		ImGui::TextUnformatted("F cycles: off -> player car -> all cars -> off");

		if (!shared::common::remix_api::is_initialized())
		{
			if (shared::common::remix_api::gave_up()) {
				ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f),
					"Remix API not available - .trex/bridge.conf needs 'exposeRemixApi = True'");
			}
			else {
				ImGui::TextDisabled("The Remix API starts with the first race frame.");
			}
		}
		else
		{
			ImGui::TextDisabled("%s: %u car(s) lit, %u Remix light(s)", shared::common::remix_api::runtime(),
				m_lit_cars, static_cast<uint32_t>(std::ranges::count_if(m_lamps, [](const auto& e) { return e.second.handle != nullptr; })));
			if (m_mode != mode::off) {
				ImGui::TextDisabled("Player car: %s", m_player_status.c_str());
			}
		}

		ImGui::Spacing();

		int current = static_cast<int>(m_mode);
		ImGui::RadioButton("Off", &current, 0); ImGui::SameLine();
		ImGui::RadioButton("Player car", &current, 1); ImGui::SameLine();
		ImGui::RadioButton("All cars", &current, 2);
		if (current != static_cast<int>(m_mode)) {
			m_night_switched = false;
		}
		m_mode = static_cast<mode>(current);

		int start = static_cast<int>(s.start_mode);
		if (ImGui::Combo("Mode at game start", &start, "Off\0Player car\0All cars\0")) {
			s.start_mode = static_cast<mode>(start);
		}

		if (ImGui::CollapsingHeader("Mounting", ImGuiTreeNodeFlags_DefaultOpen))
		{
			ImGui::TextDisabled("Relative to each car's own size, so one setting fits an Eagle and a dump truck.");
			ImGui::SliderFloat("Spacing", &s.spacing, 0.0f, 1.2f, "%.2f of half width");
			ImGui::SliderFloat("Height", &s.height, 0.0f, 1.2f, "%.2f of body height");
			ImGui::SliderFloat("Ahead of nose", &s.forward, -0.3f, 0.5f, "%.3f units");
		}

		if (ImGui::CollapsingHeader("Beam", ImGuiTreeNodeFlags_DefaultOpen))
		{
			ImGui::SliderFloat("Pitch down", &s.pitch_down, -15.0f, 30.0f, "%.1f deg");
			ImGui::SliderFloat("Toe out", &s.toe_out, -15.0f, 15.0f, "%.1f deg");
			ImGui::SliderFloat("Cone angle", &s.cone_angle, 5.0f, 90.0f, "%.1f deg");
			ImGui::SliderFloat("Cone softness", &s.cone_softness, 0.0f, 1.0f, "%.2f");
			ImGui::SliderFloat("Focus", &s.focus, 0.0f, 20.0f, "%.1f");
			ImGui::Checkbox("Follow lamp damage", &s.follow_damage);
			ImGui::TextDisabled("Cars with their own headlight geometry: a crushed lamp moves and turns its beam.");
			ImGui::TextDisabled("Saved to remix-comp-proxy.ini, [Lights] HeadlightsFollowDamage.");
		}

		if (ImGui::CollapsingHeader("Lamp", ImGuiTreeNodeFlags_DefaultOpen))
		{
			ImGui::ColorEdit3("Colour", s.colour);
			ImGui::SliderFloat("Brightness", &s.brightness, 0.05f, 5000.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
			ImGui::SliderFloat("Emitter radius", &s.emitter_radius, 0.002f, 0.1f, "%.3f units", ImGuiSliderFlags_Logarithmic);
			ImGui::SliderFloat("Volumetric glow", &s.volumetric_scale, 0.0f, 5.0f, "%.2f");
			ImGui::TextDisabled("A car is about 0.4 units wide. The radius softens shadows; it does not change brightness.");
		}

		if (ImGui::CollapsingHeader("Other cars", ImGuiTreeNodeFlags_DefaultOpen))
		{
			ImGui::SliderFloat("Brightness scale", &s.other_brightness, 0.0f, 2.0f, "%.2f");
			ImGui::SliderFloat("Range from camera", &s.other_range, 1.0f, 100.0f, "%.1f units", ImGuiSliderFlags_Logarithmic);
			ImGui::Checkbox("Wasted cars stay lit", &s.wasted_stay_lit);
		}

		if (ImGui::CollapsingHeader("Civilian cars", ImGuiTreeNodeFlags_DefaultOpen))
		{
			ImGui::TextDisabled("City traffic, lit in 'All cars' mode. The game only draws the few nearest the camera.");
			ImGui::SliderFloat("Civilian brightness scale", &s.civilian_brightness, 0.0f, 2.0f, "%.2f");
			ImGui::SliderFloat("Civilian range from camera", &s.civilian_range, 1.0f, 100.0f, "%.1f units", ImGuiSliderFlags_Logarithmic);
		}

		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();

		const bool dirty = !(m_settings == m_saved);
		if (ImGui::Button("Save to ini")) {
			save();
		}
		ImGui::SameLine();
		if (ImGui::Button("Reload from ini")) {
			load();
		}
		ImGui::SameLine();
		if (ImGui::Button("Defaults")) {
			m_settings = settings{};
		}
		ImGui::SameLine();
		ImGui::TextDisabled(dirty ? "unsaved changes" : INI_NAME);
	}

	// ------

	headlights::headlights()
	{
		p_this = this;

		load();
		m_mode = m_settings.start_mode;

		shared::common::log("Headlights", std::format("Module initialized, start mode: {}.", mode_name(m_mode)),
			shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
	}
}
