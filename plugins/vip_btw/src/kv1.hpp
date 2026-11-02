// vip_btw — a read-only Valve KeyValues (KV1) reader for the plugin's config.
//
// The config is the original Metamod file, unchanged:
//
//   "VIP"
//   {
//       "CT"
//       {
//           "ak47"  //!ak47 and mm_ak47
//           {
//               "price"     "2700"
//               "weapon"    "weapon_ak47"
//           }
//       }
//       "T" { ... }
//   }
//
// The plugin is pure s2sdk (docs/PLUGIN_STANDARD.md §7): the hl2sdk KeyValues the original loaded
// through pisexcompat's shim is replaced by this parser, which lives in the plugin itself. The
// grammar is the one pisexcompat's KeyValues accepted (the same code s2guard shipped):
//   * quoted ("key" "value") and unquoted tokens, nested "key" { ... } blocks, a key may carry a
//     value and a block,
//   * // and /* */ comments, escapes \" \\ \n \r \t inside quotes,
//   * #base / #include / #define lines are skipped (no inclusion),
//   * an unterminated string, a missing '}' or a stray top-level '}' makes the file invalid
//     (nullopt; the caller stays disabled instead of using a half-read tree).
// Like Valve's loader, a file that is a single "Root" { ... } block is returned as that block, so
// `ParseKv1(text)->Find("CT")` reads the section inside it. Key lookup is case-insensitive.

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vip_btw {

struct KvNode {
	std::string name;
	std::string value;
	bool hasValue = false;
	std::vector<KvNode> children;

	// First child with that name (case-insensitive), or nullptr.
	const KvNode* Find(std::string_view key) const;
	// The child's value; `def` when the child is missing or has no value.
	std::string_view GetString(std::string_view key, std::string_view def = {}) const;
	// The child's value as an int, with Valve's atoi fallback ("5 // x" -> 5, "abc" -> 0); `def`
	// when the child is missing or its value is empty.
	int GetInt(std::string_view key, int def = 0) const;
};

std::optional<KvNode> ParseKv1(std::string_view text);
std::optional<KvNode> LoadKv1File(const std::string& path);

} // namespace vip_btw
