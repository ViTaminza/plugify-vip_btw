// vip_btw tests — no engine: the plugin TU (src/plugin.cpp) is compiled in and every
// __s2sdk_* / __vip_* binding slot is a recording fake.
//
// Covered:
//   * the pure helpers (command tokenizer, "mm_"/"!" stripping, team table choice, round-start
//     list, command spellings);
//   * the KV1 reader on the production-shaped vip_btw.ini and on comments / unquoted tokens /
//     malformed files;
//   * PITFALLS B1/B2 — the map gate: not one s2sdk client, entity or schema call before the first
//     map, in the window between OnMapEnd and the next OnMapStart, or after OnPluginEnd; the first
//     update latches readiness only when no OnMapEnd was seen since OnPluginStart (a plugin loaded
//     into the map-change window); without the map listeners the gate never opens;
//   * PITFALLS C2 — IsConnected/IsInGame/IsAlive/PawnHandle/GetPlayerTeam: the slot range,
//     connected / in-game / SourceTV / controller / pawn / alive layers, each checked before the
//     next export is called;
//   * PITFALLS C1 — valid negative CEntityHandle values for the controller and the pawn work end
//     to end (team, buy zone, purchase, state change), while the 0/-1 sentinels are refused even
//     when IsValidEntHandle is mutated to accept everything;
//   * PITFALLS C8 — GetSchemaOffset is only called with a live map and a successful offset is
//     cached until OnPluginEnd;
//   * the guarded PrintToChat, the cs2-menus colour table and FormatString;
//   * the config resolution: <configs>/vip_btw/vip_btw.ini first, <configs>/vip/vip_btw.ini as the
//     fallback, a missing or malformed file leaves the module disabled;
//   * the command registration and dispatch: console "mm_<name>" and chat "!<name>" on their own
//     sources, a bare chat word through say/say_team, the buy path, and a clean unload
//     (RemoveCommand / RemoveCommandListener for the right names, PITFALLS A7/C11/C12);
//   * the round_start hook and message (C12).
//
// Run by ./test.sh (build/tests/vip_btw_tests), or directly.

#include "../src/plugin.cpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef VIP_BTW_TEST_DATA_DIR
#define VIP_BTW_TEST_DATA_DIR "plugins/vip_btw/tests/data"
#endif

namespace {

int g_checks = 0;
int g_failures = 0;

// Non-printable bytes are shown as \xNN: the colour output is mostly control bytes.
std::string Printable(std::string_view text) {
	std::string out;
	for (const unsigned char c : text) {
		if (c >= 0x20 && c < 0x7f) {
			out.push_back(static_cast<char>(c));
		} else {
			char buffer[8];
			std::snprintf(buffer, sizeof(buffer), "\\x%02X", c);
			out.append(buffer);
		}
	}
	return out;
}

void Check(bool condition, const std::string& what) {
	++g_checks;
	if (!condition) {
		++g_failures;
		std::printf("FAIL: %s\n", what.c_str());
	}
}

void CheckEq(std::string_view actual, std::string_view expected, const std::string& what) {
	++g_checks;
	if (actual != expected) {
		++g_failures;
		std::printf("FAIL: %s (expected \"%s\", got \"%s\")\n", what.c_str(),
					Printable(expected).c_str(), Printable(actual).c_str());
	}
}

void CheckEqInt(long long actual, long long expected, const std::string& what) {
	++g_checks;
	if (actual != expected) {
		++g_failures;
		std::printf("FAIL: %s (expected %lld, got %lld)\n", what.c_str(), expected, actual);
	}
}

std::string ToStd(const plg::string& text) {
	return std::string(text.data(), text.size());
}

plg::string ToPlg(std::string_view text) {
	return plg::string(text.data(), text.size());
}

// --- the plugify host fakes -------------------------------------------------------------------

std::vector<std::string> g_logLines;
std::string g_configsDir;

void FakeLog(std::string_view message, plg::Severity /*severity*/,
			 const plg::source_location& /*location*/) {
	g_logLines.emplace_back(message);
}

plg::string FakeGetConfigsDir() {
	return ToPlg(g_configsDir);
}

// --- the fake s2sdk world ---------------------------------------------------------------------

constexpr int32_t kTeamOffset = 100;       // CBaseEntity::m_iTeamNum
constexpr int32_t kBuyZoneOffset = 200;    // CCSPlayerPawn::m_bInBuyZone
// The pointer fields are 8-byte aligned in the real classes, and the raw read below relies on it.
constexpr int32_t kItemServicesOffset = 400;   // CBasePlayerPawn::m_pItemServices
constexpr int32_t kMoneyServicesOffset = 408;  // CCSPlayerController::m_pInGameMoneyServices
constexpr int32_t kControllerHandleBase = 0x100;
constexpr int32_t kPawnHandleBase = 0x200;
constexpr int32_t kInvalidPawnHandle = 999;

struct FakeClient {
	bool connected = true;
	bool inGame = true;
	bool sourceTV = false;
	bool alive = true;
	bool inBuyZone = true;
	bool pawnPointerValid = true;
	bool controllerPointerValid = true;
	int32_t controllerHandle = 0;
	int32_t pawnHandle = 0;
	int team = 2;
	int money = 16000;
	void* controller = nullptr;
	void* pawn = nullptr;
};

std::array<FakeClient, vip_btw::kMaxSlots> g_clients;

// The live "memory" of the fake pawn/controller: the plugin reads the component pointers with a
// raw 8-byte read at the cached schema offset (PITFALLS C6). The rows are 8-aligned like the real
// fields (schema offsets are aligned too, so the fake must not rely on unaligned access).
struct alignas(8) FakeMemory {
	unsigned char bytes[512]{};

	unsigned char* data() { return bytes; }
	const unsigned char* data() const { return bytes; }
	void fill(unsigned char value) { std::fill(std::begin(bytes), std::end(bytes), value); }
};

std::array<FakeMemory, vip_btw::kMaxSlots> g_pawnMemory{};
std::array<FakeMemory, vip_btw::kMaxSlots> g_controllerMemory{};
unsigned char g_itemServices{};
unsigned char g_moneyServices{};

int g_connectedCalls = 0;
int g_inGameCalls = 0;
int g_sourceTVCalls = 0;
int g_controllerPointerCalls = 0;
int g_controllerHandleCalls = 0;
int g_pawnCalls = 0;
int g_entPointerCalls = 0;
int g_validHandleCalls = 0;
int g_validPointerCalls = 0;
int g_aliveCalls = 0;
int g_schemaCalls = 0;
int g_entData2Calls = 0;
int g_entDataCalls = 0;
int g_moneyGets = 0;
int g_moneySets = 0;
int g_giveCalls = 0;
int g_networkStateCalls = 0;
int g_badSlotCalls = 0;

std::vector<std::string> g_schemaLookups;
std::vector<std::pair<int32_t, std::string>> g_chatPrints;
std::vector<std::pair<int32_t, std::string>> g_givenItems;
std::vector<int32_t> g_moneySetValues;
std::vector<int32_t> g_networkStateHandles;

std::string g_roundStartFormat = "Weapons: %s";
std::string g_noBuyZoneText = "{RED}no buyzone";
std::string g_noMoneyText = "{RED}no money";
std::vector<std::string> g_translateCalls;

int SlotFromPawnPointer(const void* entity) {
	for (int32_t i = 0; i < vip_btw::kMaxSlots; ++i) {
		if (entity == g_pawnMemory[static_cast<std::size_t>(i)].data()) {
			return i;
		}
	}
	return -1;
}

int SlotFromControllerPointer(const void* entity) {
	for (int32_t i = 0; i < vip_btw::kMaxSlots; ++i) {
		if (entity == g_controllerMemory[static_cast<std::size_t>(i)].data()) {
			return i;
		}
	}
	return -1;
}

// The handle is looked up by value, so a test may install any valid handle - including a negative
// one - on any slot (PITFALLS C1: the sign of a CEntityHandle means nothing).
int SlotFromPawnHandle(int32_t handle) {
	for (int32_t i = 0; i < vip_btw::kMaxSlots; ++i) {
		if (g_clients[static_cast<std::size_t>(i)].pawnHandle == handle) {
			return i;
		}
	}
	return -1;
}

bool InRange(int32_t slot) {
	if (slot >= 0 && slot < vip_btw::kMaxSlots) {
		return true;
	}
	++g_badSlotCalls;
	return false;
}

void ResetCounters() {
	g_connectedCalls = 0;
	g_inGameCalls = 0;
	g_sourceTVCalls = 0;
	g_controllerPointerCalls = 0;
	g_controllerHandleCalls = 0;
	g_pawnCalls = 0;
	g_entPointerCalls = 0;
	g_validHandleCalls = 0;
	g_validPointerCalls = 0;
	g_aliveCalls = 0;
	g_schemaCalls = 0;
	g_entDataCalls = 0;
	g_entData2Calls = 0;
	g_moneyGets = 0;
	g_moneySets = 0;
	g_giveCalls = 0;
	g_networkStateCalls = 0;
	g_badSlotCalls = 0;
	g_schemaLookups.clear();
	g_chatPrints.clear();
	g_givenItems.clear();
	g_moneySetValues.clear();
	g_networkStateHandles.clear();
	g_translateCalls.clear();
}

int ClientCalls() {
	return g_connectedCalls + g_inGameCalls + g_sourceTVCalls + g_controllerPointerCalls +
		   g_controllerHandleCalls + g_pawnCalls + g_entPointerCalls + g_validHandleCalls +
		   g_validPointerCalls + g_aliveCalls;
}

bool FakeIsClientConnected(int32_t slot) {
	++g_connectedCalls;
	return InRange(slot) && g_clients[static_cast<std::size_t>(slot)].connected;
}

bool FakeIsClientInGame(int32_t slot) {
	++g_inGameCalls;
	return InRange(slot) && g_clients[static_cast<std::size_t>(slot)].inGame;
}

bool FakeIsClientSourceTV(int32_t slot) {
	++g_sourceTVCalls;
	return InRange(slot) && g_clients[static_cast<std::size_t>(slot)].sourceTV;
}

bool FakeIsClientAlive(int32_t slot) {
	++g_aliveCalls;
	return InRange(slot) && g_clients[static_cast<std::size_t>(slot)].alive;
}

void* FakePlayerSlotToEntPointer(int32_t slot) {
	++g_controllerPointerCalls;
	return InRange(slot) ? g_clients[static_cast<std::size_t>(slot)].controller : nullptr;
}

int32_t FakePlayerSlotToEntHandle(int32_t slot) {
	++g_controllerHandleCalls;
	return InRange(slot) ? g_clients[static_cast<std::size_t>(slot)].controllerHandle : -1;
}

void* FakeGetClientPawn(int32_t slot) {
	++g_pawnCalls;
	return InRange(slot) ? g_clients[static_cast<std::size_t>(slot)].pawn : nullptr;
}

int32_t FakeEntPointerToEntHandle(void* entity) {
	++g_entPointerCalls;
	const int slot = SlotFromPawnPointer(entity);
	return slot < 0 ? -1 : g_clients[static_cast<std::size_t>(slot)].pawnHandle;
}

bool FakeIsValidEntHandle(int32_t handle) {
	++g_validHandleCalls;
	return handle != -1 && handle != 0 && handle != kInvalidPawnHandle;
}

// The mutation-check validator: it accepts every handle, even 0 and -1, so the plugin's own
// sentinel checks must be what rejects them.
bool FakeIsValidEntHandlePermissive(int32_t /*handle*/) {
	++g_validHandleCalls;
	return true;
}

bool FakeIsValidEntPointer(void* entity) {
	++g_validPointerCalls;
	if (entity == nullptr || entity == reinterpret_cast<void*>(0xBAD)) {
		return false;
	}
	const int pawnSlot = SlotFromPawnPointer(entity);
	if (pawnSlot >= 0) {
		return g_clients[static_cast<std::size_t>(pawnSlot)].pawnPointerValid;
	}
	const int controllerSlot = SlotFromControllerPointer(entity);
	if (controllerSlot >= 0) {
		return g_clients[static_cast<std::size_t>(controllerSlot)].controllerPointerValid;
	}
	return true;
}

int32_t FakeGetSchemaOffset(const plg::string& className, const plg::string& memberName) {
	++g_schemaCalls;
	g_schemaLookups.push_back(ToStd(className) + "::" + ToStd(memberName));
	if (className == "CBaseEntity" && memberName == "m_iTeamNum") {
		return kTeamOffset;
	}
	if (className == "CCSPlayerPawn" && memberName == "m_bInBuyZone") {
		return kBuyZoneOffset;
	}
	if (className == "CBasePlayerPawn" && memberName == "m_pItemServices") {
		return kItemServicesOffset;
	}
	if (className == "CCSPlayerController" && memberName == "m_pInGameMoneyServices") {
		return kMoneyServicesOffset;
	}
	return -1;
}

int64_t FakeGetEntData(int32_t handle, int32_t offset, int32_t /*size*/) {
	++g_entDataCalls;
	const int slot = SlotFromPawnHandle(handle);
	if (slot < 0) {
		return 0;
	}
	if (offset == kBuyZoneOffset) {
		return g_clients[static_cast<std::size_t>(slot)].inBuyZone ? 1 : 0;
	}
	return 0;
}

int64_t FakeGetEntData2(void* entity, int32_t offset, int32_t /*size*/) {
	++g_entData2Calls;
	const int slot = SlotFromControllerPointer(entity);
	if (slot < 0) {
		return 0;
	}
	if (offset == kTeamOffset) {
		return g_clients[static_cast<std::size_t>(slot)].team;
	}
	return 0;
}

int32_t FakeGetClientMoney(int32_t slot) {
	++g_moneyGets;
	return InRange(slot) ? g_clients[static_cast<std::size_t>(slot)].money : 0;
}

void FakeSetClientMoney(int32_t slot, int32_t money) {
	++g_moneySets;
	if (!InRange(slot)) {
		return;
	}
	g_clients[static_cast<std::size_t>(slot)].money = money;
	g_moneySetValues.push_back(money);
}

int32_t FakeGiveNamedItem(int32_t slot, const plg::string& itemName) {
	++g_giveCalls;
	g_givenItems.emplace_back(slot, ToStd(itemName));
	return 1;
}

void FakeNetworkStateChanged(int32_t handle, const plg::string& /*className*/,
							 const plg::string& /*memberName*/) {
	++g_networkStateCalls;
	g_networkStateHandles.push_back(handle);
}

void FakePrintToChat(int32_t slot, const plg::string& message) {
	g_chatPrints.emplace_back(slot, ToStd(message));
}

// --- the fake map listeners -------------------------------------------------------------------

s2sdk::OnMapStartCallback g_mapStartCallback = nullptr;
s2sdk::OnMapEndCallback g_mapEndCallback = nullptr;
int g_mapStartRegisters = 0;
int g_mapStartUnregisters = 0;
int g_mapEndRegisters = 0;
int g_mapEndUnregisters = 0;

void FakeOnMapStartRegister(s2sdk::OnMapStartCallback callback) {
	g_mapStartCallback = callback;
	++g_mapStartRegisters;
}

void FakeOnMapStartUnregister(s2sdk::OnMapStartCallback callback) {
	if (g_mapStartCallback == callback) {
		g_mapStartCallback = nullptr;
	}
	++g_mapStartUnregisters;
}

void FakeOnMapEndRegister(s2sdk::OnMapEndCallback callback) {
	g_mapEndCallback = callback;
	++g_mapEndRegisters;
}

void FakeOnMapEndUnregister(s2sdk::OnMapEndCallback callback) {
	if (g_mapEndCallback == callback) {
		g_mapEndCallback = nullptr;
	}
	++g_mapEndUnregisters;
}

void FireMapStart() {
	if (g_mapStartCallback != nullptr) {
		g_mapStartCallback();
	}
}

void FireMapEnd() {
	if (g_mapEndCallback != nullptr) {
		g_mapEndCallback();
	}
}

// --- the fake command/event bindings ----------------------------------------------------------

struct FakeCommand {
	std::string name;
	s2sdk::ConVarFlag flags = s2sdk::ConVarFlag::None;
	s2sdk::ConCommandCallback callback = nullptr;
};

std::vector<FakeCommand> g_consoleCommands;
std::vector<std::pair<std::string, s2sdk::ConCommandCallback>> g_listeners;
std::vector<std::string> g_removedCommands;
std::vector<std::string> g_removedListeners;
std::vector<std::string> g_existingCommands;
int g_addCommandCalls = 0;

bool FakeAddConsoleCommand(const plg::string& name, const plg::string& /*description*/,
						   s2sdk::ConVarFlag flags, s2sdk::ConCommandCallback callback,
						   s2sdk::HookMode /*type*/) {
	++g_addCommandCalls;
	const std::string commandName = ToStd(name);
	g_consoleCommands.push_back({commandName, flags, callback});
	return std::find(g_existingCommands.begin(), g_existingCommands.end(), commandName) ==
		   g_existingCommands.end();
}

bool FakeRemoveCommand(const plg::string& name, s2sdk::ConCommandCallback /*callback*/) {
	g_removedCommands.push_back(ToStd(name));
	return true;
}

bool FakeAddCommandListener(const plg::string& name, s2sdk::ConCommandCallback callback,
							s2sdk::HookMode /*type*/) {
	g_listeners.emplace_back(ToStd(name), callback);
	return true;
}

bool FakeRemoveCommandListener(const plg::string& name, s2sdk::ConCommandCallback /*callback*/,
							   s2sdk::HookMode /*type*/) {
	g_removedListeners.push_back(ToStd(name));
	return true;
}

std::string g_hookedEvent;
s2sdk::EventCallback g_eventCallback = nullptr;
s2sdk::HookMode g_eventMode = s2sdk::HookMode::Post;
int g_hookCalls = 0;
int g_unhookCalls = 0;

s2sdk::EventHookError FakeHookEvent(const plg::string& name, s2sdk::EventCallback callback,
									s2sdk::HookMode mode) {
	++g_hookCalls;
	g_hookedEvent = ToStd(name);
	g_eventCallback = callback;
	g_eventMode = mode;
	return s2sdk::EventHookError::Okay;
}

s2sdk::EventHookError FakeUnhookEvent(const plg::string& name, s2sdk::EventCallback callback,
									  s2sdk::HookMode mode) {
	++g_unhookCalls;
	if (g_hookedEvent == ToStd(name) && g_eventCallback == callback && g_eventMode == mode) {
		g_eventCallback = nullptr;
	}
	return s2sdk::EventHookError::Okay;
}

// --- the fake vip core ------------------------------------------------------------------------

bool g_vipLoaded = true;
std::array<bool, vip_btw::kMaxSlots> g_isVIP{};
std::array<bool, vip_btw::kMaxSlots> g_hasFeature{};
std::vector<std::string> g_registeredFeatures;
vip::ValueType g_registeredValueType = vip::ValueType::VIP_NULL;
vip::FeatureType g_registeredFeatureType = vip::FeatureType::TOGGLABLE;

bool FakeVipIsLoaded() {
	return g_vipLoaded;
}

bool FakeVipIsClientVIP(int32_t slot) {
	return InRange(slot) && g_isVIP[static_cast<std::size_t>(slot)];
}

bool FakeVipGetClientFeatureBool(int32_t slot, const plg::string& /*feature*/) {
	return InRange(slot) && g_hasFeature[static_cast<std::size_t>(slot)];
}

plg::string FakeVipGetTranslate(const plg::string& phrase) {
	const std::string key = ToStd(phrase);
	g_translateCalls.push_back(key);
	if (key == "btw_round_start") {
		return ToPlg(g_roundStartFormat);
	}
	if (key == "btw_no_buyzone") {
		return ToPlg(g_noBuyZoneText);
	}
	if (key == "btw_no_money") {
		return ToPlg(g_noMoneyText);
	}
	return ToPlg(std::string());
}

void FakeVipRegisterFeature(const plg::string& feature, vip::ValueType valueType,
							vip::FeatureType featureType, vip::ItemSelectableDelegate /*select*/,
							vip::ItemTogglableDelegate /*toggle*/,
							vip::ItemDisplayDelegate /*display*/) {
	g_registeredFeatures.push_back(ToStd(feature));
	g_registeredValueType = valueType;
	g_registeredFeatureType = featureType;
}

// --- install / reset the fake world ----------------------------------------------------------

void InstallFakes() {
	plg::Log = &FakeLog;
	plg::GetConfigsDir = &FakeGetConfigsDir;

	__s2sdk_OnMapStart_Register = &FakeOnMapStartRegister;
	__s2sdk_OnMapStart_Unregister = &FakeOnMapStartUnregister;
	__s2sdk_OnMapEnd_Register = &FakeOnMapEndRegister;
	__s2sdk_OnMapEnd_Unregister = &FakeOnMapEndUnregister;

	__s2sdk_IsClientConnected = &FakeIsClientConnected;
	__s2sdk_IsClientInGame = &FakeIsClientInGame;
	__s2sdk_IsClientSourceTV = &FakeIsClientSourceTV;
	__s2sdk_IsClientAlive = &FakeIsClientAlive;
	__s2sdk_PlayerSlotToEntPointer = &FakePlayerSlotToEntPointer;
	__s2sdk_PlayerSlotToEntHandle = &FakePlayerSlotToEntHandle;
	__s2sdk_GetClientPawn = &FakeGetClientPawn;
	__s2sdk_EntPointerToEntHandle = &FakeEntPointerToEntHandle;
	__s2sdk_IsValidEntHandle = &FakeIsValidEntHandle;
	__s2sdk_IsValidEntPointer = &FakeIsValidEntPointer;

	__s2sdk_GetSchemaOffset = &FakeGetSchemaOffset;
	__s2sdk_GetEntData = &FakeGetEntData;
	__s2sdk_GetEntData2 = &FakeGetEntData2;

	__s2sdk_GetClientMoney = &FakeGetClientMoney;
	__s2sdk_SetClientMoney = &FakeSetClientMoney;
	__s2sdk_GiveNamedItem = &FakeGiveNamedItem;
	__s2sdk_NetworkStateChanged = &FakeNetworkStateChanged;
	__s2sdk_PrintToChat = &FakePrintToChat;

	__s2sdk_AddConsoleCommand = &FakeAddConsoleCommand;
	__s2sdk_RemoveCommand = &FakeRemoveCommand;
	__s2sdk_AddCommandListener = &FakeAddCommandListener;
	__s2sdk_RemoveCommandListener = &FakeRemoveCommandListener;

	__s2sdk_HookEvent = &FakeHookEvent;
	__s2sdk_UnhookEvent = &FakeUnhookEvent;

	__vip_VIP_IsVIPLoaded = &FakeVipIsLoaded;
	__vip_VIP_IsClientVIP = &FakeVipIsClientVIP;
	__vip_VIP_GetClientFeatureBool = &FakeVipGetClientFeatureBool;
	__vip_VIP_GetTranslate = &FakeVipGetTranslate;
	__vip_VIP_RegisterFeature = &FakeVipRegisterFeature;
}

// Forgets every fake registration and all recorded state. The plugin must have been stopped first
// (PrepareTest/FinishTest do that), otherwise it still believes its listeners are registered.
void ResetWorld() {
	for (int32_t i = 0; i < vip_btw::kMaxSlots; ++i) {
		const std::size_t slot = static_cast<std::size_t>(i);
		FakeClient& client = g_clients[slot];
		client = FakeClient{};
		client.controllerHandle = kControllerHandleBase + i;
		client.pawnHandle = kPawnHandleBase + i;
		client.controller = g_controllerMemory[slot].data();
		client.pawn = g_pawnMemory[slot].data();
		g_pawnMemory[slot].fill(0);
		g_controllerMemory[slot].fill(0);
		*reinterpret_cast<void**>(g_pawnMemory[slot].data() + kItemServicesOffset) =
			&g_itemServices;
		*reinterpret_cast<void**>(g_controllerMemory[slot].data() + kMoneyServicesOffset) =
			&g_moneyServices;
	}

	ResetCounters();
	g_logLines.clear();

	g_mapStartCallback = nullptr;
	g_mapEndCallback = nullptr;
	g_mapStartRegisters = 0;
	g_mapStartUnregisters = 0;
	g_mapEndRegisters = 0;
	g_mapEndUnregisters = 0;

	g_consoleCommands.clear();
	g_listeners.clear();
	g_removedCommands.clear();
	g_removedListeners.clear();
	g_existingCommands.clear();
	g_addCommandCalls = 0;

	g_hookedEvent.clear();
	g_eventCallback = nullptr;
	g_eventMode = s2sdk::HookMode::Post;
	g_hookCalls = 0;
	g_unhookCalls = 0;

	g_vipLoaded = true;
	g_isVIP.fill(false);
	g_hasFeature.fill(false);
	g_registeredFeatures.clear();
	g_registeredValueType = vip::ValueType::VIP_NULL;
	g_registeredFeatureType = vip::FeatureType::TOGGLABLE;

	g_roundStartFormat = "Weapons: %s";
	g_noBuyZoneText = "{RED}no buyzone";
	g_noMoneyText = "{RED}no money";

	g_configsDir = std::string(VIP_BTW_TEST_DATA_DIR) + "/primary";

	// The translated phrases are cached per key by the consumer header.
	g_pVIPCore->ReloadTranslations();
}

// Stops whatever a previous test left behind (OnPluginEnd is a no-op when nothing is registered)
// and starts the next test from an empty fake world.
void PrepareTest() {
	g_plugin.OnPluginEnd();
	ResetWorld();
}

void FinishTest() {
	g_plugin.OnPluginEnd();
	ResetWorld();
}

// --- small helpers ----------------------------------------------------------------------------

const FakeCommand* FindCommand(std::string_view name) {
	for (const FakeCommand& command : g_consoleCommands) {
		if (command.name == name) {
			return &command;
		}
	}
	return nullptr;
}

s2sdk::ConCommandCallback FindListener(std::string_view name) {
	for (const auto& listener : g_listeners) {
		if (listener.first == name) {
			return listener.second;
		}
	}
	return nullptr;
}

plg::vector<plg::string> MakeArgs(std::initializer_list<std::string_view> words) {
	plg::vector<plg::string> args;
	for (const std::string_view word : words) {
		args.push_back(ToPlg(word));
	}
	return args;
}

// Calls the recorded ConCommand callback of `commandName` with the given arguments.
s2sdk::ResultType DispatchArgs(std::string_view commandName, int32_t slot,
							   s2sdk::ConCommandContext context,
							   const plg::vector<plg::string>& args) {
	const FakeCommand* command = FindCommand(commandName);
	if (command == nullptr || command->callback == nullptr) {
		Check(false, std::string("command '") + std::string(commandName) + "' is registered");
		return s2sdk::ResultType::Continue;
	}
	return command->callback(slot, context, args);
}

// Calls the recorded ConCommand callback of `commandName` with args[0] = `commandName`.
s2sdk::ResultType Dispatch(std::string_view commandName, int32_t slot,
						   s2sdk::ConCommandContext context,
						   std::initializer_list<std::string_view> rest = {}) {
	plg::vector<plg::string> args;
	args.push_back(ToPlg(commandName));
	for (const std::string_view word : rest) {
		args.push_back(ToPlg(word));
	}
	return DispatchArgs(commandName, slot, context, args);
}

// --- A. the pure helpers ----------------------------------------------------------------------

void TestPureHelpers() {
	// Tokenizing (the original's SplitStringBySpace over the command content).
	CheckEq(std::string("mm_ak47|15|20"),
			[&] {
				std::string joined;
				for (const std::string& token : vip_btw::SplitStringBySpace("mm_ak47 15 20")) {
					if (!joined.empty()) {
						joined.push_back('|');
					}
					joined += token;
				}
				return joined;
			}(),
			"split 'mm_ak47 15 20'");
	CheckEqInt(static_cast<long long>(vip_btw::SplitStringBySpace("  mm_ak47   extra ").size()), 2,
			   "leading/trailing spaces are dropped");
	CheckEqInt(static_cast<long long>(vip_btw::SplitStringBySpace("say \"a b\" c").size()), 3,
			   "quotes group a token");
	CheckEq(vip_btw::SplitStringBySpace("\"quoted only\"").front(), std::string("quoted only"),
			"quoted-only token");
	Check(vip_btw::SplitStringBySpace("").empty(), "empty content has no tokens");

	// "mm_"/"!" stripping (safe_replace, all occurrences).
	CheckEq(vip_btw::CleanCommand("!ak47"), "ak47", "clean '!ak47'");
	CheckEq(vip_btw::CleanCommand("mm_ak47"), "ak47", "clean 'mm_ak47'");
	CheckEq(vip_btw::CleanCommand("!mm_ak47"), "ak47", "clean '!mm_ak47'");
	CheckEq(vip_btw::CleanCommand("ak_47"), "ak_47", "no partial match of a command name");
	CheckEq(vip_btw::SafeReplace("abc", "", "x"), "abc", "empty search leaves the subject");

	// The team table choice of the original (CT = 3 -> table 1, anything else -> table 0).
	CheckEqInt(vip_btw::TeamIndex(3), 1, "CT uses the CT table");
	CheckEqInt(vip_btw::TeamIndex(2), 0, "T uses the T table");
	CheckEqInt(vip_btw::TeamIndex(1), 0, "spectators use the T table");

	// The round-start list (map order = sorted, like the original's std::map).
	vip_btw::WeaponTable table;
	CheckEq(vip_btw::JoinWeapons(table), "", "empty table");
	table["m4a4"] = {"weapon_m4a1", 3100};
	table["m4a1"] = {"weapon_m4a1_silencer", 2900};
	table["awp"] = {"weapon_awp", 4750};
	CheckEq(vip_btw::JoinWeapons(table), "awp, m4a1, m4a4", "three weapons, sorted");

	// The command spellings of the original's LoadSection.
	CheckEq(vip_btw::ConsoleCommandName("ak47"), "mm_ak47", "console spelling");
	CheckEq(vip_btw::ChatCommandName("ak47"), "!ak47", "chat spelling");
	Check(vip_btw::IsChatTrigger('!'), "'!' is a chat trigger");
	Check(vip_btw::IsChatTrigger('/'), "'/' is a chat trigger");
	Check(!vip_btw::IsChatTrigger('a'), "'a' is not a chat trigger");
	CheckEq(vip_btw::StripChatTrigger("!ak47"), "ak47", "strip '!'");
	CheckEq(vip_btw::StripChatTrigger("/ak47"), "ak47", "strip '/'");
	CheckEq(vip_btw::StripChatTrigger("ak47"), "ak47", "a bare name is unchanged");
	CheckEq(vip_btw::StripChatTrigger("!"), "", "the trigger alone becomes empty");
}

// --- B. the KV1 reader ------------------------------------------------------------------------

constexpr std::string_view kIniFixture = R"ini("VIP"
{
    "CT"
    {
        "ak47"  //!ak47 and mm_ak47
        {
            "price"     "2700"
            "weapon"    "weapon_ak47"
        }
    }
    "T"
    {
        "m4a1"
        {
            "price"     "2900"
            "weapon"    "weapon_m4a1_silencer"
        }
    }
}
)ini";

void TestKv1() {
	const std::optional<vip_btw::KvNode> ini = vip_btw::ParseKv1(kIniFixture);
	Check(ini.has_value(), "the production-shaped vip_btw.ini parses");
	if (ini) {
		const vip_btw::KvNode* ct = ini->Find("CT");
		Check(ct != nullptr, "the CT section is found");
		if (ct) {
			const vip_btw::KvNode* ak47 = ct->Find("ak47");
			Check(ak47 != nullptr, "the ak47 key is found");
			if (ak47) {
				CheckEqInt(ak47->GetInt("price", -1), 2700, "ak47 price");
				CheckEq(ak47->GetString("weapon"), "weapon_ak47", "ak47 weapon");
			}
			Check(ct->Find("missing") == nullptr, "a missing key is not found");
		}
		CheckEqInt(ini->Find("T")->Find("m4a1")->GetInt("price", -1), 2900, "T price");
		CheckEqInt(ini->Find("ct")->Find("AK47")->GetInt("PRICE", -1), 2700,
				   "section/key lookup is case-insensitive");
	}

	// Comments, #base lines, unquoted tokens and a nested block.
	const std::optional<vip_btw::KvNode> grammar = vip_btw::ParseKv1(R"(
#base "something.txt" // skipped, never included
VIP
{
    type 1
    outer /* block comment */ { inner "x" }
    "tail" "y" // trailing comment
}
)");
	Check(grammar.has_value(), "unquoted tokens / comments / nested blocks parse");
	if (grammar) {
		CheckEqInt(grammar->GetInt("type", -1), 1, "unquoted type value");
		const vip_btw::KvNode* outer = grammar->Find("outer");
		Check(outer != nullptr && outer->GetString("inner") == "x", "nested block");
		CheckEq(grammar->GetString("tail"), "y", "quoted key/value with a trailing comment");
	}

	// Broken files are rejected instead of leaving a half-read tree (PITFALLS A1: the caller
	// stays disabled).
	Check(!vip_btw::ParseKv1("\"VIP\" { \"CT\" { \"ak47\" { }").has_value(),
		  "a missing '}' makes the file invalid");
	Check(!vip_btw::ParseKv1("\"VIP\" { \"CT\" { } }\n}").has_value(),
		  "a stray top-level '}' makes the file invalid");
	Check(!vip_btw::ParseKv1("\"VIP\" { \"key\" \"unterminated").has_value(),
		  "an unterminated string makes the file invalid");

	// The reader reads the real files the test data ships.
	Check(vip_btw::LoadKv1File(std::string(VIP_BTW_TEST_DATA_DIR) + "/primary/vip_btw/vip_btw.ini")
			  .has_value(),
		  "the primary config file loads from disk");
	Check(!vip_btw::LoadKv1File(std::string(VIP_BTW_TEST_DATA_DIR) + "/missing/vip_btw.ini")
			   .has_value(),
		  "a missing config file does not load");
}

// --- C. the map gate (PITFALLS B1/B2) ---------------------------------------------------------

void TestMapGate() {
	PrepareTest();
	ResetCounters();
	Check(g_plugin.OnPluginStart().GetCode() == plg::PluginCode::Ok,
		  "OnPluginStart returns Ok (never Failed)");
	Check(g_mapStartRegisters == 1 && g_mapEndRegisters == 1,
		  "OnPluginStart registers both map listeners");
	Check(!g_mapReady, "the gate is shut on plugin start");
	// PITFALLS B1: the start path (config, commands, feature, hook) touches no client, entity or
	// schema.
	CheckEqInt(ClientCalls(), 0, "OnPluginStart makes no s2sdk client/entity call");
	CheckEqInt(g_schemaCalls, 0, "OnPluginStart resolves no schema offset");

	// --- before the first map: not one client/entity/schema call may reach s2sdk --------------
	ResetCounters();
	Check(!IsConnected(3), "IsConnected is false before the first map");
	Check(!IsInGame(3), "IsInGame is false before the first map");
	Check(!IsAlive(3), "IsAlive is false before the first map");
	CheckEqInt(PawnHandle(3), 0, "no pawn handle before the first map");
	CheckEqInt(GetPlayerTeam(3), 0, "no team before the first map");
	Check(!InBuyZone(3), "no buy-zone read before the first map");
	CheckEqInt(ClientCalls(), 0, "not one s2sdk client call is made before the first map");
	CheckEqInt(g_schemaCalls, 0, "GetSchemaOffset is never called before the first map");

	// A plugin loaded into a running map: its first update latches readiness.
	g_plugin.OnPluginUpdate(std::chrono::milliseconds(0));
	Check(g_mapReady, "the first OnPluginUpdate latches the gate");
	Check(IsConnected(3), "a connected client is connected after the latch");
	Check(IsInGame(3), "a client in game is in game after the latch");
	Check(IsAlive(3), "a live client is alive after the latch");
	Check(ClientCalls() > 0, "client calls reach s2sdk after the latch");

	// --- after OnMapEnd: not one client/entity/schema call may reach s2sdk ---------------------
	FireMapEnd();
	Check(!g_mapReady, "OnMapEnd drops readiness");
	g_plugin.OnPluginUpdate(std::chrono::milliseconds(0));
	Check(!g_mapReady, "an update after OnMapEnd does not re-open the gate");
	ResetCounters();
	Check(!IsAlive(3), "IsAlive is false after OnMapEnd");
	Check(!IsConnected(3), "IsConnected is false after OnMapEnd");
	CheckEqInt(ClientCalls(), 0, "not one s2sdk client call is made after OnMapEnd");
	CheckEqInt(g_schemaCalls, 0, "GetSchemaOffset is never called after OnMapEnd");

	// --- the next OnMapStart opens the gate again without waiting for an update ----------------
	FireMapStart();
	Check(g_mapReady, "OnMapStart latches the gate");
	Check(IsAlive(3), "the client is alive again");

	FinishTest();
}

// A plugin loaded during the map change (OnMapEnd before its first update) must not latch on the
// update: the first update only counts when no OnMapEnd was seen since OnPluginStart.
void TestMapChangeWindowStart() {
	PrepareTest();
	g_plugin.OnPluginStart();
	FireMapEnd();
	g_plugin.OnPluginUpdate(std::chrono::milliseconds(0));
	Check(!g_mapReady, "the first update after an OnMapEnd does not latch the gate");
	ResetCounters();
	Check(!IsAlive(2), "no client is alive in the map-change window");
	CheckEqInt(ClientCalls(), 0, "not one s2sdk client call is made in the map-change window");

	FireMapStart();
	Check(g_mapReady, "the next OnMapStart opens the gate");
	Check(IsAlive(2), "the client is alive after the map start");

	FinishTest();
}

// Without the map listeners nothing could ever close the gate again, so it must never open.
void TestNoMapListeners() {
	PrepareTest();
	const s2sdk::_OnMapStart_Register savedStart = __s2sdk_OnMapStart_Register;
	const s2sdk::_OnMapEnd_Register savedEnd = __s2sdk_OnMapEnd_Register;
	__s2sdk_OnMapStart_Register = nullptr;
	__s2sdk_OnMapEnd_Register = nullptr;

	g_plugin.OnPluginStart();
	Check(!g_mapListenersRegistered, "without the map listeners the gate is never armed");
	g_plugin.OnPluginUpdate(std::chrono::milliseconds(0));
	Check(!g_mapReady, "an update cannot open a gate that has no map-end listener");
	ResetCounters();
	Check(!IsAlive(0), "no client is alive without the map listeners");
	CheckEqInt(ClientCalls(), 0, "not one s2sdk client call is made without the map listeners");

	g_plugin.OnPluginEnd();
	Check(!g_mapListenersRegistered, "OnPluginEnd leaves the unarmed gate alone");

	__s2sdk_OnMapStart_Register = savedStart;
	__s2sdk_OnMapEnd_Register = savedEnd;
	FinishTest();
}

// --- D. the client checks (PITFALLS C2) -------------------------------------------------------

void TestClientGuards() {
	PrepareTest();
	g_plugin.OnPluginStart();
	FireMapStart();
	Check(g_mapReady, "the map gate is open");

	// --- the slot range is checked before any export ------------------------------------------
	ResetCounters();
	Check(!IsConnected(-1), "slot -1 is refused");
	Check(!IsConnected(vip_btw::kMaxSlots), "slot 64 is refused");
	Check(!IsInGame(1000), "a far out-of-range slot is refused");
	Check(!IsAlive(1000), "IsAlive refuses a far out-of-range slot");
	CheckEqInt(ClientCalls(), 0, "no s2sdk call is made for an out-of-range slot");
	CheckEqInt(g_badSlotCalls, 0, "no export ever receives an out-of-range slot");

	// --- every IsAlive layer in order ---------------------------------------------------------
	Check(IsAlive(7), "a connected, in-game, alive client with a controller and pawn is alive");
	Check(IsInGame(7), "IsInGame accepts the same client");
	Check(IsConnected(7), "IsConnected accepts the same client");

	g_clients[7].connected = false;
	ResetCounters();
	Check(!IsAlive(7), "a disconnected slot is not alive");
	CheckEqInt(g_connectedCalls, 1, "only the connected check runs for a disconnected slot");
	CheckEqInt(g_inGameCalls + g_sourceTVCalls + g_controllerPointerCalls + g_pawnCalls +
				   g_aliveCalls,
			   0, "the lower layers are not reached for a disconnected slot");

	g_clients[7].connected = true;
	g_clients[7].inGame = false;
	ResetCounters();
	Check(!IsAlive(7), "a client outside the game is not alive");
	CheckEqInt(g_connectedCalls, 1, "the connected check runs once");
	CheckEqInt(g_inGameCalls, 1, "the in-game check runs");
	CheckEqInt(g_sourceTVCalls + g_controllerPointerCalls + g_pawnCalls + g_aliveCalls, 0,
			   "SourceTV and the lower layers are not reached for a client outside the game");

	g_clients[7].inGame = true;
	g_clients[7].sourceTV = true;
	ResetCounters();
	Check(!IsInGame(7), "the SourceTV bot is not in game");
	Check(!IsAlive(7), "the SourceTV bot is not alive");
	CheckEqInt(g_controllerPointerCalls + g_pawnCalls + g_aliveCalls, 0,
			   "the controller is not queried for SourceTV");

	g_clients[7].sourceTV = false;
	g_clients[7].controller = nullptr;
	ResetCounters();
	Check(!IsAlive(7), "a client without a controller is not alive");
	Check(g_pawnCalls == 0 && g_aliveCalls == 0,
		  "the pawn and IsClientAlive are not reached without a controller");

	g_clients[7].controller = g_controllerMemory[7].data();
	g_clients[7].controllerHandle = 0; // the world handle: invalid
	ResetCounters();
	Check(!IsAlive(7), "a controller with an invalid handle is not alive");
	Check(g_pawnCalls == 0 && g_aliveCalls == 0,
		  "the pawn and IsClientAlive are not reached without a controller handle");

	g_clients[7].controllerHandle = kControllerHandleBase + 7;
	g_clients[7].pawn = nullptr;
	ResetCounters();
	Check(!IsAlive(7), "a client without a pawn is not alive");
	Check(g_entPointerCalls == 0 && g_aliveCalls == 0,
		  "IsClientAlive is not reached without a pawn");

	g_clients[7].pawn = g_pawnMemory[7].data();
	g_clients[7].pawnHandle = kInvalidPawnHandle;
	ResetCounters();
	Check(!IsAlive(7), "a pawn with an invalid handle is not alive");
	Check(g_entPointerCalls == 1 && g_aliveCalls == 0,
		  "IsClientAlive is not reached with an invalid pawn handle");

	g_clients[7].pawnHandle = kPawnHandleBase + 7;
	g_clients[7].pawnPointerValid = false;
	ResetCounters();
	CheckEqInt(PawnHandle(7), 0, "an invalid pawn pointer has no handle");
	Check(!IsAlive(7), "a client with an invalid pawn pointer is not alive");
	Check(g_aliveCalls == 0, "IsClientAlive is not reached with an invalid pawn pointer");

	g_clients[7].pawnPointerValid = true;
	g_clients[7].alive = false;
	ResetCounters();
	Check(!IsAlive(7), "a dead client is not alive");
	CheckEqInt(g_aliveCalls, 1, "the alive check is reached for a valid client");

	g_clients[7].alive = true;
	ResetCounters();
	CheckEqInt(PawnHandle(7), kPawnHandleBase + 7, "PawnHandle returns the validated handle");
	CheckEqInt(GetPlayerTeam(7), 2, "GetPlayerTeam reads the 1-byte m_iTeamNum");
	CheckEqInt(g_entData2Calls, 1, "m_iTeamNum is read with GetEntData2");
	Check(std::find(g_schemaLookups.begin(), g_schemaLookups.end(), "CBaseEntity::m_iTeamNum") !=
			  g_schemaLookups.end(),
		  "GetPlayerTeam resolves CBaseEntity::m_iTeamNum");

	// --- the buy-zone flag (handle-based read) -------------------------------------------------
	g_clients[7].inBuyZone = true;
	Check(InBuyZone(7), "a player inside the buy zone is in the buy zone");
	g_clients[7].inBuyZone = false;
	Check(!InBuyZone(7), "a player outside the buy zone is not in the buy zone");
	Check(std::find(g_schemaLookups.begin(), g_schemaLookups.end(),
					"CCSPlayerPawn::m_bInBuyZone") != g_schemaLookups.end(),
		  "InBuyZone resolves CCSPlayerPawn::m_bInBuyZone");

	FinishTest();
}

// --- E. the schema offset cache (PITFALLS C8) -------------------------------------------------

void TestSchemaCache() {
	PrepareTest();
	g_plugin.OnPluginStart();
	FireMapStart();

	ResetCounters();
	CheckEqInt(GetPlayerTeam(3), 2, "team read");
	CheckEqInt(g_schemaCalls, 1, "the first team read resolves the offset");
	CheckEqInt(GetPlayerTeam(3), 2, "team read again");
	CheckEqInt(g_schemaCalls, 1, "a successful offset is cached");
	CheckEqInt(GetPlayerTeam(4), 2, "team read for another slot");
	CheckEqInt(g_schemaCalls, 1, "the cache is shared by every slot");

	Check(InBuyZone(3), "buy zone read");
	CheckEqInt(g_schemaCalls, 2, "a different field resolves its own offset");
	Check(InBuyZone(4), "buy zone read again");
	CheckEqInt(g_schemaCalls, 2, "the buy-zone offset is cached too");

	// PITFALLS C8/A7: OnPluginEnd forgets the cache, so a reload resolves the fields again.
	g_plugin.OnPluginEnd();
	g_plugin.OnPluginStart();
	FireMapStart();
	ResetCounters();
	CheckEqInt(GetPlayerTeam(3), 2, "team read after a reload");
	CheckEqInt(g_schemaCalls, 1, "a reload resolves the offset again");

	FinishTest();
}

// --- F. output: colour tags, FormatString, the guarded PrintToChat -----------------------------

void TestOutput() {
	CheckEq(Colorize("{RED}red{BLUE}blue{DEFAULT}"),
			std::string("\x02" "red" "\x0C" "blue" "\x01"), "cs2-menus colour tags are converted");
	CheckEq(Colorize("{LIGHTGREEN}ok\\nnext"), std::string("\x06" "ok\xe2\x80\xa9next"),
			"the phrase line break is U+2029");
	CheckEq(Colorize("plain {UNKNOWN} text"), "plain {UNKNOWN} text",
			"an unknown tag is left alone");

	CheckEq(FormatString("Weapons: %s", "ak47, m4a1"), "Weapons: ak47, m4a1", "FormatString");
	CheckEq(FormatString("%d%%", 7), "7%", "FormatString keeps the printf semantics");
	CheckEq(FormatString("", "ignored"), "", "an empty phrase prints nothing");

	PrepareTest();
	g_plugin.OnPluginStart();

	// Before the first map PrintToChat must not touch s2sdk (PITFALLS B2).
	ResetCounters();
	PrintToChat(3, "{RED}hello");
	Check(g_chatPrints.empty(), "PrintToChat is refused before the first map");

	FireMapStart();
	PrintToChat(3, "{RED}hello");
	CheckEqInt(static_cast<long long>(g_chatPrints.size()), 1, "PrintToChat prints after the latch");
	if (!g_chatPrints.empty()) {
		CheckEqInt(g_chatPrints.front().first, 3, "PrintToChat targets the slot");
		CheckEq(g_chatPrints.front().second, std::string("\x02hello"),
				"PrintToChat converts the colour tags");
	}

	g_clients[5].connected = false;
	PrintToChat(5, "hidden");
	CheckEqInt(static_cast<long long>(g_chatPrints.size()), 1,
			   "PrintToChat refuses a slot without a client");
	g_clients[5].connected = true;

	FinishTest();
}

// --- G. config resolution ---------------------------------------------------------------------

void TestConfigResolution() {
	PrepareTest();
	g_configsDir = std::string(VIP_BTW_TEST_DATA_DIR) + "/primary";
	g_plugin.OnPluginStart();
	Check(g_plugin.IsLoaded(), "the primary config loads");
	CheckEqInt(static_cast<long long>(g_plugin.GetWeaponTable(1).size()), 1, "one CT weapon");
	CheckEqInt(static_cast<long long>(g_plugin.GetWeaponTable(0).size()), 2, "two T weapons");
	CheckEq(g_plugin.GetWeaponTable(1).at("ak47").szName, "weapon_ak47", "the CT weapon name");
	CheckEqInt(g_plugin.GetWeaponTable(1).at("ak47").iPrice, 2700, "the CT weapon price");
	CheckEq(g_plugin.GetWeaponTable(0).at("m4a1").szName, "weapon_m4a1_silencer", "the T weapon name");
	g_plugin.OnPluginEnd();

	// <configs>/vip/vip_btw.ini is the fallback when the plugin's own dir has no file.
	PrepareTest();
	g_configsDir = std::string(VIP_BTW_TEST_DATA_DIR) + "/fallback";
	g_plugin.OnPluginStart();
	Check(g_plugin.IsLoaded(), "the fallback config loads");
	Check(g_plugin.GetWeaponTable(1).contains("awp"), "the fallback config fills the CT table");
	Check(!g_plugin.GetWeaponTable(1).contains("ak47"), "the primary config is not read in the fallback case");
	g_plugin.OnPluginEnd();

	// A missing config leaves the module disabled (logs, never Failed).
	PrepareTest();
	g_configsDir = std::string(VIP_BTW_TEST_DATA_DIR) + "/missing";
	g_plugin.OnPluginStart();
	Check(!g_plugin.IsLoaded(), "a missing config leaves the module disabled");
	CheckEqInt(static_cast<long long>(g_consoleCommands.size()), 0, "no commands without a config");
	Check(!g_logLines.empty(), "the missing config is logged");
	g_plugin.OnPluginEnd();

	// A malformed config is rejected and the module stays disabled.
	PrepareTest();
	g_configsDir = std::string(VIP_BTW_TEST_DATA_DIR) + "/broken";
	g_plugin.OnPluginStart();
	Check(!g_plugin.IsLoaded(), "a malformed config leaves the module disabled");
	CheckEqInt(static_cast<long long>(g_consoleCommands.size()), 0,
			   "no commands from a malformed config");
	g_plugin.OnPluginEnd();

	// No configs dir at all: still not fatal.
	PrepareTest();
	plg::GetConfigsDir = nullptr;
	g_plugin.OnPluginStart();
	Check(!g_plugin.IsLoaded(), "no configs dir leaves the module disabled");
	g_plugin.OnPluginEnd();
	plg::GetConfigsDir = &FakeGetConfigsDir;

	FinishTest();
}

// --- H. command registration + dispatch (PITFALLS C11) ----------------------------------------

// Prepares a VIP that can buy the CT "ak47" (team 3 -> the CT table).
void MakeBuyer(int32_t slot, int team = 3) {
	g_clients[static_cast<std::size_t>(slot)].team = team;
	g_isVIP[static_cast<std::size_t>(slot)] = true;
	g_hasFeature[static_cast<std::size_t>(slot)] = true;
}

void TestCommandRegistration() {
	PrepareTest();
	g_configsDir = std::string(VIP_BTW_TEST_DATA_DIR) + "/primary";
	g_plugin.OnPluginStart();

	CheckEqInt(g_addCommandCalls, 6, "three weapons register a console and a chat command");
	CheckEqInt(static_cast<long long>(g_consoleCommands.size()), 6, "six s2sdk command names");
	Check(FindCommand("mm_ak47") != nullptr, "the console spelling is registered");
	Check(FindCommand("ak47") != nullptr, "the trigger-less chat name is registered");
	Check(FindCommand("mm_m4a1") != nullptr && FindCommand("m4a1") != nullptr,
		  "the T console/chat names are registered");
	Check(FindCommand("mm_m4a4") != nullptr && FindCommand("m4a4") != nullptr,
		  "the second T console/chat names are registered");
	for (const FakeCommand& command : g_consoleCommands) {
		const int64_t flags = static_cast<int64_t>(command.flags);
		Check((flags & static_cast<int64_t>(s2sdk::ConVarFlag::Release)) != 0,
			  "every command has ConVarFlag::Release");
		Check((flags & static_cast<int64_t>(s2sdk::ConVarFlag::ClientCanExecute)) != 0,
			  "every command has ConVarFlag::ClientCanExecute");
		Check(command.callback != nullptr, "every command has a callback");
	}
	Check(g_listeners.empty(), "no say listener is installed for triggered chat names");

	// A command that already exists (engine or another plugin) is only listened to on unload.
	g_plugin.OnPluginEnd();
	CheckEqInt(static_cast<long long>(g_removedCommands.size()), 6,
			   "OnPluginEnd removes the six owned commands");
	Check(g_removedListeners.empty(), "no listener is removed for an owned command");

	FinishTest();
}

void TestCommandDispatch() {
	PrepareTest();
	g_configsDir = std::string(VIP_BTW_TEST_DATA_DIR) + "/primary";
	g_plugin.OnPluginStart();
	FireMapStart();

	// --- the console path: "mm_ak47" buys the CT weapon ----------------------------
	MakeBuyer(3);
	ResetCounters();
	Check(Dispatch("mm_ak47", 3, s2sdk::ConCommandContext::Console) ==
			  s2sdk::ResultType::Continue,
		  "a console command lets the engine continue (the handler returns false)");
	CheckEqInt(g_clients[3].money, 16000 - 2700, "the price is subtracted");
	CheckEqInt(static_cast<long long>(g_givenItems.size()), 1, "one weapon is given");
	if (!g_givenItems.empty()) {
		CheckEqInt(g_givenItems.front().first, 3, "the weapon goes to the buyer");
		CheckEq(g_givenItems.front().second, "weapon_ak47", "the CT weapon is given");
	}
	CheckEqInt(g_networkStateCalls, 1, "the money services are marked changed");
	Check(!g_networkStateHandles.empty() && g_networkStateHandles.front() ==
				kControllerHandleBase + 3,
		  "the state change is on the controller handle");

	// --- the chat path: "!ak47" reaches the trigger-less "ak47" command -----------
	MakeBuyer(4);
	ResetCounters();
	Check(Dispatch("ak47", 4, s2sdk::ConCommandContext::Chat) ==
			  s2sdk::ResultType::Handled,
		  "a chat command is swallowed (Pisex chat-command path)");
	CheckEqInt(g_clients[4].money, 16000 - 2700, "the chat path buys too");

	// --- the source filter ---------------------------------------------------------
	// "ak47" (the chat name) typed in the console must not answer: the console spelling is
	// "mm_ak47" only.
	MakeBuyer(5);
	ResetCounters();
	Check(Dispatch("ak47", 5, s2sdk::ConCommandContext::Console) ==
			  s2sdk::ResultType::Continue,
		  "the chat name does not answer in the console context");
	CheckEqInt(g_moneySets, 0, "nothing is bought through the console chat name");

	// "mm_ak47" in the chat context must not answer either.
	ResetCounters();
	Check(Dispatch("mm_ak47", 5, s2sdk::ConCommandContext::Chat) ==
			  s2sdk::ResultType::Continue,
		  "the console name does not answer in the chat context");
	CheckEqInt(g_moneySets, 0, "nothing is bought through the chat console name");

	// An unknown command is left alone.
	ResetCounters();
	Check(DispatchArgs("mm_ak47", 5, s2sdk::ConCommandContext::Console,
					   MakeArgs({"zzz"})) == s2sdk::ResultType::Continue,
		  "a command not in any table is ignored");
	CheckEqInt(g_moneySets, 0, "an unknown command buys nothing");

	// --- the early-outs ------------------------------------------------------------
	// The server console (caller -1) is not a player: nothing is bought and not one s2sdk client
	// export sees the slot.
	ResetCounters();
	Check(Dispatch("mm_ak47", -1, s2sdk::ConCommandContext::Console) ==
			  s2sdk::ResultType::Continue,
		  "a server-console call is ignored");
	CheckEqInt(g_moneySets, 0, "the server console buys nothing");
	CheckEqInt(ClientCalls(), 0, "no s2sdk client call is made for the server console");

	// The command must be in the player's own table: the CT player may not buy the T "m4a1".
	MakeBuyer(6);
	ResetCounters();
	Dispatch("mm_m4a1", 6, s2sdk::ConCommandContext::Console);
	CheckEqInt(g_moneySets, 0, "a T-table weapon cannot be bought by a CT");

	// Only a VIP with the feature can buy.
	g_isVIP[6] = false;
	ResetCounters();
	Dispatch("mm_ak47", 6, s2sdk::ConCommandContext::Console);
	CheckEqInt(g_moneySets, 0, "a non-VIP cannot buy");
	g_isVIP[6] = true;
	g_hasFeature[6] = false;
	ResetCounters();
	Dispatch("mm_ak47", 6, s2sdk::ConCommandContext::Console);
	CheckEqInt(g_moneySets, 0, "a VIP without the btw feature cannot buy");
	g_hasFeature[6] = true;

	// A dead client cannot buy.
	g_clients[6].alive = false;
	ResetCounters();
	Dispatch("mm_ak47", 6, s2sdk::ConCommandContext::Console);
	CheckEqInt(g_moneySets, 0, "a dead client cannot buy");
	g_clients[6].alive = true;

	// Outside the buy zone the refusal phrase is printed.
	g_clients[6].inBuyZone = false;
	ResetCounters();
	Dispatch("mm_ak47", 6, s2sdk::ConCommandContext::Console);
	CheckEqInt(g_moneySets, 0, "a client outside the buy zone cannot buy");
	CheckEqInt(static_cast<long long>(g_chatPrints.size()), 1, "the buy-zone refusal is printed");
	if (!g_chatPrints.empty()) {
		CheckEq(g_chatPrints.front().second, std::string("\x02no buyzone"),
				"the buy-zone phrase is colourised");
	}
	g_clients[6].inBuyZone = true;

	// Not enough money prints the other refusal.
	g_clients[6].money = 100;
	ResetCounters();
	Dispatch("mm_ak47", 6, s2sdk::ConCommandContext::Console);
	CheckEqInt(g_moneySets, 0, "a poor client cannot buy");
	CheckEqInt(static_cast<long long>(g_chatPrints.size()), 1, "the money refusal is printed");
	CheckEqInt(g_clients[6].money, 100, "the refused purchase costs nothing");
	g_clients[6].money = 16000;

	// Missing item / money services stop the purchase silently.
	*reinterpret_cast<void**>(g_pawnMemory[6].data() + kItemServicesOffset) = nullptr;
	ResetCounters();
	Dispatch("mm_ak47", 6, s2sdk::ConCommandContext::Console);
	CheckEqInt(g_moneySets, 0, "no item services: no purchase");
	Check(g_chatPrints.empty(), "no item services: no message");
	*reinterpret_cast<void**>(g_pawnMemory[6].data() + kItemServicesOffset) = &g_itemServices;

	*reinterpret_cast<void**>(g_controllerMemory[6].data() + kMoneyServicesOffset) = nullptr;
	ResetCounters();
	Dispatch("mm_ak47", 6, s2sdk::ConCommandContext::Console);
	CheckEqInt(g_moneySets, 0, "no money services: no purchase");
	Check(g_chatPrints.empty(), "no money services: no message");

	FinishTest();
}

// PITFALLS C1: a CEntityHandle is index | serial << 15, so a valid handle can be negative. The
// plugin must accept a negative valid controller/pawn handle end to end and reject only the -1
// (INVALID_EHANDLE_INDEX) / 0 (world) sentinels, independently of IsValidEntHandle.
void TestNegativeEntityHandles() {
	PrepareTest();
	g_configsDir = std::string(VIP_BTW_TEST_DATA_DIR) + "/primary";
	g_plugin.OnPluginStart();
	FireMapStart();

	// --- a valid negative controller/pawn handle works end to end --------------------
	MakeBuyer(8, 3); // CT: the ak47 is in the CT table
	g_clients[8].controllerHandle = -1008;
	g_clients[8].pawnHandle = -2008;

	Check(IsAlive(8), "a client with negative valid handles is alive");
	ResetCounters();
	CheckEqInt(GetPlayerTeam(8), 3, "a negative valid controller handle is accepted");
	CheckEqInt(g_controllerHandleCalls, 1, "the controller handle is validated");
	CheckEqInt(PawnHandle(8), -2008, "a negative valid pawn handle is returned unchanged");
	Check(InBuyZone(8), "the buy zone is read through a negative valid pawn handle");
	CheckEqInt(g_entDataCalls, 1, "GetEntData receives the negative pawn handle");

	ResetCounters();
	Check(Dispatch("mm_ak47", 8, s2sdk::ConCommandContext::Console) ==
			  s2sdk::ResultType::Continue,
		  "the buy command runs with negative valid handles");
	CheckEqInt(g_clients[8].money, 16000 - 2700, "the price is subtracted");
	CheckEqInt(static_cast<long long>(g_givenItems.size()), 1, "the weapon is given");
	if (!g_givenItems.empty()) {
		CheckEq(g_givenItems.front().second, "weapon_ak47", "the CT weapon is given");
	}
	CheckEqInt(g_networkStateCalls, 1, "the money-services state change is emitted");
	Check(!g_networkStateHandles.empty() && g_networkStateHandles.front() == -1008,
		  "the state change carries the negative controller handle unchanged");

	// --- 0 and -1 are sentinels, not handles ------------------------------------------
	for (const int32_t sentinel : {0, -1}) {
		MakeBuyer(9, 3);
		g_clients[9].controllerHandle = sentinel;
		g_clients[9].pawnHandle = kPawnHandleBase + 9;
		ResetCounters();
		Check(!IsAlive(9), "a sentinel controller handle is refused");
		CheckEqInt(GetPlayerTeam(9), 0, "no team through a sentinel controller handle");
		Check(g_pawnCalls == 0 && g_aliveCalls == 0,
			  "the pawn is not reached through a sentinel controller handle");
		Check(Dispatch("mm_ak47", 9, s2sdk::ConCommandContext::Console) ==
				  s2sdk::ResultType::Continue,
			  "no buy through a sentinel controller handle");
		CheckEqInt(g_moneySets, 0, "nothing is bought through a sentinel controller handle");

		MakeBuyer(10, 3);
		g_clients[10].controllerHandle = -1010;
		g_clients[10].pawnHandle = sentinel;
		ResetCounters();
		Check(!IsAlive(10), "a sentinel pawn handle is refused");
		CheckEqInt(PawnHandle(10), 0, "a sentinel pawn handle reports no pawn");
		Check(!InBuyZone(10), "the buy zone is refused for a sentinel pawn handle");
		CheckEqInt(g_entDataCalls, 0, "GetEntData is not called for a sentinel pawn handle");
		Check(Dispatch("mm_ak47", 10, s2sdk::ConCommandContext::Console) ==
				  s2sdk::ResultType::Continue,
			  "no buy through a sentinel pawn handle");
		CheckEqInt(g_moneySets, 0, "nothing is bought through a sentinel pawn handle");
	}

	// --- mutation check: a permissive IsValidEntHandle must not let 0/-1 through --------
	// If the plugin relied on IsValidEntHandle alone, this fake would turn the sentinels into
	// accepted handles and the checks below would fail.
	const s2sdk::_IsValidEntHandle savedValid = __s2sdk_IsValidEntHandle;
	__s2sdk_IsValidEntHandle = &FakeIsValidEntHandlePermissive;

	MakeBuyer(11, 3);
	g_clients[11].controllerHandle = 0;
	g_clients[11].pawnHandle = kPawnHandleBase + 11;
	ResetCounters();
	Check(!IsAlive(11), "0 is rejected even when IsValidEntHandle accepts it");
	Check(g_pawnCalls == 0 && g_aliveCalls == 0,
		  "the pawn is not reached when 0 is the controller handle");
	Check(Dispatch("mm_ak47", 11, s2sdk::ConCommandContext::Console) ==
			  s2sdk::ResultType::Continue,
		  "no buy through controller handle 0 under a permissive validator");
	CheckEqInt(g_moneySets, 0, "nothing is bought through controller handle 0");

	g_clients[11].controllerHandle = -1011;
	g_clients[11].pawnHandle = -1;
	ResetCounters();
	Check(!IsAlive(11), "-1 is rejected even when IsValidEntHandle accepts it");
	CheckEqInt(PawnHandle(11), 0, "-1 reports no pawn");
	Check(!InBuyZone(11), "the buy zone is refused for pawn handle -1");
	CheckEqInt(g_entDataCalls, 0, "GetEntData is not called for pawn handle -1");
	Check(Dispatch("mm_ak47", 11, s2sdk::ConCommandContext::Console) ==
			  s2sdk::ResultType::Continue,
		  "no buy through pawn handle -1 under a permissive validator");
	CheckEqInt(g_moneySets, 0, "nothing is bought through pawn handle -1");

	__s2sdk_IsValidEntHandle = savedValid;

	FinishTest();
}

// A chat spelling without "!"/"/" is dispatched from say/say_team, like pisexcompat did for bare
// chat words (PITFALLS C11).
void TestBareChatWord() {
	PrepareTest();
	g_plugin.OnPluginStart();
	FireMapStart();

	ResetCounters();
	RegisterCommand("test_console", "testbare", &InvokeBuy);
	Check(FindCommand("test_console") != nullptr, "the bare registration creates the console name");
	Check(FindCommand("testbare") != nullptr, "the bare chat word gets its s2sdk name");
	CheckEqInt(static_cast<long long>(g_listeners.size()), 2,
			   "say and say_team are hooked for the bare chat word");
	Check(FindListener("say") != nullptr && FindListener("say_team") != nullptr,
		  "both say listeners are recorded");

	const s2sdk::ConCommandCallback say = FindListener("say");
	Check(say != nullptr, "the say listener exists");
	if (say != nullptr) {
		// A CS2 client sends the whole message as ONE quoted argument: it is split into words.
		CheckEqInt(static_cast<long long>(say(3, s2sdk::ConCommandContext::Chat,
											  MakeArgs({"say", "testbare"}))),
				   static_cast<long long>(s2sdk::ResultType::Handled),
				   "a bare chat word is dispatched and swallowed");
		CheckEqInt(static_cast<long long>(say(3, s2sdk::ConCommandContext::Chat,
											  MakeArgs({"say", "testBARE extra"}))),
				   static_cast<long long>(s2sdk::ResultType::Handled),
				   "the word is matched case-insensitively inside a quoted message");
		CheckEqInt(static_cast<long long>(say(3, s2sdk::ConCommandContext::Chat,
											  MakeArgs({"say", "!testbare"}))),
				   static_cast<long long>(s2sdk::ResultType::Continue),
				   "a triggered message stays on the s2sdk chat-command path");
		CheckEqInt(static_cast<long long>(say(3, s2sdk::ConCommandContext::Chat,
											  MakeArgs({"say", "hello"}))),
				   static_cast<long long>(s2sdk::ResultType::Continue),
				   "an unregistered word is left alone");
		CheckEqInt(static_cast<long long>(say(3, s2sdk::ConCommandContext::Chat,
											  MakeArgs({"say"}))),
				   static_cast<long long>(s2sdk::ResultType::Continue),
				   "an empty say line is left alone");
	}

	// OnPluginEnd removes the extra command and both say listeners.
	g_plugin.OnPluginEnd();
	Check(std::find(g_removedCommands.begin(), g_removedCommands.end(), "test_console") !=
			  g_removedCommands.end(),
		  "the extra console command is removed");
	Check(std::find(g_removedCommands.begin(), g_removedCommands.end(), "testbare") !=
			  g_removedCommands.end(),
		  "the bare chat command is removed");
	CheckEqInt(static_cast<long long>(g_removedListeners.size()), 2,
			   "both say listeners are removed");
	g_listeners.clear();
	g_removedCommands.clear();
	g_removedListeners.clear();

	FinishTest();
}

// A name that already exists in the engine is owned by someone else: unload must remove only the
// listener, never RemoveCommand (PITFALLS A7).
void TestExistingCommandName() {
	PrepareTest();
	g_configsDir = std::string(VIP_BTW_TEST_DATA_DIR) + "/fallback";
	g_existingCommands.push_back("mm_awp");
	g_plugin.OnPluginStart();

	g_plugin.OnPluginEnd();
	Check(std::find(g_removedCommands.begin(), g_removedCommands.end(), "mm_awp") ==
			  g_removedCommands.end(),
		  "RemoveCommand is not called for a name the plugin does not own");
	Check(std::find(g_removedListeners.begin(), g_removedListeners.end(), "mm_awp") !=
			  g_removedListeners.end(),
		  "RemoveCommandListener detaches the listener of a pre-existing name");
	Check(std::find(g_removedCommands.begin(), g_removedCommands.end(), "awp") !=
			  g_removedCommands.end(),
		  "the owned chat name is still removed with RemoveCommand");

	FinishTest();
}

// --- I. the round_start hook (PITFALLS C12) ---------------------------------------------------

void TestRoundStart() {
	PrepareTest();
	g_configsDir = std::string(VIP_BTW_TEST_DATA_DIR) + "/primary";
	g_plugin.OnPluginStart();

	CheckEq(g_hookedEvent, "round_start", "OnPluginStart hooks round_start");
	Check(g_eventMode == s2sdk::HookMode::Pre, "the hook is a Pre hook");
	Check(g_eventCallback != nullptr, "the round_start callback is recorded");
	Check(std::find(g_registeredFeatures.begin(), g_registeredFeatures.end(), "btw") !=
			  g_registeredFeatures.end(),
		  "the btw feature is registered");
	Check(g_registeredValueType == vip::ValueType::VIP_BOOL, "btw is a boolean feature");
	Check(g_registeredFeatureType == vip::FeatureType::HIDE, "btw is hidden");

	FireMapStart();

	// slot 3: CT VIP with the feature -> the CT table; slot 4: T VIP -> the T table;
	// slot 5: VIP without the feature; slot 6: not a VIP; slot 7: a dead VIP.
	MakeBuyer(3, 3);
	MakeBuyer(4, 2);
	g_isVIP[5] = true;
	g_hasFeature[5] = false;
	MakeBuyer(7, 3);
	g_clients[7].alive = false;

	ResetCounters();
	if (g_eventCallback != nullptr) {
		g_eventCallback(ToPlg("round_start"), nullptr, false);
	}
	CheckEqInt(static_cast<long long>(g_chatPrints.size()), 2,
			   "the round-start message goes to the two VIPs with the feature and a live pawn");
	if (g_chatPrints.size() == 2) {
		CheckEqInt(g_chatPrints[0].first, 3, "the CT VIP is the first message");
		CheckEq(g_chatPrints[0].second, "Weapons: ak47", "the CT VIP sees the CT table");
		CheckEqInt(g_chatPrints[1].first, 4, "the T VIP is the second message");
		CheckEq(g_chatPrints[1].second, "Weapons: m4a1, m4a4", "the T VIP sees the T table");
	}
	Check(std::find(g_translateCalls.begin(), g_translateCalls.end(), "btw_round_start") !=
			  g_translateCalls.end(),
		  "the round-start phrase is requested");

	// PITFALLS C12: OnPluginEnd unhooks the exact callback it registered.
	g_plugin.OnPluginEnd();
	CheckEqInt(g_unhookCalls, 1, "OnPluginEnd unhooks round_start once");
	Check(g_eventCallback == nullptr, "the registered round_start callback is gone");

	FinishTest();
}

// --- J. a clean unload / reload (PITFALLS A7/C12) ---------------------------------------------

void TestCleanReload() {
	PrepareTest();
	g_configsDir = std::string(VIP_BTW_TEST_DATA_DIR) + "/primary";

	for (int round = 0; round < 2; ++round) {
		g_plugin.OnPluginStart();
		FireMapStart();
		Check(g_mapReady, "the gate is open after a start");
		CheckEqInt(g_mapStartRegisters, 1, "the map listeners are registered exactly once");
		g_plugin.OnPluginEnd();
		Check(!g_mapReady, "OnPluginEnd closes the gate");
		CheckEqInt(g_mapStartUnregisters, 1, "OnPluginEnd unregisters the map listener");
		CheckEqInt(g_mapEndUnregisters, 1, "OnPluginEnd unregisters the map-end listener");
		Check(g_roundStartHooked == false, "OnPluginEnd clears the hook flag");

		// Reset the registration counters between the two rounds; the plugin state is checked in
		// the second round (a reload must not leave anything behind or register twice).
		ResetWorld();
		g_configsDir = std::string(VIP_BTW_TEST_DATA_DIR) + "/primary";
	}

	// The last round left the plugin stopped; nothing is registered any more.
	Check(g_listeners.empty(), "no say listeners survive the unload");
	Check(g_plugin.OnPluginEnd().GetCode() == plg::PluginCode::Ok,
		  "OnPluginEnd is idempotent");

	FinishTest();
}

} // namespace

int main() {
	std::printf("vip_btw tests\n");

	InstallFakes();
	ResetWorld();

	TestPureHelpers();
	TestKv1();
	TestMapGate();
	TestMapChangeWindowStart();
	TestNoMapListeners();
	TestClientGuards();
	TestSchemaCache();
	TestOutput();
	TestConfigResolution();
	TestCommandRegistration();
	TestCommandDispatch();
	TestNegativeEntityHandles();
	TestBareChatWord();
	TestExistingCommandName();
	TestRoundStart();
	TestCleanReload();

	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
