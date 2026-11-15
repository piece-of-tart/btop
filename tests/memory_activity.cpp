// SPDX-License-Identifier: Apache-2.0
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <gtest/gtest.h>

#include "linux/memory_activity.hpp"

TEST(Pressure, ParsesNamedAveragesAndFullRegardlessOfFieldOrder) {
	std::istringstream input("some avg60=2.50 total=100 avg300=1.25 avg10=12.75\n"
		"full avg10=3.00 avg60=0.50 avg300=0.25 total=10\n");
	const auto result = MemoryActivity::parse_pressure(input);
	ASSERT_TRUE(result.available);
	ASSERT_TRUE(result.full_available);
	EXPECT_EQ(result.some, (std::array<double, 3>{12.75, 2.50, 1.25}));
	EXPECT_EQ(result.full, (std::array<double, 3>{3.00, 0.50, 0.25}));
}

TEST(Pressure, CpuDoesNotRequireFull) {
	std::istringstream input("some avg10=0.00 avg60=0.01 avg300=100.00 total=100\n");
	const auto result = MemoryActivity::parse_pressure(input);
	EXPECT_TRUE(result.available);
	EXPECT_FALSE(result.full_available);
}

TEST(Pressure, RejectsMissingMalformedAndNonFiniteAverages) {
	for (const auto* line : {"", "some avg10=1 avg60=2", "some avg10=nan avg60=0 avg300=0",
		"some avg10=-1 avg60=0 avg300=0", "some avg10=101 avg60=0 avg300=0",
		"some avg10=1garbage avg60=0 avg300=0", "some avg10=garbage avg60=0 avg300=0"}) {
		std::istringstream input(line);
		EXPECT_FALSE(MemoryActivity::parse_pressure(input).available) << line;
	}
}

class MemoryActivityTest : public testing::Test {
protected:
	std::filesystem::path root;
	Mem::mem_info mem;
	MemoryActivity::Collector collector;
	void SetUp() override {
		std::string pattern = (std::filesystem::temp_directory_path() / "btop-activity-XXXXXX").string();
		const auto result = mkdtemp(pattern.data());
		ASSERT_NE(result, nullptr);
		root = result;
	}
	void TearDown() override { std::filesystem::remove_all(root); }
	void counters(uint64_t in, uint64_t out, uint64_t major) {
		std::ofstream(root / "vmstat") << "other 123\npswpout " << out
			<< "\npgmajfault " << major << "\npswpin " << in << '\n';
	}
	void sample(uint64_t time, size_t history = 10) { collector.collect(mem, root, time, 4096, history); }
};

TEST_F(MemoryActivityTest, FirstSampleIsBaselineAndRatesUseActualElapsedTime) {
	counters(1000, 2000, 3000); sample(1'000'000);
	EXPECT_FALSE(mem.activity.available);
	EXPECT_EQ(mem.activity.swap_in.back(), 0);
	counters(1008, 2004, 3006); sample(3'000'000);
	ASSERT_TRUE(mem.activity.available);
	EXPECT_EQ(mem.activity.swap_in.back(), 16384);
	EXPECT_EQ(mem.activity.swap_out.back(), 8192);
	EXPECT_DOUBLE_EQ(mem.activity.major_faults, 3);
}

TEST_F(MemoryActivityTest, MajorFaultsCanIncreaseWithoutAnySwapIO) {
	counters(100, 200, 300); sample(1'000'000);
	counters(100, 200, 330); sample(1'500'000);
	EXPECT_TRUE(mem.activity.available);
	EXPECT_EQ(mem.activity.swap_in.back(), 0);
	EXPECT_EQ(mem.activity.swap_out.back(), 0);
	EXPECT_DOUBLE_EQ(mem.activity.major_faults, 60);
}

TEST_F(MemoryActivityTest, CounterResetDoesNotGenerateOverflowSpike) {
	counters(1000, 2000, 3000); sample(1'000'000);
	counters(1, 2, 3); sample(2'000'000);
	EXPECT_FALSE(mem.activity.available);
	EXPECT_EQ(mem.activity.swap_in.back(), 0);
	counters(2, 4, 6); sample(3'000'000);
	EXPECT_TRUE(mem.activity.available);
	EXPECT_EQ(mem.activity.swap_in.back(), 4096);
	EXPECT_EQ(mem.activity.swap_out.back(), 8192);
	EXPECT_DOUBLE_EQ(mem.activity.major_faults, 3);
}

TEST_F(MemoryActivityTest, MissingVmstatClearsRatesAndRequiresNewBaseline) {
	counters(1, 2, 3); sample(1'000'000);
	counters(2, 4, 6); sample(2'000'000);
	ASSERT_TRUE(mem.activity.available);
	std::filesystem::remove(root / "vmstat"); sample(3'000'000);
	EXPECT_FALSE(mem.activity.available);
	EXPECT_EQ(mem.activity.swap_in.back(), 0);
	EXPECT_DOUBLE_EQ(mem.activity.major_faults, 0);
	counters(100, 200, 300); sample(4'000'000);
	EXPECT_FALSE(mem.activity.available);
	counters(100, 200, 300); sample(5'000'000);
	EXPECT_TRUE(mem.activity.available);
	EXPECT_EQ(mem.activity.swap_in.back(), 0);
}

TEST_F(MemoryActivityTest, ZeroIntervalIsSafeAndHistoryStaysBounded) {
	counters(1, 2, 3); sample(1'000'000);
	counters(2, 4, 6); sample(1'000'000);
	EXPECT_FALSE(mem.activity.available);
	for (int i = 2; i < 20; ++i) { counters(i, i, i); sample(i * 1'000'000, 3); }
	EXPECT_EQ(mem.activity.swap_in.size(), 3);
	EXPECT_EQ(mem.activity.swap_out.size(), 3);
}

TEST_F(MemoryActivityTest, PressureIsIndependentPerResourceAndRemovalClearsOldData) {
	std::filesystem::create_directories(root / "pressure");
	std::ofstream(root / "pressure/cpu") << "some avg10=20 avg60=10 avg300=5 total=10\n";
	sample(1'000'000);
	EXPECT_TRUE(mem.pressure[0].available);
	EXPECT_FALSE(mem.pressure[1].available);
	EXPECT_FALSE(mem.pressure[2].available);
	std::filesystem::remove(root / "pressure/cpu"); sample(2'000'000);
	EXPECT_FALSE(mem.pressure[0].available);
}
