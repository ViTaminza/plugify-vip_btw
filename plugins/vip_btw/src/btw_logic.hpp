// vip_btw — engine-free part of the Buy Team Weapon module: the command parsing and the weapon
// tables of the original (ref/src/cs2-vip-modules/VIP_BuyTeamWeapon/source/vip_btw.cpp).
//
// Kept pure so they are unit-testable (tests/vip_btw_tests.cpp).

#pragma once

#include <cctype>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace vip_btw {

// The original's WeaponData: the entity name ("weapon_ak47") and the price.
struct WeaponData {
	std::string szName;
	int iPrice = 0;
};

// The original's weapons are keyed by the command name; iteration order is the map's (sorted),
// which is what the round-start list showed.
using WeaponTable = std::map<std::string, WeaponData>;

// The original's SplitStringBySpace: whitespace separated, double quotes group a token and are
// removed (the CCommand::Tokenize stand-in of that module).
inline std::vector<std::string> SplitStringBySpace(std::string_view input) {
	std::vector<std::string> tokens;
	std::string current;
	bool inQuotes = false;

	for (const char ch : input) {
		if (ch == '"') {
			inQuotes = !inQuotes;
			continue;
		}
		if (std::isspace(static_cast<unsigned char>(ch)) && !inQuotes) {
			if (!current.empty()) {
				tokens.push_back(current);
				current.clear();
			}
		} else {
			current += ch;
		}
	}
	if (!current.empty()) {
		tokens.push_back(current);
	}
	return tokens;
}

// The original's safe_replace: every occurrence of `search` is replaced; an empty `search` leaves
// the subject unchanged (the guard against the infinite loop that comment warned about).
inline std::string SafeReplace(std::string_view subject, std::string_view search, std::string_view replace) {
	if (search.empty()) {
		return std::string(subject);
	}
	std::string result(subject);
	std::size_t pos = 0;
	while ((pos = result.find(search, pos)) != std::string::npos) {
		result.replace(pos, search.size(), replace);
		pos += replace.size();
	}
	return result;
}

// sCommand = safe_replace(sCommand, "mm_", ""); sCommand = safe_replace(sCommand, "!", "");
inline std::string CleanCommand(std::string_view command) {
	return SafeReplace(SafeReplace(command, "mm_", ""), "!", "");
}

// int iTeamNum = iTeam == 3 ? 1 : 0; ("the CT table for CTs, the T table for everyone else")
inline int TeamIndex(int iTeam) {
	return iTeam == 3 ? 1 : 0;
}

// The ', '-joined command list of the round-start message (the original's Format/append loop).
inline std::string JoinWeapons(const WeaponTable& weapons) {
	std::string list;
	for (const auto& pair : weapons) {
		if (!list.empty()) {
			list += ", ";
		}
		list += pair.first;
	}
	return list;
}

} // namespace vip_btw
