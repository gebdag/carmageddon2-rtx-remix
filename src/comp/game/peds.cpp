#include "std_include.hpp"
#include "game.hpp"
#include "shared/common/config.hpp"

namespace comp::game
{
	namespace
	{
		/*
		 * Every active pedestrian registers its root bone (the Spine: chest and pelvis) as an
		 * asleep, non-simulated physics body. On a frame with no 40 ms physics step, DoPhysics
		 * (0x004B6630) calls RestoreSteppedPoses (0x004C2600) to put every body's actor back
		 * to its pose at the last step -- which undoes the rewind simulated bodies get, but for
		 * a walking ped undoes nothing and overwrites the root the animation just moved
		 * (findings 76). Above 25 fps most frames have no step: the torso is drawn keyframes
		 * behind the limbs, and every keyframe that lands in such a frame loses its step, so
		 * peds jitter and walk at a fraction of their speed.
		 *
		 * The call is redirected here: the roots of walking peds are saved, the game restores
		 * every body as before, and the saved roots go back. Anything the physics owns -- a
		 * ragdoll, a flung body -- has a simulated body and keeps the game's restore.
		 */
		constexpr uint32_t ADDR_RestoreSteppedPosesCall = 0x004B66A2u;
		constexpr uint32_t ADDR_RestoreSteppedPoses = 0x004C2600u;

		constexpr uint32_t ADDR_g_peds = 0x00744808u;             // ped record*
		constexpr uint32_t ADDR_g_ped_count = 0x007447D4u;        // int
		constexpr uint32_t PED_RECORD_STRIDE = 0x54u;
		constexpr uint32_t MAX_PEDS = 2000u;
		constexpr uint16_t PED_ACTIVE = 1u;                       // record +0x08

		// Character instance, personality and form fields (findings 59, 76).
		constexpr uint32_t INST_ACTOR_SET = 0x04u;                // s8, -1 without actors
		constexpr uint32_t INST_PHYSICS_SLOT = 0x05u;             // s8, the simple-physics body
		constexpr uint32_t INST_PHYSICS_FLAGS = 0x14u;            // bit 0 simple physics, bit 2 boned
		constexpr uint32_t PERSONALITY_FORM = 0x28u;
		constexpr uint32_t FORM_SIMPLE_BODIES = 0x38u;            // pool: +4 + slot * 8 = body*
		constexpr uint32_t BODY_PARAMS = 0x240u;                  // params*: +8 == 1 not simulated
		constexpr int PARAMS_NOT_SIMULATED = 1;

		using restore_t = void(__fastcall*)(void* bodies);

		void restore(void* bodies) {
			reinterpret_cast<restore_t>(rebase(ADDR_RestoreSteppedPoses))(bodies);
		}

		struct saved_root
		{
			br_matrix34* t;
			br_matrix34 value;
		};
		std::vector<saved_root> g_saved;

		template <typename T>
		T read(const uint8_t* p, const uint32_t offset) {
			return *reinterpret_cast<const T*>(p + offset);
		}

		// The root bone of a walking ped whose physics body only follows it, or null.
		br_matrix34* walking_root(const uint8_t* record)
		{
			if (!(read<uint16_t>(record, 0x08) & PED_ACTIVE)) {
				return nullptr;
			}
			const auto inst = read<const uint8_t*>(record, 0x00);
			if (!can_read(inst, 0x20) || read<int8_t>(inst, INST_ACTOR_SET) < 0
				|| (read<uint32_t>(inst, INST_PHYSICS_FLAGS) & 5u) != 1u)
			{
				return nullptr;
			}

			const auto personality = read<const uint8_t*>(inst, 0x00);
			const auto form = can_read(personality, PERSONALITY_FORM + 4) ? read<const uint8_t*>(personality, PERSONALITY_FORM) : nullptr;
			const auto pool = can_read(form, FORM_SIMPLE_BODIES + 4) ? read<const uint8_t*>(form, FORM_SIMPLE_BODIES) : nullptr;
			const int slot = read<int8_t>(inst, INST_PHYSICS_SLOT);
			if (!pool || slot < 0 || !can_read(pool + 4 + slot * 8, 4)) {
				return nullptr;
			}

			const auto body = read<const uint8_t*>(pool, 4 + slot * 8);
			const auto params = can_read(body, BODY_PARAMS + 4) ? read<const uint8_t*>(body, BODY_PARAMS) : nullptr;
			if (!can_read(params, 12) || read<int>(params, 8) != PARAMS_NOT_SIMULATED) {
				return nullptr;
			}

			const auto root = read<br_actor*>(body, 0x00);
			return can_read(root, sizeof(br_actor)) ? &root->t : nullptr;
		}

		void __fastcall hk_restore_stepped_poses(void* bodies)
		{
			if (!shared::common::config::get().effects.ped_motion_fix)
			{
				restore(bodies);
				return;
			}

			g_saved.clear();
			const int count = *reinterpret_cast<const int*>(rebase(ADDR_g_ped_count));
			const auto records = *reinterpret_cast<const uint8_t* const*>(rebase(ADDR_g_peds));
			if (count > 0 && count <= static_cast<int>(MAX_PEDS)
				&& can_read(records, static_cast<size_t>(count) * PED_RECORD_STRIDE))
			{
				for (int i = 0; i < count; ++i)
				{
					if (br_matrix34* t = walking_root(records + i * PED_RECORD_STRIDE)) {
						g_saved.push_back({ t, *t });
					}
				}
			}

			restore(bodies);

			for (const auto& s : g_saved) {
				*s.t = s.value;
			}
		}
	}

	void install_ped_motion_fix()
	{
		const auto call = reinterpret_cast<uint8_t*>(rebase(ADDR_RestoreSteppedPosesCall));
		const int32_t expected = static_cast<int32_t>(rebase(ADDR_RestoreSteppedPoses) - (rebase(ADDR_RestoreSteppedPosesCall) + 5));
		if (call[0] != 0xE8 || *reinterpret_cast<const int32_t*>(call + 1) != expected)
		{
			shared::common::log("Game", "pedestrian motion fix not installed: the physics restore call is not the expected one",
				shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
			return;
		}

		const int32_t target = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&hk_restore_stepped_poses)
			- (rebase(ADDR_RestoreSteppedPosesCall) + 5));
		DWORD old_protect = 0;
		if (!VirtualProtect(call + 1, 4, PAGE_EXECUTE_READWRITE, &old_protect)) {
			return;
		}
		*reinterpret_cast<int32_t*>(call + 1) = target;
		VirtualProtect(call + 1, 4, old_protect, &old_protect);
		FlushInstructionCache(GetCurrentProcess(), call, 5);

		shared::common::log("Game", "pedestrian motion fix: walking peds keep their animated root between physics steps",
			shared::common::LOG_TYPE::LOG_TYPE_GREEN, true);
	}

	namespace
	{
		/*
		 * Two things make ped limbs step at a higher frame rate, and one RenderAFrame detour
		 * handles both. Nothing the game simulates sees either: every matrix and field touched
		 * is put back once the frame is drawn.
		 *
		 * A walking ped is posed only at its keyframes (30 Hz while walking, 10 to 33 Hz
		 * running) and never between them (findings 76). It is drawn between its current
		 * keyframe and the next, by the fraction of a keyframe its clock has run (inst+0x18):
		 * the next pose comes from the game's own PoseCharacterActors, and every bone blends
		 * between the two.
		 *
		 * A flung ped or a body (physics-driven, findings 76.6) has its torso moved by the
		 * physics every frame, but its limbs follow the torso only on 40 ms physics steps. The
		 * limbs are posed again from this frame's torso the way the step does it
		 * (RederiveFromPhysicsRoot 0x0040B770, without its side effects), and blended between
		 * keyframes too while the flailing animation runs.
		 *
		 * Action replay (findings 76.7) steps through the recorded frames, forward or back, and
		 * the animation clock keeps its meaning both ways, so walking peds blend as in play. It
		 * has the root of a flung ped and each severed piece only as they were on every 40 ms
		 * physics step, and no physics to carry them between: those are drawn between the
		 * last two matrices replay gave them, one step behind.
		 */
		constexpr uint32_t ADDR_RenderAFrame = 0x004E4E40u;          // void __cdecl (void)
		constexpr uint32_t ADDR_PoseCharacterActors = 0x00407B30u;   // __fastcall (inst, mode, move_root), ret 4
		constexpr uint32_t ADDR_BrMatrix34LPInverse = 0x00532EB0u;   // cdecl (dst, src)
		constexpr uint32_t ADDR_BrMatrix34Mul = 0x00532620u;         // cdecl (dst, a, b): dst = a x b, dst aliases neither
		constexpr uint32_t ADDR_g_action_replay_mode = 0x00676914u;
		constexpr uint32_t ADDR_g_corpse_twitch = 0x007447A4u;       // float, a powerup: non-zero jitters corpse bones on every pose
		constexpr uint32_t ADDR_g_rand_state = 0x00673750u;          // u32, the CRT rand() seed
		constexpr uint32_t PED_STATE = 0x10u;                        // record u32: 5 hit, 6 dead
		constexpr uint32_t PED_STATE_DEAD = 6u;
		constexpr uint32_t ADDR_g_replay_time = 0x0079EFB4u;         // u32 ms, the last recorded frame applied
		constexpr uint32_t ADDR_g_replay_rate = 0x00676900u;         // float, < 0 plays back: root motion is subtracted
		constexpr int64_t MAX_REPLAY_STEP_MS = 250;                  // more between frames is a jump

		constexpr uint32_t INST_MOVE = 0x07u;                        // s8, index into form+0x40
		constexpr uint32_t INST_NEXT_MOVE = 0x08u;                   // s8, -1 loops the move
		constexpr uint32_t INST_POSE_FLAGS = 0x09u;                  // u8, PoseCharacterActors clears bit 0
		constexpr uint32_t INST_SEVERED = 0x0Cu;                     // u32, bit i: bone i is severed
		constexpr uint32_t INST_FRACTION = 0x18u;                    // float, keyframe time run since the last one
		constexpr uint32_t INST_FRAME = 0x1Cu;                       // s16
		constexpr uint32_t INST_PERIOD = 0x1Eu;                      // s16 ms, 0 frozen
		constexpr uint32_t INST_BODY_ORIENTATION = 0x8Cu;            // br_matrix34 the children are posed under
		constexpr uint32_t INST_TRANSITION = 0xBCu;                  // move-transition slot, blended by time
		constexpr uint32_t INST_BONE_OVERRIDE = 0xE8u;
		constexpr uint32_t FORM_BONE_COUNT = 0x29u;                  // u8
		constexpr uint32_t FORM_ACTOR_SETS = 0x30u;                  // *(+0x30) + (set * 2 + 1) * 4 = br_actor*[]
		constexpr uint32_t FORM_MOVES = 0x40u;                       // +move * 8 + 4 = move*
		constexpr uint32_t MOVE_FRAME_COUNT = 0x28u;                 // s16
		constexpr uint32_t MOVE_FLAGS = 0x30u;                       // bit 0: holds on its last frame in replay
		constexpr uint32_t MOVE_FRAMES = 0x4Cu;                      // frame*, each led by its root br_matrix34
		constexpr uint32_t MOVE_FRAME_STRIDE = 0x34u;
		constexpr uint32_t PHYSICS_MASK = 7u;
		constexpr uint32_t PHYSICS_DRIVEN = 5u;                      // simple body registered and simulated
		constexpr unsigned MAX_BONES = 32u;
		constexpr int POSE_ALL = 0;
		constexpr int POSE_CHILDREN = 3;

		using render_a_frame_t = void(__cdecl*)();
		using pose_t = void(__fastcall*)(const void* inst, int mode, int move_root);
		using lp_inverse_t = void(__cdecl*)(br_matrix34* dst, const br_matrix34* src);
		using mul_t = void(__cdecl*)(br_matrix34* dst, const br_matrix34* a, const br_matrix34* b);
		render_a_frame_t o_render_a_frame = nullptr;

		struct held_matrix
		{
			br_matrix34* t;
			br_matrix34 value;   // the game's own, put back after the frame
		};
		std::vector<held_matrix> g_held;

		void hold(br_matrix34* t) {
			g_held.push_back({ t, *t });
		}

		struct quaternion { float x, y, z, w; };

		quaternion to_quaternion(const float r[3][3])
		{
			const float trace = r[0][0] + r[1][1] + r[2][2];
			if (trace > 0.0f)
			{
				const float s = std::sqrt(trace + 1.0f) * 2.0f;
				return { (r[1][2] - r[2][1]) / s, (r[2][0] - r[0][2]) / s, (r[0][1] - r[1][0]) / s, 0.25f * s };
			}
			if (r[0][0] > r[1][1] && r[0][0] > r[2][2])
			{
				const float s = std::sqrt(1.0f + r[0][0] - r[1][1] - r[2][2]) * 2.0f;
				return { 0.25f * s, (r[1][0] + r[0][1]) / s, (r[2][0] + r[0][2]) / s, (r[1][2] - r[2][1]) / s };
			}
			if (r[1][1] > r[2][2])
			{
				const float s = std::sqrt(1.0f + r[1][1] - r[0][0] - r[2][2]) * 2.0f;
				return { (r[1][0] + r[0][1]) / s, 0.25f * s, (r[2][1] + r[1][2]) / s, (r[2][0] - r[0][2]) / s };
			}
			const float s = std::sqrt(1.0f + r[2][2] - r[0][0] - r[1][1]) * 2.0f;
			return { (r[2][0] + r[0][2]) / s, (r[2][1] + r[1][2]) / s, 0.25f * s, (r[0][1] - r[1][0]) / s };
		}

		void to_rotation(const quaternion& q, float r[3][3])
		{
			const float x = q.x, y = q.y, z = q.z, w = q.w;
			r[0][0] = 1 - 2 * (y * y + z * z); r[0][1] = 2 * (x * y + z * w);     r[0][2] = 2 * (x * z - y * w);
			r[1][0] = 2 * (x * y - z * w);     r[1][1] = 1 - 2 * (x * x + z * z); r[1][2] = 2 * (y * z + x * w);
			r[2][0] = 2 * (x * z + y * w);     r[2][1] = 2 * (y * z - x * w);     r[2][2] = 1 - 2 * (x * x + y * y);
		}

		// Rows of a BRender matrix are the images of the axes. Their lengths carry any scale,
		// which blends apart from the rotation; the rotation takes the shorter arc.
		br_matrix34 blend(const br_matrix34& a, const br_matrix34& b, const float k)
		{
			float ra[3][3], rb[3][3], sa[3], sb[3];
			for (int i = 0; i < 3; ++i)
			{
				sa[i] = std::sqrt(a.m[i][0] * a.m[i][0] + a.m[i][1] * a.m[i][1] + a.m[i][2] * a.m[i][2]);
				sb[i] = std::sqrt(b.m[i][0] * b.m[i][0] + b.m[i][1] * b.m[i][1] + b.m[i][2] * b.m[i][2]);
				if (!(sa[i] > 1e-8f) || !(sb[i] > 1e-8f)) {
					return a;
				}
				for (int j = 0; j < 3; ++j)
				{
					ra[i][j] = a.m[i][j] / sa[i];
					rb[i][j] = b.m[i][j] / sb[i];
				}
			}

			const quaternion qa = to_quaternion(ra);
			quaternion qb = to_quaternion(rb);
			if (qa.x * qb.x + qa.y * qb.y + qa.z * qb.z + qa.w * qb.w < 0.0f) {
				qb = { -qb.x, -qb.y, -qb.z, -qb.w };
			}
			quaternion q = { qa.x + (qb.x - qa.x) * k, qa.y + (qb.y - qa.y) * k, qa.z + (qb.z - qa.z) * k, qa.w + (qb.w - qa.w) * k };
			const float length = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
			if (!(length > 1e-8f)) {
				return a;
			}
			q = { q.x / length, q.y / length, q.z / length, q.w / length };

			float r[3][3];
			to_rotation(q, r);
			br_matrix34 out{};
			for (int i = 0; i < 3; ++i)
			{
				const float scale = sa[i] + (sb[i] - sa[i]) * k;
				for (int j = 0; j < 3; ++j) {
					out.m[i][j] = r[i][j] * scale;
				}
				out.m[3][i] = a.m[3][i] + (b.m[3][i] - a.m[3][i]) * k;
			}
			return out;
		}

		// The last two distinct matrices replay gave one actor, and the replay time each came.
		struct replay_track
		{
			br_matrix34 prev;
			br_matrix34 cur;
			uint32_t prev_time;
			uint32_t cur_time;
			bool has_prev;
			uint32_t seen;
		};
		std::unordered_map<br_matrix34*, replay_track> g_tracks;
		uint32_t g_replay_serial = 0;
		std::optional<uint32_t> g_last_replay_time;
		int g_replay_direction = 0;

		void reset_replay_tracks()
		{
			g_tracks.clear();
			g_last_replay_time.reset();
			g_replay_direction = 0;
		}

		// History only holds while replay time runs on one way: a turn or a jump drops it.
		void begin_replay_frame(const uint32_t now)
		{
			++g_replay_serial;
			if (g_last_replay_time)
			{
				const int64_t dt = static_cast<int64_t>(now) - static_cast<int64_t>(*g_last_replay_time);
				const int direction = dt > 0 ? 1 : dt < 0 ? -1 : g_replay_direction;
				if (std::abs(dt) > MAX_REPLAY_STEP_MS || (g_replay_direction != 0 && direction != g_replay_direction)) {
					g_tracks.clear();
				}
				g_replay_direction = direction;
			}
			g_last_replay_time = now;
		}

		void end_replay_frame()
		{
			std::erase_if(g_tracks, [](const auto& entry) { return entry.second.seen != g_replay_serial; });
		}

		// Draws `t` between the last two matrices replay gave it: the previous one when the
		// current one has just come, the current one a step later.
		void smooth_replayed(br_matrix34* t, const uint32_t now)
		{
			auto [it, added] = g_tracks.try_emplace(t);
			replay_track& track = it->second;
			if (added)
			{
				track.cur = *t;
				track.cur_time = now;
			}
			else if (std::memcmp(&track.cur, t, sizeof(br_matrix34)) != 0)
			{
				track.prev = track.cur;
				track.prev_time = track.cur_time;
				track.cur = *t;
				track.cur_time = now;
				track.has_prev = true;
			}
			track.seen = g_replay_serial;

			if (!track.has_prev || track.cur_time == track.prev_time) {
				return;
			}
			const float step = static_cast<float>(static_cast<int64_t>(track.cur_time) - static_cast<int64_t>(track.prev_time));
			const float since = static_cast<float>(static_cast<int64_t>(now) - static_cast<int64_t>(track.cur_time));
			hold(t);
			*t = blend(track.prev, track.cur, std::clamp(since / step, 0.0f, 1.0f));
		}

		// A ped's instance, its current move and its bones: actors[0] is the root (the Spine).
		struct skeleton
		{
			uint8_t* inst;
			const uint8_t* move;
			br_actor* const* actors;
			unsigned bones;

			int16_t frame() const { return read<int16_t>(inst, INST_FRAME); }
			int16_t frame_count() const { return read<int16_t>(move, MOVE_FRAME_COUNT); }

			// A bone the game poses, or null for a severed one.
			br_matrix34* bone(const unsigned i) const
			{
				if (read<uint32_t>(inst, INST_SEVERED) & (1u << i) || !can_read(actors[i], sizeof(br_actor))) {
					return nullptr;
				}
				return &actors[i]->t;
			}
		};

		std::optional<skeleton> find_skeleton(const uint8_t* record)
		{
			if (!(read<uint16_t>(record, 0x08) & PED_ACTIVE)) {
				return std::nullopt;
			}
			const auto inst = read<uint8_t*>(record, 0x00);
			if (!can_read(inst, INST_BONE_OVERRIDE + 4) || read<int8_t>(inst, INST_ACTOR_SET) < 0
				|| read<const void*>(inst, INST_BONE_OVERRIDE) != nullptr)
			{
				return std::nullopt;
			}

			const auto personality = read<const uint8_t*>(inst, 0x00);
			const auto form = can_read(personality, PERSONALITY_FORM + 4) ? read<const uint8_t*>(personality, PERSONALITY_FORM) : nullptr;
			if (!can_read(form, FORM_MOVES + 4)) {
				return std::nullopt;
			}

			const auto moves = read<const uint8_t*>(form, FORM_MOVES);
			const int move_index = read<int8_t>(inst, INST_MOVE);
			if (move_index < 0 || !can_read(moves + move_index * 8, 8)) {
				return std::nullopt;
			}
			const auto move = read<const uint8_t*>(moves, move_index * 8 + 4);
			if (!can_read(move, MOVE_FRAMES + 4)) {
				return std::nullopt;
			}

			const auto sets = read<const uint8_t*>(form, FORM_ACTOR_SETS);
			const int set = read<int8_t>(inst, INST_ACTOR_SET);
			if (!can_read(sets + (set * 2 + 1) * 4, 4)) {
				return std::nullopt;
			}
			const auto actors = read<br_actor* const*>(sets, (set * 2 + 1) * 4);
			const unsigned bones = std::min<unsigned>(read<uint8_t>(form, FORM_BONE_COUNT), MAX_BONES);
			if (bones == 0 || !can_read(actors, sizeof(br_actor*) * bones) || !can_read(actors[0], sizeof(br_actor))) {
				return std::nullopt;
			}
			return skeleton{ inst, move, actors, bones };
		}

		// The keyframe after the current one, or -1 when the move ends into another move.
		int next_frame(const skeleton& s)
		{
			const int next = s.frame() + 1;
			if (next < s.frame_count()) {
				return next;
			}
			return read<int8_t>(s.inst, INST_NEXT_MOVE) < 0 ? 0 : -1;
		}

		// Replay never follows a queued move: the end of a move wraps to its first frame, or
		// holds on the last.
		int replay_next_frame(const skeleton& s)
		{
			const int next = s.frame() + 1;
			if (next < s.frame_count()) {
				return next;
			}
			return read<uint8_t>(s.move, MOVE_FLAGS) & 1u ? -1 : 0;
		}

		const br_matrix34* root_transform(const skeleton& s, const int frame)
		{
			const auto frames = read<const uint8_t*>(s.move, MOVE_FRAMES);
			const auto t = frames + frame * MOVE_FRAME_STRIDE;
			return frame >= 0 && frame < s.frame_count() && can_read(t, sizeof(br_matrix34))
				? reinterpret_cast<const br_matrix34*>(t) : nullptr;
		}

		// PoseCharacterActors at `frame`, leaving the instance's frame and flags and the game's
		// RNG as they were.
		void pose_at(const skeleton& s, const int frame, const int mode, const int move_root)
		{
			auto& current = *reinterpret_cast<int16_t*>(s.inst + INST_FRAME);
			auto& flags = s.inst[INST_POSE_FLAGS];
			auto& seed = *reinterpret_cast<uint32_t*>(rebase(ADDR_g_rand_state));
			const int16_t saved_frame = current;
			const uint8_t saved_flags = flags;
			const uint32_t saved_seed = seed;

			current = static_cast<int16_t>(frame);
			reinterpret_cast<pose_t>(rebase(ADDR_PoseCharacterActors))(s.inst, mode, move_root);

			current = saved_frame;
			flags = saved_flags;
			seed = saved_seed;
		}

		// Poses the children of a physics-driven ped at `frame` from where the physics has put
		// the root this frame, as RederiveFromPhysicsRoot does on a physics step: the body
		// orientation is the physics root with the keyframe's own root rotation undone.
		bool pose_children_on_root(const skeleton& s, const int frame)
		{
			const br_matrix34* root_t = root_transform(s, frame);
			if (!root_t) {
				return false;
			}
			br_matrix34 undo_root;
			reinterpret_cast<lp_inverse_t>(rebase(ADDR_BrMatrix34LPInverse))(&undo_root, root_t);
			auto orientation = reinterpret_cast<br_matrix34*>(s.inst + INST_BODY_ORIENTATION);
			reinterpret_cast<mul_t>(rebase(ADDR_BrMatrix34Mul))(orientation, &undo_root, &s.actors[0]->t);
			orientation->m[3][0] = orientation->m[3][1] = orientation->m[3][2] = 0.0f;

			pose_at(s, frame, POSE_CHILDREN, 0);
			return true;
		}

		void blend_walking(const skeleton& s, const int next, const float k, const bool replay)
		{
			br_matrix34* bone_t[MAX_BONES] = {};
			for (unsigned i = 0; i < s.bones; ++i)
			{
				bone_t[i] = s.bone(i);
				if (bone_t[i]) {
					hold(bone_t[i]);
				}
			}

			// A step back in replay poses the frame it leaves, so the bones may hold f+1: pose f
			// again, the root staying where the step left it.
			if (replay) {
				pose_at(s, s.frame(), POSE_ALL, 0);
			}
			br_matrix34 pose_f[MAX_BONES];
			for (unsigned i = 0; i < s.bones; ++i)
			{
				if (bone_t[i]) {
					pose_f[i] = *bone_t[i];
				}
			}

			// The root moves on to g whichever way replay plays.
			auto& rate = *reinterpret_cast<float*>(rebase(ADDR_g_replay_rate));
			const float saved_rate = rate;
			if (replay) {
				rate = 1.0f;
			}
			pose_at(s, next, POSE_ALL, 1);
			rate = saved_rate;

			for (unsigned i = 0; i < s.bones; ++i)
			{
				if (bone_t[i]) {
					*bone_t[i] = blend(pose_f[i], *bone_t[i], k);
				}
			}
		}

		// `next` is -1 when the limbs should only follow the root, without a keyframe blend.
		void repose_driven(const skeleton& s, const int next, const float k)
		{
			br_matrix34* bone_t[MAX_BONES] = {};
			hold(reinterpret_cast<br_matrix34*>(s.inst + INST_BODY_ORIENTATION));
			for (unsigned i = 1; i < s.bones; ++i)
			{
				bone_t[i] = s.bone(i);
				if (bone_t[i]) {
					hold(bone_t[i]);
				}
			}

			if (!pose_children_on_root(s, s.frame()) || next < 0) {
				return;
			}

			br_matrix34 pose_f[MAX_BONES];
			for (unsigned i = 1; i < s.bones; ++i)
			{
				if (bone_t[i]) {
					pose_f[i] = *bone_t[i];
				}
			}
			if (!pose_children_on_root(s, next)) {
				return;
			}
			for (unsigned i = 1; i < s.bones; ++i)
			{
				if (bone_t[i]) {
					*bone_t[i] = blend(pose_f[i], *bone_t[i], k);
				}
			}
		}

		void smooth_ped(const uint8_t* record, const std::optional<uint32_t> replay_time)
		{
			const auto s = find_skeleton(record);
			if (!s) {
				return;
			}

			if (replay_time)
			{
				for (unsigned i = 0; i < s->bones; ++i)
				{
					if (read<uint32_t>(s->inst, INST_SEVERED) & (1u << i) && can_read(s->actors[i], sizeof(br_actor))) {
						smooth_replayed(&s->actors[i]->t, *replay_time);
					}
				}
			}

			// Under the powerup, every pose of a corpse is freshly random. Replay does not
			// rewind health, so a ped that dies later reads dead all through it; the state it
			// does rewind.
			if (read<uint8_t>(record, 0x04) == 0 && read<uint32_t>(record, PED_STATE) == PED_STATE_DEAD
				&& *reinterpret_cast<const float*>(rebase(ADDR_g_corpse_twitch)) != 0.0f)
			{
				return;
			}

			const bool animating = read<int16_t>(s->inst, INST_PERIOD) != 0
				&& read<const void*>(s->inst, INST_TRANSITION) == nullptr;
			const float k = std::clamp(read<float>(s->inst, INST_FRACTION), 0.0f, 1.0f);
			const int next = !animating || !(k > 0.0f) ? -1
				: replay_time ? replay_next_frame(*s) : next_frame(*s);

			const uint32_t physics = read<uint32_t>(s->inst, INST_PHYSICS_FLAGS) & PHYSICS_MASK;
			if (physics == PHYSICS_DRIVEN)
			{
				if (replay_time) {
					smooth_replayed(&s->actors[0]->t, *replay_time);
				}
				repose_driven(*s, next, k);
			}
			else if (!(physics & 6u) && next >= 0) {
				blend_walking(*s, next, k, replay_time.has_value());
			}
		}

		void __cdecl hk_render_a_frame()
		{
			g_held.clear();
			const bool smoothing = shared::common::config::get().effects.ped_interpolation;
			std::optional<uint32_t> replay_time;
			if (smoothing && *reinterpret_cast<const int*>(rebase(ADDR_g_action_replay_mode)) != 0)
			{
				replay_time = *reinterpret_cast<const uint32_t*>(rebase(ADDR_g_replay_time));
				begin_replay_frame(*replay_time);
			}
			else {
				reset_replay_tracks();
			}

			if (smoothing)
			{
				const int count = *reinterpret_cast<const int*>(rebase(ADDR_g_ped_count));
				const auto records = *reinterpret_cast<const uint8_t* const*>(rebase(ADDR_g_peds));
				if (count > 0 && count <= static_cast<int>(MAX_PEDS)
					&& can_read(records, static_cast<size_t>(count) * PED_RECORD_STRIDE))
				{
					for (int i = 0; i < count; ++i) {
						smooth_ped(records + i * PED_RECORD_STRIDE, replay_time);
					}
				}
			}
			if (replay_time) {
				end_replay_frame();
			}

			o_render_a_frame();

			for (auto h = g_held.rbegin(); h != g_held.rend(); ++h) {
				*h->t = h->value;
			}
			g_held.clear();
		}
	}

	void install_ped_interpolation()
	{
		if (shared::utils::hook::detour(rebase(ADDR_RenderAFrame), hk_render_a_frame,
			reinterpret_cast<void**>(&o_render_a_frame)))
		{
			shared::common::log("Game", "pedestrian interpolation: peds drawn between their keyframes and physics steps, in play and replay",
				shared::common::LOG_TYPE::LOG_TYPE_GREEN, true);
			return;
		}
		shared::common::log("Game", "pedestrian interpolation not installed: RenderAFrame could not be hooked",
			shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
	}
}
