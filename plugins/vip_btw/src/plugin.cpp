// vip_btw — Plugify port of the Pisex VIP module "VIP_BuyTeamWeapon" / "[VIP] Buy Team Weapon"
// (the original's vip_btw.cpp, 308 lines).
//
// Behaviour kept identical:
//   * config <configs>/vip_btw/vip_btw.ini (the original's addons/configs/vip/vip_btw.ini), the
//     "VIP" KeyValues file with a "CT" and a "T" section; every subkey is a command name with
//     "price" and "weapon".
//   * every weapon registers two commands: the console "mm_<name>" and the chat "!<name>"
//     (pisex::RegCommand strips the chat trigger and registers the bare name, like Pisex).
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
//     clients. The handler always returns false, so the chat line is still broadcast (Pisex).
//   * the order of the early-outs (and the original's `return`s inside the round_start loop,
//     which abort the whole loop - parity, they only fire for a VIP whose pawn is gone) is kept.
//
// Port notes:
//   * CCSPlayerController::FromSlot + m_hPlayerPawn + IsAlive -> pisex::IsInGame/pisex::IsAlive;
//     m_iTeamNum -> pisex::GetPlayerTeam.
//   * m_iAccount -> s2sdk Get/SetClientMoney (the engine's own accessor).
//   * GiveNamedItem -> s2sdk::GiveNamedItem; SetStateChanged -> s2sdk::NetworkStateChanged2 on
//     the raw controller pointer (the original called it explicitly, so the port does too).
//   * a missing config no longer aborts the plugin (the original's AllPluginsLoaded logged and
//     unloaded): the module logs it and stays disabled (no commands, no message).

#include <pisex/pisex.hpp>
#include <plg/plugin.hpp>
#include <plugin_export.h>
#include <s2sdk/s2sdk.hpp>
#include <vip_api.hpp>

#include "btw_logic.hpp"

#include <chrono>
#include <cstdio>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>
#include <vector>

namespace {

// The original code's unqualified spellings (VIP_BOOL, HIDE, ...).
using namespace vip;

constexpr int kMaxSlots = 64;
constexpr const char* kLogTag = "VIP-BTW"; // GetLogTag() of the original ("[VIP-BTW]")
constexpr const char* kConfigFile = "vip_btw.ini";
constexpr const char* kFeature = "btw";
constexpr const char* kNoBuyZonePhrase = "btw_no_buyzone";
constexpr const char* kNoMoneyPhrase = "btw_no_money";
constexpr const char* kRoundStartPhrase = "btw_round_start";

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

// --- raw entity access -------------------------------------------------------

// The raw pawn pointer of a slot (the original's m_hPlayerPawn).
void* PawnPointer(int iSlot) {
	// s2sdk's GetClientPawn warns "invalid player slot: N" (to the server console) for every call
	// on a slot without a controller, so an empty slot is filtered out here.
	if (!__s2sdk_GetClientPawn || iSlot < 0 || iSlot >= kMaxSlots || !pisex::IsConnected(iSlot)) {
		return nullptr;
	}
	try {
		return s2sdk::GetClientPawn(iSlot);
	} catch (...) {
		return nullptr;
	}
}

// The raw controller pointer of a slot (the original's CCSPlayerController).
void* ControllerPointer(int iSlot) {
	if (!__s2sdk_PlayerSlotToEntPointer) {
		return nullptr;
	}
	try {
		return s2sdk::PlayerSlotToEntPointer(iSlot);
	} catch (...) {
		return nullptr;
	}
}

// The buy-zone flag of the pawn (the field lives on CCSPlayerPawn).
bool InBuyZone(int iSlot) {
	// The pawn is a real entity, so the handle-based schema API is used (the "2" pointer variants
	// are only for non-entity schema objects). pisex::GetPlayerPawn resolves the slot's current
	// pawn as an index and validates it; the handle is checked again before the read.
	if (!__s2sdk_GetEntSchema || !__s2sdk_EntIndexToEntHandle || !__s2sdk_IsValidEntHandle) {
		return false;
	}
	const int pawnIndex = pisex::GetPlayerPawn(iSlot);
	if (pawnIndex <= 0) {
		return false;
	}
	try {
		const int handle = s2sdk::EntIndexToEntHandle(pawnIndex);
		if (!s2sdk::IsValidEntHandle(handle)) {
			return false;
		}
		return s2sdk::GetEntSchema(handle, "CCSPlayerPawn", "m_bInBuyZone", 0) != 0;
	} catch (...) {
		return false;
	}
}

// The pawn's item services (the original only null-checked the pointer).
void* ItemServices(int iSlot) {
	void* pawn = PawnPointer(iSlot);
	if (!pawn) {
		return nullptr;
	}
	const int offset = pisex::SchemaOffset("CBasePlayerPawn", "m_pItemServices");
	if (offset < 0) {
		return nullptr;
	}
	try {
		// m_pItemServices is a pointer field: its value is the component address.
		return *reinterpret_cast<void**>(static_cast<char*>(pawn) + offset);
	} catch (...) {
		return nullptr;
	}
}

// The controller's in-game money services (the original only null-checked the pointer).
void* MoneyServices(int iSlot) {
	void* controller = ControllerPointer(iSlot);
	if (!controller) {
		return nullptr;
	}
	const int offset = pisex::SchemaOffset("CCSPlayerController", "m_pInGameMoneyServices");
	if (offset < 0) {
		return nullptr;
	}
	try {
		return *reinterpret_cast<void**>(static_cast<char*>(controller) + offset);
	} catch (...) {
		return nullptr;
	}
}

class BuyTeamWeaponPlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override;
	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds dt) override;
	plg::PluginResult OnPluginEnd() override;

private:
	void LoadConfig();
	void LoadSection(pisex::KeyValues& kv, const char* szSection, int iTable);
	// VIP_BuyWeapon of the original (the RegCommand handler).
	bool BuyWeapon(int iSlot, const char* szContent);
	// OnRoundStart of the original (the "round_start" hook).
	void OnRoundStart(const char* szName, pisex::IGameEvent* pEvent, bool bDontBroadcast);

	// g_WeaponData of the original: [0] = the "T" section, [1] = the "CT" section.
	vip_btw::WeaponTable m_weaponData[2];
	bool m_loaded = false;
};

BuyTeamWeaponPlugin g_plugin;

// --- buying ------------------------------------------------------------------

bool BuyTeamWeaponPlugin::BuyWeapon(int iSlot, const char* szContent) {
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
	// CCSPlayerController + GetPlayerPawn + IsAlive.
	if (!pisex::IsAlive(iSlot)) {
		return false;
	}
	const int iTeamNum = vip_btw::TeamIndex(pisex::GetPlayerTeam(iSlot));
	if (!m_weaponData[iTeamNum].contains(sCommand)) {
		return false;
	}

	// Outside the buy zone the original printed "btw_no_buyzone" and refused the purchase.
	if (!InBuyZone(iSlot)) {
		pisex::PrintToChat(iSlot, "%s", g_pVIPCore->VIP_GetTranslate(kNoBuyZonePhrase));
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
		pisex::PrintToChat(iSlot, "%s", g_pVIPCore->VIP_GetTranslate(kNoMoneyPhrase));
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
	// an entity, so the handle-based call is used (the "2" pointer variants are only for
	// non-entity schema objects).
	if (__s2sdk_NetworkStateChanged && __s2sdk_PlayerSlotToEntHandle && __s2sdk_IsValidEntHandle) {
		try {
			const int handle = s2sdk::PlayerSlotToEntHandle(iSlot);
			if (s2sdk::IsValidEntHandle(handle)) {
				s2sdk::NetworkStateChanged(handle, "CCSPlayerController", "m_pInGameMoneyServices");
			}
		} catch (...) {
		}
	}
	return false;
}

// --- round start --------------------------------------------------------------

void BuyTeamWeaponPlugin::OnRoundStart(const char* /*szName*/, pisex::IGameEvent* /*pEvent*/,
									   bool /*bDontBroadcast*/) {
	if (!m_loaded) {
		return;
	}
	for (int i = 0; i < kMaxSlots; ++i) {
		if (!g_pVIPCore->VIP_IsClientVIP(i) || !g_pVIPCore->VIP_GetClientFeatureBool(i, kFeature)) {
			continue;
		}
		// The original's controller lookup; a missing controller ends the whole loop (parity).
		if (!pisex::IsInGame(i)) {
			return;
		}
		// A missing or dead pawn ends the loop too (the original's IsAlive check).
		if (!pisex::IsAlive(i)) {
			return;
		}
		// The team index (team - 2) must be one of the two tables; otherwise the loop ends.
		const int teamIndex = pisex::GetPlayerTeam(i) - 2;
		if (teamIndex < 0 || teamIndex >= 2) {
			return;
		}
		const std::string szWeapons = vip_btw::JoinWeapons(m_weaponData[teamIndex]);
		// The original printed the round-start phrase, formatted with the weapon list.
		const std::string text =
			pisex::FormatString(g_pVIPCore->VIP_GetTranslate(kRoundStartPhrase), szWeapons.c_str());
		pisex::PrintToChat(i, "%s", text.c_str());
	}
}

// --- config / lifecycle -------------------------------------------------------

void BuyTeamWeaponPlugin::LoadSection(pisex::KeyValues& kv, const char* szSection, int iTable) {
	pisex::KeyValues* hSection = kv.FindKey(szSection);
	if (!hSection) {
		return;
	}
	FOR_EACH_SUBKEY(hSection, pValue) {
		const std::string szCommand = pValue->GetName();
		const int iPrice = pValue->GetInt("price");
		const std::string szWeapon = pValue->GetString("weapon");
		m_weaponData[iTable][szCommand] = {szWeapon, iPrice};

		// The original formatted the chat "!<name>" and console "mm_<name>" command names.
		char szChat[128];
		char szConsole[128];
		std::snprintf(szChat, sizeof(szChat), "!%s", szCommand.c_str());
		std::snprintf(szConsole, sizeof(szConsole), "mm_%s", szCommand.c_str());
		// The original registered both names (console + chat) to the buy handler.
		pisex::RegCommand({szConsole}, {szChat},
						  [this](int iSlot, const char* szContent) { return BuyWeapon(iSlot, szContent); });
	}
}

void BuyTeamWeaponPlugin::LoadConfig() {
	m_loaded = false;
	pisex::KeyValues kv("VIP");
	if (!kv.LoadFromFile(kConfigFile)) {
		LogTaggedError(std::string("failed to load ") + kConfigFile +
					   " - the module stays disabled (no buy commands, no round-start message)");
		return;
	}
	// The original's LoadConfig: the "CT" section fills g_WeaponData[1], the "T" section [0].
	LoadSection(kv, "CT", 1);
	LoadSection(kv, "T", 0);
	m_loaded = true;
}

plg::PluginResult BuyTeamWeaponPlugin::OnPluginStart() {
	try {
		// Never fatal: only logs when the s2sdk bindings are missing.
		pisex::Init("vip_btw");

		LoadConfig();

		// The original hooked "round_start" in AllPluginsLoaded.
		pisex::HookEvent("round_start", [this](const char* szName, pisex::IGameEvent* pEvent, bool bDontBroadcast) {
			OnRoundStart(szName, pEvent, bDontBroadcast);
		});

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
		pisex::Poll();
	} catch (const std::exception& ex) {
		LogTaggedError(std::string("OnPluginUpdate failed: ") + ex.what());
	} catch (...) {
		LogTaggedError("OnPluginUpdate failed with an unknown exception");
	}
	return {};
}

plg::PluginResult BuyTeamWeaponPlugin::OnPluginEnd() {
	try {
		pisex::Shutdown(); // drops the round_start hook and the buy commands
	} catch (...) {
		// nothing left to do at unload
	}
	return {};
}

} // namespace

PLUGIFY_PLUGIN(PLUGIN_API, &g_plugin)
