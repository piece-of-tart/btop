// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>

#include "../btop_shared.hpp"

namespace MemoryActivity {

	inline auto parse_pressure(std::istream& input) -> Mem::mem_info::pressure_info {
		Mem::mem_info::pressure_info result;
		for (std::string line; std::getline(input, line);) {
			std::istringstream fields(line);
			std::string kind;
			fields >> kind;
			if (kind != "some" and kind != "full") continue;
			std::array<double, 3> values{};
			std::array<bool, 3> found{};
			for (std::string field; fields >> field;) {
				const auto equal = field.find('=');
				if (equal == std::string::npos) continue;
				const auto name = field.substr(0, equal);
				const int index = name == "avg10" ? 0 : name == "avg60" ? 1 : name == "avg300" ? 2 : -1;
				if (index < 0) continue;
				const auto text = field.substr(equal + 1);
				const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), values[index]);
				found[index] = error == std::errc{} and end == text.data() + text.size()
					and std::isfinite(values[index]) and values[index] >= 0 and values[index] <= 100;
			}
			if (not std::ranges::all_of(found, [](bool value) { return value; })) continue;
			if (kind == "some") { result.some = values; result.available = true; }
			else { result.full = values; result.full_available = true; }
		}
		return result;
	}

	class Collector {
		std::optional<std::array<uint64_t, 3>> previous;
		uint64_t previous_us = 0;
	public:
		void collect(Mem::mem_info& mem, const std::filesystem::path& proc, uint64_t now_us,
					uint64_t page_size, size_t history_size) {
			for (size_t i = 0; const auto* name : {"cpu", "memory", "io"}) {
				std::ifstream pressure(proc / "pressure" / name);
				mem.pressure[i++] = parse_pressure(pressure);
			}

			std::ifstream vmstat(proc / "vmstat");
			std::array<uint64_t, 3> current{};
			std::array<bool, 3> found{};
			for (std::string name; vmstat >> name;) {
				uint64_t value;
				if (not (vmstat >> value)) break;
				const int index = name == "pswpin" ? 0 : name == "pswpout" ? 1 : name == "pgmajfault" ? 2 : -1;
				if (index >= 0) { current[index] = value; found[index] = true; }
			}
			const bool valid = std::ranges::all_of(found, [](bool value) { return value; });
			auto& activity = mem.activity;
			activity.available = false;
			activity.major_faults = 0;
			long long in = 0, out = 0;
			if (valid and previous and now_us > previous_us and page_size > 0
				and current[0] >= (*previous)[0] and current[1] >= (*previous)[1] and current[2] >= (*previous)[2]) {
				const long double seconds = (now_us - previous_us) / 1'000'000.0L;
				auto bytes_per_second = [&](size_t index) {
					return static_cast<long long>(std::min<long double>(std::numeric_limits<long long>::max(),
						(current[index] - (*previous)[index]) * static_cast<long double>(page_size) / seconds));
				};
				in = bytes_per_second(0); out = bytes_per_second(1);
				activity.major_faults = (current[2] - (*previous)[2]) / seconds;
				activity.available = true;
			}
			if (valid) { previous = current; previous_us = now_us; }
			else previous.reset();
			activity.swap_in.push_back(in); activity.swap_out.push_back(out);
			const auto limit = std::max<size_t>(1, history_size);
			while (activity.swap_in.size() > limit) activity.swap_in.pop_front();
			while (activity.swap_out.size() > limit) activity.swap_out.pop_front();
		}
	};
}
