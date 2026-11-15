// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>

#include "../btop_shared.hpp"

namespace Hwmon {

	inline auto read_number(const std::filesystem::path& path) -> std::optional<long long> {
		long long value;
		if (std::ifstream file(path); file >> value) return value;
		return std::nullopt;
	}

	inline auto entries(const std::filesystem::path& path) -> std::vector<std::filesystem::path> {
		std::vector<std::filesystem::path> result;
		std::error_code ec;
		std::filesystem::directory_iterator it(path, std::filesystem::directory_options::skip_permission_denied, ec), end;
		for (; not ec and it != end; it.increment(ec)) result.push_back(it->path());
		std::ranges::sort(result);
		return result;
	}

	inline auto canonical(const std::filesystem::path& path) -> std::filesystem::path {
		std::error_code ec;
		return std::filesystem::canonical(path, ec);
	}

	#ifdef GPU_SUPPORT
	inline void collect_gpu(Gpu::gpu_info& gpu, const std::filesystem::path& hwmon) {
		gpu.hotspot.reset(); gpu.memory_temp.reset();
		if (hwmon.empty()) return;
		for (const auto& [channel, value, limit, fallback] : {
			std::tuple{"temp2_", &gpu.hotspot, &gpu.hotspot_max, 110ll},
			std::tuple{"temp3_", &gpu.memory_temp, &gpu.memory_temp_max, 100ll}}) {
			value->reset();
			*limit = fallback;
			const auto current = read_number(hwmon / (std::string(channel) + "input"));
			const auto fault = read_number(hwmon / (std::string(channel) + "fault"));
			if (current and *current >= -40000 and *current <= 200000 and (not fault or *fault == 0))
				*value = *current / 1000;
			const auto critical = read_number(hwmon / (std::string(channel) + "crit"));
			if (critical and *critical >= 1000 and *critical <= 200000) *limit = *critical / 1000;
		}
	}
	#endif

	// A partition and its controller have different paths; compare complete path components.
	inline bool below(const std::filesystem::path& child, const std::filesystem::path& parent) {
		if (child.empty() or parent.empty()) return false;
		auto relative = child.lexically_relative(parent);
		return not relative.empty() and *relative.begin() != "..";
	}

	inline auto backing_devices(const std::filesystem::path& block, std::set<std::filesystem::path>& visited)
		-> std::vector<std::filesystem::path> {
		std::vector<std::filesystem::path> result;
		const auto device = Hwmon::canonical(block);
		if (device.empty() or not visited.insert(device).second) return result;
		result.push_back(device);
		for (const auto& slave : entries(device / "slaves")) {
			auto backing = backing_devices(slave, visited);
			result.insert(result.end(), backing.begin(), backing.end());
		}
		return result;
	}

	// Read hwmon directly: no root privileges, smartctl subprocesses or new libraries.
	// Rescan each update so removed/unreadable sensors never leave stale temperatures.
	inline void collect(Mem::mem_info& mem, const std::filesystem::path& sys = "/sys") {
		struct drive_sensor {
			std::filesystem::path device;
			Mem::temperature_info temperature;
		};
		std::vector<drive_sensor> drives;
		mem.temperatures.clear();
		for (const auto& hwmon : entries(sys / "class/hwmon")) {
			std::string driver;
			std::ifstream(hwmon / "name") >> driver;
			const bool ram = driver == "spd5118" or driver == "jc42";
			if (not ram and driver != "nvme" and driver != "drivetemp") continue;
			const auto value = read_number(hwmon / "temp1_input");
			const auto fault = read_number(hwmon / "temp1_fault");
			if (not value or *value < -40'000 or *value > 200'000 or (fault and *fault != 0)) continue;
			const auto device = Hwmon::canonical(hwmon / "device");
			long long limit = driver == "drivetemp" ? 60 : 85;
			for (const auto& suffix : {"crit", "max"}) {
				const auto threshold = read_number(hwmon / (std::string("temp1_") + suffix));
				if (threshold and *threshold >= 1'000 and *threshold <= 200'000) {
					limit = *threshold / 1000;
					break;
				}
			}
			Mem::temperature_info temperature{device.filename().string(), *value / 1000, limit};
			if (ram) mem.temperatures.push_back(temperature);
			else if (not device.empty()) drives.push_back({device, temperature});
		}
		std::ranges::sort(mem.temperatures, {}, &Mem::temperature_info::name);
		for (size_t i = 0; i < mem.temperatures.size(); ++i)
			mem.temperatures[i].name = "DIMM" + std::to_string(i + 1);

		for (auto& [mount, disk] : mem.disks) {
			disk.temperatures.clear();
			if (disk.dev.empty() or disk.name == "swap") continue;
			std::set<std::filesystem::path> visited;
			const auto devices = backing_devices(sys / "class/block" / disk.dev.filename(), visited);
			for (const auto& drive : drives) {
				if (std::ranges::any_of(devices, [&](const auto& block) { return below(block, drive.device); }))
					disk.temperatures.push_back(drive.temperature);
			}
		}
	}
}
