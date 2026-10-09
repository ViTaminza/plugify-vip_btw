// vip_btw unit tests — the engine-free part: the command tokenizer, the "mm_"/"!" stripping, the
// team table choice and the round-start weapon list.
//
// Run by ../../test.sh (inside the Steam Runtime image, cwd = build/tests).

#include "btw_logic.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;

void CheckEq(const std::string& actual, const std::string& expected, const std::string& what) {
	++g_checks;
	if (actual != expected) {
		++g_failures;
		std::printf("FAIL: %s (expected '%s', got '%s')\n", what.c_str(), expected.c_str(), actual.c_str());
	}
}

void CheckEq(int actual, int expected, const std::string& what) {
	++g_checks;
	if (actual != expected) {
		++g_failures;
		std::printf("FAIL: %s (expected %d, got %d)\n", what.c_str(), expected, actual);
	}
}

std::string Join(const std::vector<std::string>& tokens) {
	std::string joined;
	for (std::size_t i = 0; i < tokens.size(); ++i) {
		if (i > 0) {
			joined += '|';
		}
		joined += tokens[i];
	}
	return joined;
}

void CheckSplit(const char* input, const char* expected) {
	CheckEq(Join(vip_btw::SplitStringBySpace(input)), expected, std::string("split '") + input + "'");
}

void CheckClean(const char* input, const char* expected) {
	CheckEq(vip_btw::CleanCommand(input), expected, std::string("clean '") + input + "'");
}

} // namespace

int main() {
	std::printf("vip_btw tests\n");

	// Tokenizing (the original's SplitStringBySpace over the command content).
	CheckSplit("!ak47", "!ak47");
	CheckSplit("mm_ak47", "mm_ak47");
	CheckSplit("mm_ak47 15 20", "mm_ak47|15|20");
	CheckSplit("  mm_ak47   extra ", "mm_ak47|extra");
	CheckSplit("say \"a b\" c", "say|a b|c");
	CheckSplit("\"quoted only\"", "quoted only");
	CheckSplit("", "");

	// "mm_"/"!" stripping (safe_replace, all occurrences).
	CheckClean("!ak47", "ak47");
	CheckClean("mm_ak47", "ak47");
	CheckClean("!mm_ak47", "ak47");
	CheckClean("mm_!m4a1", "m4a1");
	CheckClean("ak47", "ak47");
	CheckClean("ak_47", "ak_47"); // no partial match of a command name
	CheckEq(vip_btw::SafeReplace("abc", "", "x"), "abc", "empty search leaves the subject");

	// The team table choice of the original (CT = 3 -> table 1, anything else -> table 0).
	CheckEq(vip_btw::TeamIndex(3), 1, "CT uses the CT table");
	CheckEq(vip_btw::TeamIndex(2), 0, "T uses the T table");
	CheckEq(vip_btw::TeamIndex(1), 0, "spectators use the T table");
	CheckEq(vip_btw::TeamIndex(0), 0, "no team uses the T table");

	// The round-start list (map order = sorted, like the original's std::map).
	vip_btw::WeaponTable table;
	CheckEq(vip_btw::JoinWeapons(table), "", "empty table");
	table["m4a4"] = {"weapon_m4a1", 3100};
	table["m4a1"] = {"weapon_m4a1_silencer", 2900};
	CheckEq(vip_btw::JoinWeapons(table), "m4a1, m4a4", "two weapons");
	table["awp"] = {"weapon_awp", 4750};
	CheckEq(vip_btw::JoinWeapons(table), "awp, m4a1, m4a4", "three weapons");

	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
