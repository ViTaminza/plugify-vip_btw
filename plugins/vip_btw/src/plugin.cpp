// vip_btw — Plugify port of the Pisex VIP module "VIP_BuyTeamWeapon" / "[VIP] Buy Team Weapon"
// (the original's vip_btw.cpp, 308 lines).
//
// Behaviour kept identical:
//   * config <configs>/vip_btw/vip_btw.ini (the original's addons/configs/vip/vip_btw.ini, with
//     <configs>/vip/vip_btw.ini kept as a fallback for the Metamod layout), the "VIP" KeyValues
//     file with a "CT" and a "T" section; every subkey is a command name with "price" and
//     "weapon".
//   * every weapon registers two commands: the console "mm_<name>" and the chat "!<name>".
//   * feature "btw" (VIP_BOOL, HIDE).
//   * "round_start" prints the round-start message to every VIP with the feature, listing the
//     commands of their own table (the CT table for CTs, the T table for everyone else; each
//     table is a std::map, so the list is sorted) - the message uses the phrase
//     "btw_round_start", which the original's vip.phrases.txt does not contain, so the original
//     prints an empty line there as well (kept).
//   * a buy command: unknown commands are ignored; only a VIP with the feature can buy; the pawn
//     must be alive; the command must be in the player's table; the player must be in the buy
//     zone (schematic m_bInBuyZone); the item services and the money services must exist; enough
//     money buys the weapon (GiveNamedItem) and marks m_pInGameMoneyServices as changed for the
//     clients. The handler always returns false, so a console invocation is left to the engine and
//     a chat line is swallowed like pisexcompat's chat-command path did (Pisex).
//   * the order of the early-outs (and the original's `return`s inside the round_start loop,
//     which abort the whole loop - parity, they only fire for a VIP whose pawn is gone) is kept.
//
// Pure s2sdk (docs/PLUGIN_STANDARD.md §7): there is no pisexcompat and no s2guard. Every safety
// rule the module used to get from pisexcompat lives in this file (PITFALLS.md):
//   * B1/B2 - the map gate g_mapReady. The OnMapStart/OnMapEnd listeners are registered in
//     OnPluginStart, readiness is latched by OnMapStart and by the first OnPluginUpdate of a
//     plugin loaded into a running map (when no OnMapEnd was seen since OnPluginStart), and
//     dropped by OnMapEnd. No client, entity or schema call is made while the gate is closed:
//     s2sdk dereferences a torn-down client list in the OnMapEnd..OnMapStart window and a null
//     serializer map before the first map.
//   * C2 - IsConnected()/IsInGame()/IsAlive() layer the gate, the slot range 0..63,
//     IsClientConnected, IsClientInGame, !IsClientSourceTV, a controller entity (pointer and,
//     when the exports exist, PlayerSlotToEntHandle + IsValidEntHandle/IsValidEntPointer), a pawn
//     with a valid pointer and entity handle, and only then IsClientAlive (which dereferences the
//     pawn).
//   * C1 - an entity int is a CEntityHandle (index | serial << 15), so the sign means nothing: a
//     valid handle can be negative. Controller/pawn handles are checked against the 0 (world) and
//     -1 (INVALID_EHANDLE_INDEX) sentinels plus IsValidEntHandle(); PawnHandle() answers 0 for "no
//     usable pawn" and returns a valid handle unchanged, even when it is negative.
//   * C5/C8 - GetPlayerTeam() reads the controller's m_iTeamNum (1 byte) at a map-gated, cached
//     schema offset, what pisexcompat's GetPlayerTeam did; s2sdk's own GetClientTeam goes through
//     the pawn and warns for a client without one. SchemaOffset() answers -1 while the gate is
//     closed (GetSchemaOffset before a map exists segfaults inside the schema system) and caches
//     only a successful lookup (>= 0); the cache is cleared in OnPluginEnd.
//   * C3/C6 - m_bInBuyZone is read with the handle-based GetEntData() (the pawn is an entity),
//     while m_pItemServices/m_pInGameMoneyServices have no s2sdk helper and are component
//     pointers of a live entity, so they are read raw at cached offsets (8-byte pointer fields,
//     in the same call as the owner's validation).
//   * C12/A7 - the round_start hook, every command, the say/say_team listeners (when a bare chat
//     spelling needs them) and the map listeners are removed again in OnPluginEnd, so `plg reload`
//     leaves nothing that points into the unloaded library.
//   * A1 - OnPluginStart never returns Failed (that stops every Plugify plugin) and a missing
//     config only logs and leaves the module disabled.
//
// Port notes:
//   * CCSPlayerController::FromSlot + m_hPlayerPawn + IsAlive -> IsAlive();
//     m_iTeamNum -> GetPlayerTeam().
//   * m_iAccount -> s2sdk Get/SetClientMoney (the engine's own accessor).
//   * GiveNamedItem -> s2sdk::GiveNamedItem; SetStateChanged -> s2sdk::NetworkStateChanged on
//     the controller handle (the original called it explicitly, so the port does too).
//   * a missing config no longer aborts the plugin (the original's AllPluginsLoaded logged and
//     unloaded): the module logs it and stays disabled (no commands, no message).

#include <plg/plugin.hpp>
#include <plg/string.hpp>
#include <plg/vector.hpp>
#include <plugin_export.h>
#include <s2sdk/s2sdk.hpp>
#include <vip_api.hpp>

#include "btw_logic.hpp"
#include "kv1.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

// The original code's unqualified spellings (VIP_BOOL, HIDE, ...).
using namespace vip;

constexpr const char* kLogTag = "VIP-BTW"; // GetLogTag() of the original ("[VIP-BTW]")
constexpr const char* kFeature = "btw";
constexpr const char* kNoBuyZonePhrase = "btw_no_buyzone";
constexpr const char* kNoMoneyPhrase = "btw_no_money";
constexpr const char* kRoundStartPhrase = "btw_round_start";
constexpr const char* kRoundStartEvent = "round_start";
// <GetConfigsDir()>/vip_btw/vip_btw.ini - the plugin's own config dir - and the Metamod install's
// addons/configs/vip/vip_btw.ini, which package.sh/older deploys may still leave in place.
constexpr const char* kConfigPrimary = "vip_btw/vip_btw.ini";
constexpr const char* kConfigFallback = "vip/vip_btw.ini";

IVIPApi* g_pVIPCore = &IVIPApi::Get();

void LogInfo(std::string_view message) {
	if (plg::Log) {
		plg::Log(message, plg::Severity::Info, plg::source_location::current());
	}
}

void LogError(std::string_view message) {
	if (plg::Log) {
		plg::Log(message, plg::Severity::Error, plg::source_location::current());
	}
}

// The original's ErrorLog("[%s] ...", GetLogTag()).
void LogTaggedError(const std::string& message) {
	LogError(std::string("[") + kLogTag + "] " + message);
}

// --- PITFALLS B1/B2: the map gate -------------------------------------------------

bool g_mapReady = false;               // a map is up: every client/entity/schema call is allowed
bool g_mapStartSeen = false;           // OnMapStart arrived since OnPluginStart / the last OnMapEnd
bool g_mapEndSeen = false;             // OnMapEnd arrived since OnPluginStart
bool g_mapListenersRegistered = false; // OnMapStart/OnMapEnd are registered with s2sdk

// s2sdk listeners are plain function pointers called on the game thread. They must not throw and
// must not touch a client: OnMapEnd runs while the client list is being torn down (PITFALLS B2).

void OnMapStartListener() {
	// OnMapStart is the map's first live frame; the gate is open for everything that follows.
	// (A plugin loaded into a running map never sees this callback - PollMapGate() covers it.)
	g_mapReady = true;
	g_mapStartSeen = true;
	g_mapEndSeen = false;
}

void OnMapEndListener() {
	// From here until the next OnMapStart every s2sdk client query can crash (PITFALLS B2): only
	// state changes, no client call.
	g_mapReady = false;
	g_mapStartSeen = false;
	g_mapEndSeen = true;
}

void RegisterMapListeners() {
	if (g_mapListenersRegistered) {
		return;
	}
	// PITFALLS B1: registering the listeners touches no client, so it is safe in OnPluginStart.
	// Without them nothing could ever close the gate again, so a missing binding keeps it shut
	// (every client call stays refused) instead of opening it blindly (PITFALLS A1: log, never
	// return Failed).
	if (!__s2sdk_OnMapStart_Register || !__s2sdk_OnMapEnd_Register) {
		LogTaggedError("the s2sdk map listeners are unavailable - client calls stay disabled");
		return;
	}
	s2sdk::OnMapStart_Register(&OnMapStartListener);
	s2sdk::OnMapEnd_Register(&OnMapEndListener);
	g_mapListenersRegistered = true;
}

// The mid-map latch of the gate: a plugin loaded into a running map never saw OnMapStart, so its
// first update is the "a map is up" signal (the host pumps updates from the server frame). After
// an OnMapEnd only a new OnMapStart re-opens the gate: the host keeps pumping updates through a
// changelevel, so `!g_mapEndSeen` alone must not be enough once one was seen.
void PollMapGate() {
	if (!g_mapListenersRegistered) {
		return;
	}
	if (!g_mapReady && (g_mapStartSeen || !g_mapEndSeen)) {
		g_mapReady = true;
	}
}

void UnregisterMapListeners() {
	if (g_mapListenersRegistered) {
		g_mapListenersRegistered = false;
		if (__s2sdk_OnMapStart_Unregister) {
			s2sdk::OnMapStart_Unregister(&OnMapStartListener);
		}
		if (__s2sdk_OnMapEnd_Unregister) {
			s2sdk::OnMapEnd_Unregister(&OnMapEndListener);
		}
	}
	// A stale callback (the engine may still hold one for a moment) can no longer reach a client.
	g_mapReady = false;
	g_mapStartSeen = false;
	g_mapEndSeen = false;
}

bool IsMapReady() {
	return g_mapReady;
}

// --- PITFALLS C8: the schema offsets ----------------------------------------------

// The offsets of the fields this module touches; -1 = not resolved yet (or the field is missing).
// A failed lookup is retried, a successful one is kept until OnPluginEnd forgets it.
struct SchemaOffsets {
	int teamNum = -1;       // CBaseEntity::m_iTeamNum (1 byte)
	int inBuyZone = -1;     // CCSPlayerPawn::m_bInBuyZone (1 byte)
	int itemServices = -1;  // CBasePlayerPawn::m_pItemServices (pointer field, 8 bytes)
	int moneyServices = -1; // CCSPlayerController::m_pInGameMoneyServices (pointer field, 8 bytes)
};

SchemaOffsets g_schemaOffsets;

// The map-gated replacement of pisex::SchemaOffset (PITFALLS C8): resolving an offset before a map
// exists segfaults inside the schema system, so a closed gate answers -1 and the caller skips the
// work. Only an offset >= 0 is cached, so a field that is not available yet is retried.
int SchemaOffset(const char* szClassName, const char* szFieldName, int& iCachedOffset) {
	if (!IsMapReady() || __s2sdk_GetSchemaOffset == nullptr) {
		return -1;
	}
	if (iCachedOffset < 0) {
		try {
			iCachedOffset = static_cast<int>(
				s2sdk::GetSchemaOffset(plg::string(szClassName), plg::string(szFieldName)));
		} catch (...) {
			iCachedOffset = -1; // retried on the next call
		}
	}
	return iCachedOffset;
}

// Called from OnPluginEnd: a reload resolves the fields again instead of reusing offsets a previous
// instance happened to have found (PITFALLS C8/A7).
void ResetSchemaOffsets() {
	g_schemaOffsets = SchemaOffsets{};
}

// --- PITFALLS C2: the client checks -----------------------------------------------

bool IsSlotInRange(int32_t iSlot) {
	return iSlot >= 0 && iSlot < vip_btw::kMaxSlots;
}

// A connected slot while a map is up (the first layer of the Pisex client guard).
bool IsConnected(int32_t iSlot) {
	if (!IsMapReady() || !IsSlotInRange(iSlot) || __s2sdk_IsClientConnected == nullptr) {
		return false;
	}
	try {
		return s2sdk::IsClientConnected(iSlot);
	} catch (...) {
		return false;
	}
}

// The client is in the game and is not the SourceTV bot (PITFALLS C2: connected -> in game ->
// not SourceTV). Bots are real clients and pass.
bool IsInGame(int32_t iSlot) {
	if (!IsConnected(iSlot) || __s2sdk_IsClientInGame == nullptr || __s2sdk_IsClientSourceTV == nullptr) {
		return false;
	}
	try {
		return s2sdk::IsClientInGame(iSlot) && !s2sdk::IsClientSourceTV(iSlot);
	} catch (...) {
		return false;
	}
}

// PITFALLS C1: an entity int is a CEntityHandle (index | serial << 15), not an entity index.
// -1 is INVALID_EHANDLE_INDEX and 0 is the world ("no entity"); the sign means nothing - a valid
// handle becomes negative once the serial passes 0xFFFF - so only these two sentinels and
// IsValidEntHandle decide. Every controller/pawn handle check goes through this; callers check that
// __s2sdk_IsValidEntHandle is available first.
bool IsUsableEntHandle(int32_t iHandle) {
	return iHandle != -1 && iHandle != 0 && s2sdk::IsValidEntHandle(iHandle);
}

// The slot's controller entity, validated in the same call as the read that follows it: s2sdk
// never checks its arguments (PITFALLS C2), and the schema exports dereference the pointer/handle.
// nullptr = no usable controller (the caller skips its work). The handle is validated only when
// the exports are present, so a host without them keeps working on the pointer check alone.
void* ControllerPointer(int32_t iSlot) {
	if (!IsMapReady() || !IsSlotInRange(iSlot) || __s2sdk_PlayerSlotToEntPointer == nullptr) {
		return nullptr;
	}
	try {
		void* controller = s2sdk::PlayerSlotToEntPointer(iSlot);
		if (controller == nullptr) {
			return nullptr;
		}
		if (__s2sdk_PlayerSlotToEntHandle && __s2sdk_IsValidEntHandle) {
			// PITFALLS C1: no sign test - a valid handle can be negative.
			if (!IsUsableEntHandle(s2sdk::PlayerSlotToEntHandle(iSlot))) {
				return nullptr;
			}
		}
		if (__s2sdk_IsValidEntPointer && !s2sdk::IsValidEntPointer(controller)) {
			return nullptr;
		}
		return controller;
	} catch (...) {
		return nullptr;
	}
}

// The slot's current pawn pointer, validated (PITFALLS C2). The controller is checked first:
// s2sdk's GetClientPawn prints "invalid player slot: N" for a slot without one.
void* PawnPointer(int32_t iSlot) {
	if (!IsInGame(iSlot) || ControllerPointer(iSlot) == nullptr || __s2sdk_GetClientPawn == nullptr) {
		return nullptr;
	}
	try {
		void* pawn = s2sdk::GetClientPawn(iSlot);
		if (pawn == nullptr) {
			return nullptr;
		}
		if (__s2sdk_IsValidEntPointer && !s2sdk::IsValidEntPointer(pawn)) {
			return nullptr;
		}
		return pawn;
	} catch (...) {
		return nullptr;
	}
}

// The pawn's entity handle (CEntityHandle, PITFALLS C1), validated with IsValidEntHandle and
// IsValidEntPointer. 0 = no usable pawn (the caller skips its work; 0 is the world and never a
// pawn). A valid handle is returned unchanged even when it is negative: the handle is what the
// handle-based schema exports (GetEntData) take, so the pawn pointer never outlives the call that
// validated it.
int32_t PawnHandle(int32_t iSlot) {
	void* pawn = PawnPointer(iSlot);
	if (pawn == nullptr || __s2sdk_EntPointerToEntHandle == nullptr ||
		__s2sdk_IsValidEntHandle == nullptr) {
		return 0;
	}
	try {
		const int32_t iHandle = s2sdk::EntPointerToEntHandle(pawn);
		if (!IsUsableEntHandle(iHandle)) {
			return 0;
		}
		return iHandle;
	} catch (...) {
		return 0;
	}
}

// The original's CCSPlayerController::FromSlot + m_hPlayerPawn + IsAlive chain: a connected,
// in-game, non-SourceTV client whose controller exists and whose pawn is valid and alive.
// s2sdk::IsClientAlive dereferences the pawn, so it is asked last.
bool IsAlive(int32_t iSlot) {
	if (!IsInGame(iSlot) || __s2sdk_IsClientAlive == nullptr) {
		return false;
	}
	if (PawnHandle(iSlot) == 0) {
		return false;
	}
	try {
		return s2sdk::IsClientAlive(iSlot);
	} catch (...) {
		return false;
	}
}

// The controller's team: CBaseEntity::m_iTeamNum, 1 byte (PITFALLS C5), read at the cached offset.
// 0 when the map is not up, the slot has no controller or the offset is unavailable (0 also means
// "unassigned", which the caller rejects).
int GetPlayerTeam(int32_t iSlot) {
	if (!IsSlotInRange(iSlot) || __s2sdk_GetEntData2 == nullptr) {
		return 0;
	}
	const int iOffset = SchemaOffset("CBaseEntity", "m_iTeamNum", g_schemaOffsets.teamNum);
	if (iOffset < 0) {
		return 0;
	}
	void* controller = ControllerPointer(iSlot);
	if (controller == nullptr) {
		return 0;
	}
	try {
		// The mask only protects against a host that sign-extends the byte.
		return static_cast<int>(s2sdk::GetEntData2(controller, iOffset, 1) & 0xFF);
	} catch (...) {
		return 0;
	}
}

// The buy-zone flag of the pawn (the original's m_bInBuyZone). The pawn is an entity, so the
// handle-based export is used (PITFALLS C3); the handle was validated in the same call.
bool InBuyZone(int32_t iSlot) {
	if (__s2sdk_GetEntData == nullptr) {
		return false;
	}
	const int32_t iHandle = PawnHandle(iSlot);
	if (iHandle == 0) {
		return false;
	}
	const int iOffset = SchemaOffset("CCSPlayerPawn", "m_bInBuyZone", g_schemaOffsets.inBuyZone);
	if (iOffset < 0) {
		return false;
	}
	try {
		return s2sdk::GetEntData(iHandle, iOffset, 1) != 0;
	} catch (...) {
		return false;
	}
}

// The value of a pointer-sized component field of `owner` at a cached offset. PITFALLS C4: the
// field stores the component address; the address of the field is not the component.
void* ComponentAt(void* owner, int iOffset) {
	if (owner == nullptr || iOffset < 0) {
		return nullptr;
	}
	return *reinterpret_cast<void**>(static_cast<char*>(owner) + iOffset);
}

// The pawn's item services (the original only null-checked the pointer). A component object of
// the live pawn, hence the raw pointer path (PITFALLS C6).
void* ItemServices(int32_t iSlot) {
	void* pawn = PawnPointer(iSlot);
	if (pawn == nullptr) {
		return nullptr;
	}
	const int iOffset =
		SchemaOffset("CBasePlayerPawn", "m_pItemServices", g_schemaOffsets.itemServices);
	if (iOffset < 0) {
		return nullptr;
	}
	try {
		return ComponentAt(pawn, iOffset);
	} catch (...) {
		return nullptr;
	}
}

// The controller's in-game money services (the original only null-checked the pointer).
void* MoneyServices(int32_t iSlot) {
	void* controller = ControllerPointer(iSlot);
	if (controller == nullptr) {
		return nullptr;
	}
	const int iOffset = SchemaOffset("CCSPlayerController", "m_pInGameMoneyServices",
									 g_schemaOffsets.moneyServices);
	if (iOffset < 0) {
		return nullptr;
	}
	try {
		return ComponentAt(controller, iOffset);
	} catch (...) {
		return nullptr;
	}
}

// --- output ----------------------------------------------------------------------

// cs2-menus/utils.h colors_text[] / colors_hex[], the same byte table pisexcompat (and cs2-menus)
// used; s2sdk's PrintToChat sends the text as it is and its PrintToChatColored has a different tag
// table, so the phrases are converted here.
struct ColorTag {
	std::string_view tag;
	std::string_view bytes;
};

constexpr std::array<ColorTag, 17> kColorTags = {{{"{DEFAULT}", "\x01"},
												  {"{WHITE}", "\x01"},
												  {"{RED}", "\x02"},
												  {"{LIGHTPURPLE}", "\x03"},
												  {"{GREEN}", "\x04"},
												  {"{LIME}", "\x05"},
												  {"{LIGHTGREEN}", "\x06"},
												  {"{DARKRED}", "\x07"},
												  {"{GRAY}", "\x08"},
												  {"{LIGHTOLIVE}", "\x09"},
												  {"{OLIVE}", "\x10"},
												  {"{LIGHTBLUE}", "\x0B"},
												  {"{BLUE}", "\x0C"},
												  {"{PURPLE}", "\x0E"},
												  {"{LIGHTRED}", "\x0F"},
												  {"{GRAYBLUE}", "\x0A"},
												  {"\\n", "\xe2\x80\xa9"}}};

std::string Colorize(std::string_view text) {
	std::string out(text);
	for (const ColorTag& color : kColorTags) {
		std::size_t pos = 0;
		while ((pos = out.find(color.tag, pos)) != std::string::npos) {
			out.replace(pos, color.tag.size(), color.bytes);
			pos += color.bytes.size();
		}
	}
	return out;
}

// The original's pisex::FormatString: a phrase from vip.phrases.txt used as the printf format (a
// phrase containing "%%" prints "%"). Deliberately no printf attribute: the phrases are data, not
// format literals to check.
std::string FormatString(const char* format, ...) {
	std::array<char, 2048> buffer{};
	va_list args;
	va_start(args, format);
	const int written = std::vsnprintf(buffer.data(), buffer.size(), format ? format : "", args);
	va_end(args);
	if (written <= 0) {
		return {};
	}
	const std::size_t size = std::min(static_cast<std::size_t>(written), buffer.size() - 1);
	return std::string(buffer.data(), size);
}

// Chat output behind IsConnected(): no client query while the map gate is closed and no call on a
// slot without a client (PITFALLS B2/C2). The colour tags of the phrase files are converted here.
void PrintToChat(int32_t iSlot, std::string_view text) {
	if (!IsConnected(iSlot) || __s2sdk_PrintToChat == nullptr) {
		return;
	}
	try {
		const std::string colored = Colorize(text);
		s2sdk::PrintToChat(iSlot, plg::string(colored.data(), colored.size()));
	} catch (...) {
	}
}

// --- the plugin ------------------------------------------------------------------

// The round_start hook of the original's AllPluginsLoaded; the flag lets OnPluginEnd unhook the
// exact callback it registered (PITFALLS C12/A7).
bool g_roundStartHooked = false;

class BuyTeamWeaponPlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override;
	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds dt) override;
	plg::PluginResult OnPluginEnd() override;

	// VIP_BuyWeapon of the original (the RegCommand handler).
	bool BuyWeapon(int32_t iSlot, const char* szContent);
	// OnRoundStart of the original (the "round_start" hook).
	void OnRoundStart();

	// The loaded state, exposed for the engine-free test (no other plugin consumes vip_btw).
	bool IsLoaded() const { return m_loaded; }
	const vip_btw::WeaponTable& GetWeaponTable(int iTable) const { return m_weaponData[iTable]; }

private:
	void LoadConfig();
	void LoadSection(const vip_btw::KvNode& kv, const char* szSection, int iTable);

	// g_WeaponData of the original: [0] = the "T" section, [1] = the "CT" section.
	vip_btw::WeaponTable m_weaponData[2];
	bool m_loaded = false;
};

BuyTeamWeaponPlugin g_plugin;

s2sdk::ResultType OnRoundStartEvent(const plg::string& /*szName*/, void* /*pEvent*/,
									bool /*bDontBroadcast*/) {
	try {
		g_plugin.OnRoundStart();
	} catch (const std::exception& ex) {
		LogTaggedError(std::string("round_start handler failed: ") + ex.what());
	} catch (...) {
		LogTaggedError("round_start handler failed with an unknown exception");
	}
	return s2sdk::ResultType::Continue;
}

// VIP_BuyWeapon handed to the command dispatcher below.
bool InvokeBuy(int32_t iSlot, const char* szContent) {
	return g_plugin.BuyWeapon(iSlot, szContent);
}

// --- commands (console + chat, self-contained) ------------------------------------

// The module is pure s2sdk, so the console/chat registration pisex::RegCommand used to provide is
// implemented here:
//   * a console spelling ("mm_<name>") is an s2sdk ConCommand reachable from the console only;
//   * a chat spelling ("!<name>") is registered under its trigger-less name ("<name>"), because
//     s2sdk chains "!<name>"/"/<name>" to that ConCommand and marks the context Chat. A chat
//     spelling WITHOUT a leading "!"/"/" is additionally dispatched from the "say"/"say_team"
//     listeners, exactly like pisexcompat did for bare chat words;
//   * every name is created with ConVarFlag::Release | ConVarFlag::ClientCanExecute (a client
//     console and the chat path both reach it), and the source filter keeps a console name from
//     answering on the chat path and vice versa;
//   * OnPluginEnd detaches every registration: RemoveCommand for the names this plugin created,
//     RemoveCommandListener for a name that already existed, and RemoveCommandListener for
//     say/say_team.

using CommandHandler = bool (*)(int32_t iSlot, const char* szContent);

struct CommandEntry {
	std::string name;     // lookup key: lowercase, chat trigger stripped
	std::string spelling; // what the handler sees as arg[0]
	bool console = false;
	bool chat = false;
	CommandHandler handler = nullptr;
};

struct S2CommandName {
	std::string name;
	bool owned = false; // AddConsoleCommand created it; false = we only listen on an existing one
};

std::vector<CommandEntry> g_commands;
std::vector<S2CommandName> g_s2Names;
bool g_sayHooked = false;

std::string ToLower(std::string_view text) {
	std::string out(text);
	std::transform(out.begin(), out.end(), out.begin(),
				   [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
	return out;
}

// s2sdk hands the callback the engine's already-tokenized arguments; the handler of the original
// expects the command content (the registered spelling followed by the arguments).
std::string JoinCommandContent(std::string_view spelling, const plg::vector<plg::string>& args) {
	std::string out(spelling);
	for (std::size_t i = 1; i < args.size(); ++i) {
		out.push_back(' ');
		out.append(args[i].data(), args[i].size());
	}
	return out;
}

// The most recently registered match wins, like pisexcompat's FindEntry().
const CommandEntry* FindEntry(std::string_view name, bool forChat) {
	const std::string lowered = ToLower(name);
	for (auto it = g_commands.rbegin(); it != g_commands.rend(); ++it) {
		if (it->name == lowered && (forChat ? it->chat : it->console)) {
			return &(*it);
		}
	}
	return nullptr;
}

// The trigger-less spelling of a chat name: s2sdk only chains a say message as a command when it
// starts with a configured trigger, so these are dispatched by BareChatTrampoline instead.
const CommandEntry* FindBareChatEntry(std::string_view name) {
	const std::string lowered = ToLower(name);
	for (auto it = g_commands.rbegin(); it != g_commands.rend(); ++it) {
		if (!it->chat || it->name != lowered || it->spelling.empty() ||
			vip_btw::IsChatTrigger(it->spelling.front())) {
			continue;
		}
		return &(*it);
	}
	return nullptr;
}

// Runs the handler and translates the result for s2sdk. Pisex swallowed every chat line whose
// first word matched a registered chat name, no matter what the handler returned; a console
// command keeps its old "false = let the engine continue" behaviour.
s2sdk::ResultType RunCommand(int32_t caller, const CommandEntry& entry,
							 const plg::vector<plg::string>& args, bool fromChat) {
	const std::string content = JoinCommandContent(entry.spelling, args);
	const CommandHandler handler = entry.handler; // copy: the callback may unregister itself
	const bool handled = handler != nullptr ? handler(caller, content.c_str()) : false;
	return (handled || fromChat) ? s2sdk::ResultType::Handled : s2sdk::ResultType::Continue;
}

s2sdk::ResultType CommandTrampoline(int32_t caller, s2sdk::ConCommandContext context,
									const plg::vector<plg::string>& args) {
	try {
		if (args.empty() || args[0].empty()) {
			return s2sdk::ResultType::Continue;
		}
		const bool fromChat = context == s2sdk::ConCommandContext::Chat;
		const CommandEntry* entry = FindEntry(std::string_view(args[0].data(), args[0].size()), fromChat);
		if (entry == nullptr) {
			// A chat message that does not belong to any registered chat name (for example
			// "!something") is left alone: s2sdk then falls back to the normal say path.
			return s2sdk::ResultType::Continue;
		}
		return RunCommand(caller, *entry, args, fromChat);
	} catch (const std::exception& ex) {
		LogTaggedError(std::string("command handler threw: ") + ex.what());
	} catch (...) {
		LogTaggedError("command handler threw an unknown exception");
	}
	// Never propagate: a throwing handler still owns the command.
	return s2sdk::ResultType::Handled;
}

// s2sdk delivers every "say"/"say_team" line here. A first token that is a registered
// trigger-less chat name is dispatched like a chat command and removed from the chat.
s2sdk::ResultType BareChatTrampoline(int32_t caller, s2sdk::ConCommandContext /*context*/,
									 const plg::vector<plg::string>& args) {
	try {
		if (args.size() < 2 || g_commands.empty()) {
			return s2sdk::ResultType::Continue;
		}
		// A CS2 client sends `say "ak47"` as ONE quoted argument, so args is (say, "ak47 top"): the
		// message is split into words; the multi-argument shape already carries one token each.
		std::vector<std::string> tokens;
		if (args.size() == 2) {
			tokens = vip_btw::SplitStringBySpace(std::string_view(args[1].data(), args[1].size()));
		} else {
			tokens.emplace_back(args[1].data(), args[1].size());
			for (std::size_t i = 2; i < args.size(); ++i) {
				tokens.emplace_back(args[i].data(), args[i].size());
			}
		}
		if (tokens.empty() || tokens.front().empty() ||
			vip_btw::IsChatTrigger(tokens.front().front())) {
			return s2sdk::ResultType::Continue;
		}
		const CommandEntry* entry = FindBareChatEntry(tokens.front());
		if (entry == nullptr) {
			return s2sdk::ResultType::Continue;
		}
		// tokens[0] is the command name, the rest are its arguments - the shape RunCommand() wants.
		plg::vector<plg::string> commandArgs;
		commandArgs.reserve(tokens.size());
		for (const std::string& token : tokens) {
			commandArgs.push_back(plg::string(token.data(), token.size()));
		}
		return RunCommand(caller, *entry, commandArgs, /*fromChat=*/true);
	} catch (const std::exception& ex) {
		LogTaggedError(std::string("bare chat handler threw: ") + ex.what());
	} catch (...) {
		LogTaggedError("bare chat handler threw an unknown exception");
	}
	// Never propagate: a throwing handler still owns the chat line.
	return s2sdk::ResultType::Handled;
}

// One say/say_team pre listener serves every trigger-less chat name; it is installed with the
// first such registration.
void EnsureSayListener() {
	if (g_sayHooked) {
		return;
	}
	if (__s2sdk_AddCommandListener == nullptr) {
		LogTaggedError("AddCommandListener is unavailable - the bare chat name is disabled");
		return;
	}
	for (const char* szSay : {"say", "say_team"}) {
		if (!s2sdk::AddCommandListener(plg::string(szSay), &BareChatTrampoline, s2sdk::HookMode::Pre)) {
			LogTaggedError(std::string("could not listen on '") + szSay + "'");
		}
	}
	g_sayHooked = true;
}

// Registers the s2sdk command name once. AddConsoleCommand returns false when the name already
// exists (engine or another plugin): s2sdk attached our callback as a listener then, so
// RemoveCommands() must detach only that listener instead of removing the command.
bool EnsureS2Name(const std::string& name) {
	for (const S2CommandName& known : g_s2Names) {
		if (known.name == name) {
			return true;
		}
	}
	if (__s2sdk_AddConsoleCommand == nullptr) {
		LogTaggedError(std::string("AddConsoleCommand is unavailable - '") + name + "' is not registered");
		return false;
	}
	// Our commands are client-executable: "mm_<name>" is typed in a client console too, and the
	// chat path ignores the flags anyway (the trampoline filters by source).
	const int64_t flagBits = static_cast<int64_t>(s2sdk::ConVarFlag::Release) |
							 static_cast<int64_t>(s2sdk::ConVarFlag::ClientCanExecute);
	const bool owned = s2sdk::AddConsoleCommand(plg::string(name.data(), name.size()),
												plg::string("vip_btw"),
												static_cast<s2sdk::ConVarFlag>(flagBits),
												&CommandTrampoline, s2sdk::HookMode::Pre);
	g_s2Names.push_back({name, owned});
	return true;
}

// The self-contained replacement of pisex::RegCommand({szConsole}, {szChat}, ...): the console
// spelling and the chat spelling of one buy command are registered for their own sources.
void RegisterCommand(std::string_view szConsole, std::string_view szChat, CommandHandler handler) {
	try {
		if (handler == nullptr) {
			return;
		}
		const std::string console(szConsole);
		if (!console.empty()) {
			const std::string lowered = ToLower(console);
			g_commands.push_back({lowered, console, /*console=*/true, /*chat=*/false, handler});
			EnsureS2Name(lowered);
		}
		const std::string chat(szChat);
		if (chat.empty()) {
			return;
		}
		// s2sdk reaches a triggered chat message through the trigger-less command name.
		const std::string stripped = vip_btw::StripChatTrigger(chat);
		if (stripped.empty()) {
			return;
		}
		const std::string lowered = ToLower(stripped);
		g_commands.push_back({lowered, chat, /*console=*/false, /*chat=*/true, handler});
		EnsureS2Name(lowered);
		if (!vip_btw::IsChatTrigger(chat.front())) {
			// No "!"/"/": s2sdk never chains this spelling, so the say listeners dispatch it.
			EnsureSayListener();
		}
	} catch (const std::exception& ex) {
		LogTaggedError(std::string("RegisterCommand failed: ") + ex.what());
	} catch (...) {
		LogTaggedError("RegisterCommand failed with an unknown exception");
	}
}

// OnPluginEnd: every registration of this library is removed again (PITFALLS A7/C12).
void RemoveCommands() {
	for (const S2CommandName& known : g_s2Names) {
		if (known.owned) {
			if (__s2sdk_RemoveCommand) {
				s2sdk::RemoveCommand(plg::string(known.name.data(), known.name.size()),
									 &CommandTrampoline);
			}
		} else if (__s2sdk_RemoveCommandListener) {
			s2sdk::RemoveCommandListener(plg::string(known.name.data(), known.name.size()),
										 &CommandTrampoline, s2sdk::HookMode::Pre);
		}
	}
	if (g_sayHooked && __s2sdk_RemoveCommandListener) {
		for (const char* szSay : {"say", "say_team"}) {
			s2sdk::RemoveCommandListener(plg::string(szSay), &BareChatTrampoline,
										 s2sdk::HookMode::Pre);
		}
	}
	g_sayHooked = false;
	g_s2Names.clear();
	g_commands.clear();
}

// --- buying ----------------------------------------------------------------------

bool BuyTeamWeaponPlugin::BuyWeapon(int32_t iSlot, const char* szContent) {
	// The server console (caller -1) and a bad caller cannot be a VIP: PITFALLS C2, the slot is
	// validated before the VIP core or any s2sdk client export sees it.
	if (!IsSlotInRange(iSlot)) {
		return false;
	}
	// The original tokenized the command content and ignored an empty argument list.
	const std::vector<std::string> args = vip_btw::SplitStringBySpace(szContent ? szContent : "");
	if (args.empty()) {
		return false;
	}
	const std::string sCommand = vip_btw::CleanCommand(args[0]);

	// A command that is in neither weapon table is ignored (the original's lookup).
	if (!m_weaponData[0].contains(sCommand) && !m_weaponData[1].contains(sCommand)) {
		return false;
	}
	// Only a VIP with the "btw" feature can buy (the original's VIP checks).
	if (!g_pVIPCore->VIP_IsClientVIP(iSlot) || !g_pVIPCore->VIP_GetClientFeatureBool(iSlot, kFeature)) {
		return false;
	}
	// CCSPlayerController + GetPlayerPawn + IsAlive (PITFALLS C2 layers inside IsAlive).
	if (!IsAlive(iSlot)) {
		return false;
	}
	const int iTeamNum = vip_btw::TeamIndex(GetPlayerTeam(iSlot));
	if (!m_weaponData[iTeamNum].contains(sCommand)) {
		return false;
	}

	// Outside the buy zone the original printed "btw_no_buyzone" and refused the purchase.
	if (!InBuyZone(iSlot)) {
		PrintToChat(iSlot, g_pVIPCore->VIP_GetTranslate(kNoBuyZonePhrase));
		return false;
	}

	// The original refused the purchase when the item services were missing.
	if (ItemServices(iSlot) == nullptr) {
		return false;
	}
	// ... and when the money services were missing.
	if (MoneyServices(iSlot) == nullptr) {
		return false;
	}

	const vip_btw::WeaponData& wData = m_weaponData[iTeamNum][sCommand];
	// Not enough money: the original printed "btw_no_money" and refused the purchase.
	if (!__s2sdk_GetClientMoney || !__s2sdk_SetClientMoney) {
		return false;
	}
	const int iAccount = s2sdk::GetClientMoney(iSlot);
	if (iAccount < wData.iPrice) {
		PrintToChat(iSlot, g_pVIPCore->VIP_GetTranslate(kNoMoneyPhrase));
		return false;
	}

	// The original subtracted the price and gave the weapon.
	s2sdk::SetClientMoney(iSlot, iAccount - wData.iPrice);
	if (__s2sdk_GiveNamedItem) {
		try {
			s2sdk::GiveNamedItem(iSlot, plg::string(wData.szName));
		} catch (...) {
		}
	}
	// The original's state change on the money-services field of the controller: the controller is
	// an entity, so the handle-based call is used.
	if (__s2sdk_NetworkStateChanged && __s2sdk_PlayerSlotToEntHandle && __s2sdk_IsValidEntHandle) {
		try {
			const int32_t iHandle = s2sdk::PlayerSlotToEntHandle(iSlot);
			if (IsUsableEntHandle(iHandle)) {
				s2sdk::NetworkStateChanged(iHandle, "CCSPlayerController", "m_pInGameMoneyServices");
			}
		} catch (...) {
		}
	}
	return false;
}

// --- round start -----------------------------------------------------------------

void BuyTeamWeaponPlugin::OnRoundStart() {
	if (!m_loaded) {
		return;
	}
	for (int32_t i = 0; i < vip_btw::kMaxSlots; ++i) {
		if (!g_pVIPCore->VIP_IsClientVIP(i) || !g_pVIPCore->VIP_GetClientFeatureBool(i, kFeature)) {
			continue;
		}
		// The original's controller lookup; a VIP without one is skipped (a VIP in spectate,
		// still connecting or dead at round start - the original ended the whole loop there,
		// which silently cut the message off for every VIP with a higher slot).
		if (!IsInGame(i)) {
			continue;
		}
		// A missing or dead pawn is skipped for the same reason (the original's IsAlive check).
		if (!IsAlive(i)) {
			continue;
		}
		// Only the two team tables exist; a spectator (team - 2 < 0) has no list to print.
		const int teamIndex = GetPlayerTeam(i) - 2;
		if (teamIndex < 0 || teamIndex >= 2) {
			continue;
		}
		const std::string szWeapons = vip_btw::JoinWeapons(m_weaponData[teamIndex]);
		// The original printed the round-start phrase, formatted with the weapon list.
		const std::string text =
			FormatString(g_pVIPCore->VIP_GetTranslate(kRoundStartPhrase), szWeapons.c_str());
		PrintToChat(i, text);
	}
}

// --- config / lifecycle -----------------------------------------------------------

void BuyTeamWeaponPlugin::LoadSection(const vip_btw::KvNode& kv, const char* szSection, int iTable) {
	const vip_btw::KvNode* hSection = kv.Find(szSection);
	if (hSection == nullptr) {
		return;
	}
	for (const vip_btw::KvNode& value : hSection->children) {
		const std::string szCommand = value.name;
		const int iPrice = value.GetInt("price");
		const std::string szWeapon(value.GetString("weapon"));
		m_weaponData[iTable][szCommand] = {szWeapon, iPrice};

		// The original formatted the chat "!<name>" and console "mm_<name>" command names.
		const std::string szChat = vip_btw::ChatCommandName(szCommand);
		const std::string szConsole = vip_btw::ConsoleCommandName(szCommand);
		// The original registered both names (console + chat) to the buy handler.
		RegisterCommand(szConsole, szChat, &InvokeBuy);
	}
}

// <plg::GetConfigsDir()>/<relative>; "" when the host provides no configs dir.
std::string ConfigPath(std::string_view relative) {
	if (!plg::GetConfigsDir) {
		return {};
	}
	std::string path(plg::GetConfigsDir());
	if (path.empty()) {
		return {};
	}
	if (path.back() != '/') {
		path.push_back('/');
	}
	path.append(relative);
	return path;
}

void BuyTeamWeaponPlugin::LoadConfig() {
	m_loaded = false;
	m_weaponData[0].clear();
	m_weaponData[1].clear();

	const std::string primary = ConfigPath(kConfigPrimary);
	const std::string fallback = ConfigPath(kConfigFallback);
	if (primary.empty()) {
		LogTaggedError("the Plugify configs directory is unavailable - the module stays disabled");
		return;
	}
	for (const std::string* path : {&primary, &fallback}) {
		const std::optional<vip_btw::KvNode> kv = vip_btw::LoadKv1File(*path);
		if (!kv) {
			continue;
		}
		// The original's LoadConfig: the "CT" section fills g_WeaponData[1], the "T" section [0].
		LoadSection(*kv, "CT", 1);
		LoadSection(*kv, "T", 0);
		m_loaded = true;
		if (path != &primary) {
			LogInfo(std::string("[") + kLogTag + "] using the fallback config " + *path);
		}
		return;
	}
	LogTaggedError("failed to load " + primary + " (or " + fallback +
				   ") - the module stays disabled (no buy commands, no round-start message)");
}

plg::PluginResult BuyTeamWeaponPlugin::OnPluginStart() {
	try {
		// PITFALLS B1: the map listeners are the only state prepared here - no client, entity or
		// schema call is allowed before the first map. A reload starts from a closed gate and
		// latches readiness again from OnMapStart or from its first update when it was loaded into
		// a running map.
		g_mapReady = false;
		g_mapStartSeen = false;
		g_mapEndSeen = false;
		RegisterMapListeners();

		LoadConfig();

		// The original hooked "round_start" in AllPluginsLoaded (PITFALLS C12: unhooked in
		// OnPluginEnd).
		if (__s2sdk_HookEvent != nullptr) {
			const s2sdk::EventHookError result = s2sdk::HookEvent(
				plg::string(kRoundStartEvent), &OnRoundStartEvent, s2sdk::HookMode::Pre);
			g_roundStartHooked = result == s2sdk::EventHookError::Okay;
			if (!g_roundStartHooked) {
				LogTaggedError("could not hook round_start");
			}
		} else {
			LogTaggedError("the s2sdk event bindings are unavailable - round_start stays unhooked");
		}

		// The original registered the "btw" feature (bool, hidden) in AllPluginsLoaded.
		g_pVIPCore->VIP_RegisterFeature(kFeature, VIP_BOOL, HIDE);

		LogInfo(std::string("[") + kLogTag + "] ready");
	} catch (const std::exception& ex) {
		LogTaggedError(std::string("OnPluginStart failed: ") + ex.what());
	} catch (...) {
		LogTaggedError("OnPluginStart failed with an unknown exception");
	}
	return {}; // never Failed: that stops every plugify plugin
}

plg::PluginResult BuyTeamWeaponPlugin::OnPluginUpdate(std::chrono::milliseconds /*dt*/) {
	try {
		// PITFALLS B1: a plugin loaded into a running map never saw OnMapStart, so its first
		// update is the "a map is up" signal; after an OnMapEnd only a new OnMapStart re-opens it.
		PollMapGate();
	} catch (const std::exception& ex) {
		LogTaggedError(std::string("OnPluginUpdate failed: ") + ex.what());
	} catch (...) {
		LogTaggedError("OnPluginUpdate failed with an unknown exception");
	}
	return {};
}

plg::PluginResult BuyTeamWeaponPlugin::OnPluginEnd() {
	// PITFALLS A7/C12: everything this library registered is removed again, so `plg reload`
	// leaves nothing that points into the unloaded .so.
	try {
		if (g_roundStartHooked && __s2sdk_UnhookEvent) {
			s2sdk::UnhookEvent(plg::string(kRoundStartEvent), &OnRoundStartEvent,
							   s2sdk::HookMode::Pre);
		}
	} catch (...) {
		// nothing left to do at unload
	}
	g_roundStartHooked = false;
	try {
		RemoveCommands();
	} catch (...) {
	}
	try {
		UnregisterMapListeners();
	} catch (...) {
	}
	// PITFALLS C8: a reload resolves the offsets itself instead of reusing this instance's.
	ResetSchemaOffsets();
	m_loaded = false;
	return {};
}

} // namespace

PLUGIFY_PLUGIN(PLUGIN_API, &g_plugin)
