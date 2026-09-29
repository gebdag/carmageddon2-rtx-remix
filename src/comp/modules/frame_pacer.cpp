#include "std_include.hpp"
#include "frame_pacer.hpp"

#include "shared/common/config.hpp"

namespace comp
{
	namespace
	{
		// FrameTiming (0x00492680) clamps every frame to at least 10 ms of game time.
		constexpr double MIN_INTERVAL_MS = 12.0;

		// Intervals longer than this are loads, pauses or menus, not the race's pace.
		constexpr double MAX_TRACKED_INTERVAL_MS = 200.0;

		// The average follows a change of pace within a second or so at 30 fps.
		constexpr double AVERAGE_WEIGHT = 0.1;

		// Sleeping may overshoot by a fraction of a millisecond; spin through the rest.
		constexpr double SPIN_MS = 0.75;

		int64_t now()
		{
			LARGE_INTEGER t{};
			QueryPerformanceCounter(&t);
			return t.QuadPart;
		}
	}

	frame_pacer& frame_pacer::get()
	{
		static frame_pacer pacer;
		return pacer;
	}

	frame_pacer::frame_pacer()
	{
		LARGE_INTEGER f{};
		QueryPerformanceFrequency(&f);
		m_frequency = f.QuadPart;
		m_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
	}

	void frame_pacer::wait_until(const int64_t deadline)
	{
		const auto ms_left = [this, deadline] {
			return static_cast<double>(deadline - now()) * 1000.0 / static_cast<double>(m_frequency);
		};

		const double sleep_ms = ms_left() - SPIN_MS;
		if (sleep_ms > 0.0 && m_timer)
		{
			LARGE_INTEGER due{};
			due.QuadPart = -static_cast<LONGLONG>(sleep_ms * 10000.0);   // relative, 100 ns units
			if (SetWaitableTimer(m_timer, &due, 0, nullptr, nullptr, FALSE)) {
				WaitForSingleObject(m_timer, INFINITE);
			}
		}
		while (ms_left() > 0.0) {
			YieldProcessor();
		}
	}

	void frame_pacer::after_present()
	{
		const auto& timing = shared::common::config::get().timing;
		const int64_t t = now();
		m_last_wait_ms = 0.0;

		if (!timing.frame_pacing || !m_last_present)
		{
			m_last_present = t;
			return;
		}

		const double interval_ms = static_cast<double>(t - m_last_present) * 1000.0 / static_cast<double>(m_frequency);
		if (interval_ms > MAX_TRACKED_INTERVAL_MS)
		{
			m_last_present = t;
			return;
		}

		// Held frames count at their held length, so the average is the pace the frames
		// actually leave at and does not drift down towards the bursts.
		const double target_ms = std::max(MIN_INTERVAL_MS, m_average_ms * timing.frame_pacing_fraction);
		if (interval_ms < target_ms)
		{
			const int64_t deadline = m_last_present + static_cast<int64_t>(target_ms * static_cast<double>(m_frequency) / 1000.0);
			wait_until(deadline);
			m_last_wait_ms = target_ms - interval_ms;
		}

		const int64_t released = now();
		const double paced_ms = static_cast<double>(released - m_last_present) * 1000.0 / static_cast<double>(m_frequency);
		m_average_ms = m_average_ms > 0.0 ? m_average_ms + AVERAGE_WEIGHT * (paced_ms - m_average_ms) : paced_ms;
		m_last_present = released;
	}
}
