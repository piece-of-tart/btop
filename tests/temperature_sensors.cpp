// SPDX-License-Identifier: Apache-2.0
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

#include "linux/temperature_sensors.hpp"

namespace fs = std::filesystem;

class TemperatureSensors : public testing::Test {
protected:
	fs::path root;
	Mem::mem_info mem;

	void SetUp() override {
		std::string pattern = (fs::temp_directory_path() / "btop-hwmon-XXXXXX").string();
		const auto result = mkdtemp(pattern.data());
		ASSERT_NE(result, nullptr);
		root = result;
	}
	void TearDown() override { fs::remove_all(root); }

	void write(const fs::path& path, const std::string& value) {
		fs::create_directories(path.parent_path());
		std::ofstream(path) << value << '\n';
	}
	fs::path sensor(const std::string& id, const std::string& driver, const std::string& device, const std::string& value) {
		const auto hwmon = root / "class/hwmon" / id;
		write(hwmon / "name", driver);
		write(hwmon / "temp1_input", value);
		fs::create_directories(root / "devices" / device);
		fs::create_directory_symlink(root / "devices" / device, hwmon / "device");
		return hwmon;
	}
	void block(const std::string& name, const std::string& device) {
		fs::create_directories(root / "devices" / device);
		fs::create_directories(root / "class/block");
		fs::create_directory_symlink(root / "devices" / device, root / "class/block" / name);
	}
};

TEST_F(TemperatureSensors, DdrModulesHaveStableOrderAndCriticalLimits) {
	auto second = sensor("hwmon0", "spd5118", "i2c/19-0053", "45750");
	auto first = sensor("hwmon9", "jc42", "i2c/19-0051", "44250");
	write(first / "temp1_max", "55000");
	write(first / "temp1_crit", "85000");
	write(second / "temp1_crit", "95000");
	sensor("hwmon1", "k10temp", "cpu", "66000");
	Hwmon::collect(mem, root);
	ASSERT_EQ(mem.temperatures.size(), 2);
	EXPECT_EQ(mem.temperatures[0].name, "DIMM1");
	EXPECT_EQ(mem.temperatures[0].celsius, 44);
	EXPECT_EQ(mem.temperatures[0].max, 85);
	EXPECT_EQ(mem.temperatures[1].celsius, 45);
	EXPECT_EQ(mem.temperatures[1].max, 95);
}

TEST_F(TemperatureSensors, NvmePartitionsMatchTheirOwnControllerAndUseComposite) {
	sensor("hwmon1", "nvme", "nvme/nvme1", "53000");
	auto nvme0 = sensor("hwmon2", "nvme", "nvme/nvme0", "41000");
	write(nvme0 / "temp2_input", "63000");
	write(nvme0 / "temp1_max", "65261850");
	block("nvme0n1p1", "nvme/nvme0/nvme0n1/nvme0n1p1");
	block("nvme1n1p1", "nvme/nvme1/nvme1n1/nvme1n1p1");
	mem.disks["/"] = {"/dev/nvme1n1p1", "root"};
	mem.disks["/data"] = {"/dev/nvme0n1p1", "data"};
	Hwmon::collect(mem, root);
	ASSERT_EQ(mem.disks["/"].temperatures.size(), 1);
	ASSERT_EQ(mem.disks["/data"].temperatures.size(), 1);
	EXPECT_EQ(mem.disks["/"].temperatures[0].celsius, 53);
	EXPECT_EQ(mem.disks["/data"].temperatures[0].celsius, 41);
	EXPECT_EQ(mem.disks["/data"].temperatures[0].max, 85);
}

TEST_F(TemperatureSensors, DeviceMapperResolvesBackingDriveWithoutRecursionLoops) {
	sensor("hwmon0", "drivetemp", "scsi/0:0:0:0", "39000");
	block("sda1", "scsi/0:0:0:0/block/sda/sda1");
	block("dm-0", "virtual/block/dm-0");
	const auto slaves = root / "devices/virtual/block/dm-0/slaves";
	fs::create_directories(slaves);
	fs::create_directory_symlink(root / "class/block/sda1", slaves / "sda1");
	fs::create_directory_symlink(root / "class/block/dm-0", slaves / "dm-0");
	mem.disks["/"] = {"/dev/dm-0", "root"};
	Hwmon::collect(mem, root);
	ASSERT_EQ(mem.disks["/"].temperatures.size(), 1);
	EXPECT_EQ(mem.disks["/"].temperatures[0].celsius, 39);
	EXPECT_EQ(mem.disks["/"].temperatures[0].max, 60);
}

TEST_F(TemperatureSensors, MissingInvalidAndFaultySensorsAreNotReportedAsZero) {
	sensor("hwmon0", "spd5118", "ram/0", "not-a-number");
	sensor("hwmon1", "spd5118", "ram/1", "999999999");
	auto faulty = sensor("hwmon2", "spd5118", "ram/2", "45000");
	write(faulty / "temp1_fault", "1");
	Hwmon::collect(mem, root);
	EXPECT_TRUE(mem.temperatures.empty());
	Hwmon::collect(mem, root / "missing");
	EXPECT_TRUE(mem.temperatures.empty());
}

TEST_F(TemperatureSensors, SensorRemovalClearsPreviouslyCollectedValues) {
	auto ram = sensor("hwmon0", "spd5118", "ram/0", "45000");
	auto drive = sensor("hwmon1", "nvme", "nvme/nvme0", "51000");
	block("nvme0n1", "nvme/nvme0/nvme0n1");
	mem.disks["/"] = {"/dev/nvme0n1", "root"};
	Hwmon::collect(mem, root);
	ASSERT_FALSE(mem.temperatures.empty());
	ASSERT_FALSE(mem.disks["/"].temperatures.empty());
	fs::remove(ram / "temp1_input");
	fs::remove(drive / "temp1_input");
	Hwmon::collect(mem, root);
	EXPECT_TRUE(mem.temperatures.empty());
	EXPECT_TRUE(mem.disks["/"].temperatures.empty());
}

#ifdef GPU_SUPPORT
TEST_F(TemperatureSensors, ExtraGpuTemperaturesUseOwnLimitsAndMissingValuesStayUnknown) {
	const auto hwmon = sensor("hwmon0", "amdgpu", "gpu/0", "55000");
	write(hwmon / "temp2_input", "74000");
	write(hwmon / "temp2_crit", "110000");
	write(hwmon / "temp3_input", "56000");
	write(hwmon / "temp3_crit", "100000");
	Gpu::gpu_info gpu{};
	Hwmon::collect_gpu(gpu, hwmon);
	ASSERT_TRUE(gpu.hotspot);
	ASSERT_TRUE(gpu.memory_temp);
	EXPECT_EQ(*gpu.hotspot, 74);
	EXPECT_EQ(*gpu.memory_temp, 56);
	EXPECT_EQ(gpu.hotspot_max, 110);
	EXPECT_EQ(gpu.memory_temp_max, 100);
	std::filesystem::remove(hwmon / "temp2_input");
	write(hwmon / "temp3_fault", "1");
	Hwmon::collect_gpu(gpu, hwmon);
	EXPECT_FALSE(gpu.hotspot);
	EXPECT_FALSE(gpu.memory_temp);
}
#endif
