#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

namespace comp::static_bake_coverage
{
	struct range
	{
		uint32_t chunk;
		uint32_t index_start;
		uint32_t index_count;
		uint32_t part;
		const void* material;
	};

	// A part belongs to the static draw only after its own chunk is ready.
	template <typename Ready>
	bool covers(const std::vector<range>& ranges, const size_t part, Ready ready)
	{
		return std::any_of(ranges.begin(), ranges.end(), [&](const range& r) {
			return r.part == part && ready(r.chunk);
		});
	}

	// Each successfully appended source part owns exactly one range.
	template <typename Ready>
	bool complete(const std::vector<range>& ranges, const size_t parts, Ready ready)
	{
		return parts != 0 && ranges.size() == parts
			&& std::all_of(ranges.begin(), ranges.end(), [&](const range& r) { return ready(r.chunk); });
	}

	template <typename Punch>
	size_t remove_material(std::vector<range>& ranges, const void* material, Punch punch)
	{
		const size_t before = ranges.size();
		std::erase_if(ranges, [&](const range& r) {
			if (r.material != material) { return false; }
			punch(r);
			return true;
		});
		return before - ranges.size();
	}
}
