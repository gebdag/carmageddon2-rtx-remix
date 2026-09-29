#pragma once

namespace comp
{
	/*
	 * Evens out the spacing of presented frames.
	 *
	 * The Remix bridge lets the game run frames ahead, so presents come in bursts: three at
	 * the render's pace, then one a few milliseconds after the last. The game cannot place
	 * the world on a frame that short (findings 70.5): FrameTiming counts it as at least
	 * 10 ms, and the physics interpolation built on that puts the cars at the wrong point of
	 * their step -- a climbing car drops back, then jumps ahead on the next frame. The chase
	 * camera follows the car rigidly across the ground but smooths its height, so the error
	 * shows as a vertical shake of the view on every slope.
	 *
	 * After each present the pacer waits until a fraction of the recent average interval has
	 * passed, and never less than the game's 10 ms floor plus a margin. Frames that already
	 * take that long pass straight through, so the frame rate stays where the render puts it.
	 */
	class frame_pacer
	{
	public:
		static frame_pacer& get();

		// Right after the device's Present returns.
		void after_present();

		double average_interval_ms() const { return m_average_ms; }
		double last_wait_ms() const { return m_last_wait_ms; }

	private:
		frame_pacer();

		void wait_until(int64_t deadline);

		int64_t m_frequency = 0;
		int64_t m_last_present = 0;
		double m_average_ms = 0.0;
		double m_last_wait_ms = 0.0;
		HANDLE m_timer = nullptr;
	};
}
