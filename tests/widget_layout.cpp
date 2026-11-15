// SPDX-License-Identifier: Apache-2.0
#include <regex>
#include <gtest/gtest.h>

#include "btop_config.hpp"
#include "btop_draw.hpp"
#include "btop_shared.hpp"
#include "btop_theme.hpp"
#include "btop_tools.hpp"

class ActivityLayout : public testing::Test {
	std::string old_boxes;
	bool old_activity;
	int old_width, old_height;
protected:
	void SetUp() override {
		old_boxes = Config::getS("shown_boxes");
		old_activity = Config::getB("show_swap_activity");
		old_width = Term::width; old_height = Term::height;
		Config::set("shown_boxes", std::string{"net"});
		Config::set("show_swap_activity", true);
		Theme::setTheme();
	}
	void TearDown() override {
		Config::set("shown_boxes", old_boxes);
		Config::set("show_swap_activity", old_activity);
		Term::width = old_width; Term::height = old_height;
	}
};

TEST_F(ActivityLayout, MinimumWindowKeepsAllPressureRowsInsideTheSwapBox) {
	for (const int width : {36, 80}) {
		for (const int height : {16, 17, 20, 24}) {
			Term::width = width; Term::height = height;
			Draw::calcSizes();
			ASSERT_TRUE(Swap::shown);
			EXPECT_GE(Net::height, Net::min_height);
			EXPECT_LE(Net::height, 8);
			EXPECT_GE(Swap::height, Swap::min_height);
			EXPECT_EQ(Net::height + Swap::height, height);
			EXPECT_EQ(Swap::y, Net::y + Net::height);
			Mem::mem_info mem;
			const auto output = Swap::draw(mem, true);
			EXPECT_NE(output.find("Major faults:"), std::string::npos);
			EXPECT_NE(output.find("CPU "), std::string::npos);
			EXPECT_NE(output.find("MEM "), std::string::npos);
			EXPECT_NE(output.find("IO  "), std::string::npos);
			const std::regex position("\x1b\\[([0-9]+);([0-9]+)f");
			for (auto it = std::sregex_iterator(output.begin(), output.end(), position); it != std::sregex_iterator(); ++it) {
				const int row = std::stoi((*it)[1]), col = std::stoi((*it)[2]);
				EXPECT_GE(row, Swap::y);
				EXPECT_LT(row, Swap::y + Swap::height);
				EXPECT_GE(col, 1);
				EXPECT_LE(col, width);
			}
		}
	}
}

TEST_F(ActivityLayout, DisablingActivityReturnsTheWholeAreaToNetwork) {
	Term::width = 80; Term::height = 20;
	Config::set("show_swap_activity", false);
	Draw::calcSizes();
	EXPECT_FALSE(Swap::shown);
	EXPECT_EQ(Net::height, 20);
}

TEST_F(ActivityLayout, PressureNumbersChangeColorWithTheirValues) {
	Term::width = 80; Term::height = 20;
	Draw::calcSizes();
	Mem::mem_info mem;
	mem.pressure[0].available = true;
	mem.pressure[0].some = {0, 10, 20};
	const auto output = Swap::draw(mem, true);
	EXPECT_NE(Theme::g("temp")[0], Theme::g("temp")[50]);
	EXPECT_NE(Theme::g("temp")[50], Theme::g("temp")[100]);
	EXPECT_NE(output.find(Theme::g("temp")[0] + "  0.0"), std::string::npos);
	EXPECT_NE(output.find(Theme::g("temp")[50] + " 10.0"), std::string::npos);
	EXPECT_NE(output.find(Theme::g("temp")[100] + " 20.0"), std::string::npos);
}
