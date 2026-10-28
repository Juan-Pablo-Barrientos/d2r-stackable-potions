#include <D2RLPlugin/console.h>
#include <D2RLPlugin/context.h>
#include <D2RLPlugin/data_tables.h>
#include <D2RLPlugin/inventory.h>
#include <D2RLPlugin/item.h>
#include <D2RLPlugin/lifecycle_events.h>
#include <D2RLPlugin/threads.h>
#include <D2RLPlugin/version.h>
#include <windows.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

// Companion to the Stackable Potions data mod, which makes potions stackable
// like keys. D2R drinks a potion by deleting the whole item, so drinking from a
// stack of 5 would use all 5. This plugin puts the other 4 back where the stack
// was. It also merges loose inventory potions into stacks. D2R never stacks
// potions saved before the mod (compact items with no quantity) or potions
// bought from a vendor on its own.
//
// Item changes go through the item service on the authoritative game thread,
// so they work in local games and when hosting, not as a remote client.

static constexpr D2RL::PluginInfo StackablePotionsPluginInfo {
	.infoSize    = D2RL::PluginInfoSize,
	.abiVersion  = D2RL_PLUGIN_ABI_VERSION,
	.id          = "stackable-potions",
	.name        = "Stackable Potions",
	.version     = "0.2.0",
	.author      = "Juan",
	.description = "Drinking from a potion stack uses one potion, and loose potions merge into stacks.",
	.flags       = D2RL::PluginFlags::Client,
};

static constexpr char DefaultConfig[] = R"(# Stackable Potions
# Needs the Stackable Potions data mod, which makes potions stack like keys.

[stackable-potions]
# Drinking from a stack uses one potion. The rest of the stack stays.
enabled = true

# Merge loose potions in the inventory into stacks, such as potions you had
# before installing the mod.
merge_loose = true

# Write every restored stack and merge to the plugin log.
log_actions = false
)";

// D2RCore wraps D2R's client-to-server send with this function. It is a private
// export, so its prologue is checked before use. Without it, a drunk belt stack
// goes back to the inventory instead of the belt.
using SendClientGameplayPacketFn = void(__cdecl*)(const uint8_t* data, uint32_t size) noexcept;

static constexpr uint8_t SendClientGameplayPacketPrologue[] = { 0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x56, 0x57, 0x53, 0x48, 0x83, 0xEC, 0x50 };
static constexpr uint8_t PacketCursorToBeltSlot             = 0x23; // [id][itemId u32][slot u32], 9 bytes

static constexpr int BeltColumns = 4;
static constexpr int BeltRows    = 4;
static constexpr int BeltSlots   = BeltColumns * BeltRows;
static constexpr int MaxPotions  = 512;
// D2's native body location for the belt.
static constexpr int32_t BeltBodyLocation = 8;

// Offsets from an Items row's code, checked against the key row at load.
static constexpr uint32_t MinStackAfterCode   = 108;
static constexpr uint32_t MaxStackAfterCode   = 112;
static constexpr uint32_t SpawnStackAfterCode = 116;

// An inventory stack only counts as drunk when it vanishes this soon after a
// plain right-click. Selling (left or Ctrl click) never deletes it this way.
static constexpr ULONGLONG RightClickWindowMs = 2'000;
// Merging waits until the inventory has been still for a moment, so it never
// competes with a manual move, Potion Refill or Inventory Sort.
static constexpr ULONGLONG MergeSettleMs    = 1'500;
static constexpr ULONGLONG GameQueueRetryMs = 2'000;
// A belt stack that cannot get the cursor this long goes to the inventory.
static constexpr ULONGLONG CursorWaitMs     = 1'000;

struct Config {
	bool enabled    = true;
	bool mergeLoose = true;
	bool logActions = false;
};

struct PotionCode {
	uint32_t code;
};

#define POTION(c) { D2RL::Items::MakeItemCode(c) }
static constexpr PotionCode PotionCodes[] = {
	POTION("hp1"), POTION("hp2"), POTION("hp3"), POTION("hp4"), POTION("hp5"), POTION("mp1"), POTION("mp2"), POTION("mp3"), POTION("mp4"), POTION("mp5"),
	POTION("rvs"), POTION("rvl"), POTION("vps"), POTION("yps"), POTION("wms"), POTION("hpo"), POTION("mpo"), POTION("hpf"), POTION("mpf"),
};
#undef POTION
static constexpr int PotionCodeCount = static_cast<int>(sizeof(PotionCodes) / sizeof(PotionCodes[0]));

struct StoredPotion {
	D2RL::ItemHandle           handle    = D2RL::InvalidItemHandle;
	uint32_t                   runtimeId = 0;
	uint32_t                   code      = 0;
	D2RL::Items::ItemContainer container = D2RL::Items::ItemContainer::Unknown;
	int32_t                    x         = 0;
	int32_t                    y         = 0;
	int32_t                    quantity  = 0;
	uint32_t                   itemLevel = 0;
};

struct Snapshot {
	StoredPotion potions[MaxPotions] {};
	int          count = 0;
	bool         overflow = false;
	bool         beltUsed[BeltSlots] {};
	bool         cursorUsed = false;
	// The equipped belt's runtime id, 0 without a belt.
	uint32_t     beltId     = 0;
};

// A drunk belt stack waiting for the cursor. Only one restored stack can be on
// the cursor at a time, so drinks from several columns are put back one by one.
struct PendingRestore {
	StoredPotion drunk;
	int          maxRow = 0;
	ULONGLONG    since  = 0;
};

static const D2RL::PluginContext*    plugin;
static const D2RL::InventoryService* inventory;
static const D2RL::ItemService*      items;
static const D2RL::ThreadService*    threads;
static const D2RL::DataTableService* dataTables;
static SendClientGameplayPacketFn    sendPacket;
static Config                        config;

// A restored belt stack is created on the cursor by the game thread, then the
// UI thread asks the server to put it in this belt slot.
struct BeltPlacement {
	uint32_t runtimeId;
	uint32_t slot;
};
static BeltPlacement beltPlacement;

static std::atomic<D2RL::PlayerHandle> localPlayer { D2RL::InvalidPlayerHandle };
static std::atomic<bool>               limitsStale { true };
static std::atomic<bool>               gameTickQueued;
static std::atomic<ULONGLONG>          gameTickQueuedAt;
static bool                            uiLoopRunning;
static bool                            rightButtonDown;
static bool                            leftButtonDown;
static std::atomic<ULONGLONG>          rightClickAt;
static std::atomic<ULONGLONG>          leftClickAt;

// Stack limits per potion code, from the active Items table. 0 or 1 means the
// data mod is not installed for that potion and the plugin leaves it alone.
static uint32_t stackLimits[PotionCodeCount];

static bool               haveSnapshot;
static D2RL::PlayerHandle snapshotPlayer = D2RL::InvalidPlayerHandle;
static Snapshot           previous;
static Snapshot           current;
static uint64_t           inventorySignature;
static ULONGLONG          inventoryChangedAt;
static uint64_t           failedMergeSignature;
// Potions removed by the last merge. They leave the inventory over the next few
// updates and must not look like drinks.
static uint32_t           mergedIds[D2RL::Items::MaxTransactionInputs];
static uint32_t           mergedIdCount;
static bool               beltSeen;
static PendingRestore     pendingRestores[BeltSlots];
static int                pendingRestoreCount;

static void Logf(bool warning, const char* format, ...) noexcept {
	char    message[256] {};
	va_list args;
	va_start(args, format);
	std::vsnprintf(message, sizeof(message), format, args);
	va_end(args);
	if (warning) {
		plugin->LogWarn(message);
	} else {
		plugin->LogInfo(message);
	}
}

static auto CodeText(uint32_t code, char (&text)[5]) noexcept -> const char* {
	std::memcpy(text, &code, 4);
	text[4] = 0;
	return text;
}

static auto ReadU32(const uint8_t* bytes) noexcept -> uint32_t {
	uint32_t value = 0;
	std::memcpy(&value, bytes, 4);
	return value;
}

static auto PotionIndex(uint32_t code) noexcept -> int {
	for (int index = 0; index < PotionCodeCount; ++index) {
		if (PotionCodes[index].code == code) {
			return index;
		}
	}
	return -1;
}

static auto StackLimit(uint32_t code) noexcept -> uint32_t {
	const int index = PotionIndex(code);
	return index >= 0 ? stackLimits[index] : 0;
}

// Native belt items store the slot as x. Accept a column/row pair as well.
static auto BeltSlot(int32_t x, int32_t y) noexcept -> int {
	const int slot = y > 0 ? y * BeltColumns + x : x;
	return slot >= 0 && slot < BeltSlots ? slot : -1;
}

// Compact potions saved before the mod have no quantity stat.
static auto LogicalQuantity(const StoredPotion& potion) noexcept -> uint32_t {
	return potion.quantity > 0 ? static_cast<uint32_t>(potion.quantity) : 1;
}

// ---- Configuration --------------------------------------------------------

static auto TrimLeft(const char* text) noexcept -> const char* {
	while (*text == ' ' || *text == '\t') {
		++text;
	}
	return text;
}

static auto ReadValue(const char* line, const char* key) noexcept -> const char* {
	const size_t length = std::strlen(key);
	if (std::strncmp(line, key, length) != 0) {
		return nullptr;
	}
	const char* rest = TrimLeft(line + length);
	return *rest == '=' ? TrimLeft(rest + 1) : nullptr;
}

static void LoadConfig() noexcept {
	config = {};
	char text[4'096] {};
	if (!plugin->EnsureConfig(DefaultConfig)) {
		plugin->LogWarn("Could not create the default config. Using defaults.");
		return;
	}
	if (!plugin->ReadConfig(text, sizeof(text))) {
		plugin->LogWarn("Could not read the config. Using defaults.");
		return;
	}
	char* context = nullptr;
	for (char* line = strtok_s(text, "\r\n", &context); line != nullptr; line = strtok_s(nullptr, "\r\n", &context)) {
		line = const_cast<char*>(TrimLeft(line));
		if (const char* enabled = ReadValue(line, "enabled")) {
			config.enabled = std::strncmp(enabled, "false", 5) != 0;
		} else if (const char* merge = ReadValue(line, "merge_loose")) {
			config.mergeLoose = std::strncmp(merge, "false", 5) != 0;
		} else if (const char* logActions = ReadValue(line, "log_actions")) {
			config.logActions = std::strncmp(logActions, "true", 4) == 0;
		}
	}
}

// ---- Packets --------------------------------------------------------------

static auto ResolveSendPacket() noexcept -> SendClientGameplayPacketFn {
	const HMODULE core    = GetModuleHandleW(L"D2RCore.dll");
	const auto    address = core != nullptr ? reinterpret_cast<const uint8_t*>(GetProcAddress(core, "SendClientGameplayPacket")) : nullptr;
	if (address == nullptr || std::memcmp(address, SendClientGameplayPacketPrologue, sizeof(SendClientGameplayPacketPrologue)) != 0) {
		plugin->LogWarn("SendClientGameplayPacket is missing or changed in this D2RLoader build. Drunk belt stacks go back to the inventory.");
		return nullptr;
	}
	return reinterpret_cast<SendClientGameplayPacketFn>(address);
}

static void __cdecl SendCursorToBelt(const D2RL::PluginContext*, void* userData) noexcept {
	const auto* placement = static_cast<const BeltPlacement*>(userData);
	uint8_t     packet[9] { PacketCursorToBeltSlot };
	std::memcpy(packet + 1, &placement->runtimeId, 4);
	std::memcpy(packet + 5, &placement->slot, 4);
	sendPacket(packet, sizeof(packet));
}

// ---- Stack limits ---------------------------------------------------------

// Reads maxstack for every potion from one bank's Items table. The field
// offsets are checked against the key row, which is stackable in every game
// version (minstack 1, maxstack and spawnstack above zero).
static auto ReadStackLimits(D2RL::DataTables::Bank bank) noexcept -> bool {
	using namespace D2RL::DataTables;

	TableView table { .structSize = TableViewSize, .bank = bank };
	if (dataTables->getTable(plugin, bank, TableId::Items, &table) != Result::Success || table.rows == nullptr || table.rowSize < SpawnStackAfterCode + 4) {
		return false;
	}
	const uint32_t key = D2RL::Items::MakeItemCode("key");
	RowView        row { .structSize = RowViewSize, .bank = bank };
	if (dataTables->findRowByCode(plugin, bank, TableId::Items, key, &row) != Result::Success || row.row == nullptr || row.rowSize != table.rowSize) {
		return false;
	}
	const auto* keyRow     = static_cast<const uint8_t*>(row.row);
	int         codeOffset = -1;
	for (uint32_t offset = 0; offset + SpawnStackAfterCode + 4 <= table.rowSize && codeOffset < 0; ++offset) {
		if (ReadU32(keyRow + offset) == key) {
			codeOffset = static_cast<int>(offset);
		}
	}
	if (codeOffset < 0 || ReadU32(keyRow + codeOffset + MinStackAfterCode) != 1 || ReadU32(keyRow + codeOffset + MaxStackAfterCode) == 0
	    || ReadU32(keyRow + codeOffset + SpawnStackAfterCode) == 0) {
		Logf(true, "The Items table layout changed (bank %u). Stackable Potions is disabled until it is updated.", static_cast<uint32_t>(bank));
		return false;
	}

	for (int index = 0; index < PotionCodeCount; ++index) {
		RowView potion { .structSize = RowViewSize, .bank = bank };
		stackLimits[index] = 0;
		if (dataTables->findRowByCode(plugin, bank, TableId::Items, PotionCodes[index].code, &potion) == Result::Success && potion.row != nullptr && potion.rowSize == table.rowSize) {
			const uint32_t limit = ReadU32(static_cast<const uint8_t*>(potion.row) + codeOffset + MaxStackAfterCode);
			stackLimits[index]   = limit <= 511 ? limit : 0;
		}
	}
	return true;
}

static void RefreshStackLimits() noexcept {
	if (!limitsStale.exchange(false)) {
		return;
	}
	for (const auto bank : { D2RL::DataTables::Bank::Rotw, D2RL::DataTables::Bank::Lod }) {
		if (ReadStackLimits(bank)) {
			int stackable = 0;
			for (const uint32_t limit : stackLimits) {
				stackable += limit > 1 ? 1 : 0;
			}
			Logf(stackable == 0, "%d of %d potions stack (Super Healing: %u)%s", stackable, PotionCodeCount, StackLimit(D2RL::Items::MakeItemCode("hp5")),
			     stackable == 0 ? ". Install the Stackable Potions data mod to make them stack." : ".");
			return;
		}
	}
	std::memset(stackLimits, 0, sizeof(stackLimits));
}

static void __cdecl OnDataTablesLoaded(const D2RL::PluginContext*, const D2RL::Lifecycle::DataTablesLoadedEvent*, void*) noexcept {
	limitsStale = true;
}

// ---- Right-clicks ---------------------------------------------------------

// D2R drinks an inventory potion on right-click, but item-interaction events
// do not report that click. The UI loop watches the mouse instead and records
// plain right-clicks made while the game window is in front. Ctrl, Shift and
// Alt clicks move or sell items in D2R, so they never count. Left clicks are
// recorded too: a left click after the right-click (picking an item up, a
// Ctrl+click sale) means the right-click was not what removed the stack.
static void WatchMouse(ULONGLONG time) noexcept {
	const bool rightDown = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
	const bool leftDown  = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
	if (leftDown && !leftButtonDown) {
		leftClickAt = time;
	}
	if (rightDown && !rightButtonDown) {
		DWORD      process    = 0;
		const HWND foreground = GetForegroundWindow();
		const bool modifier   = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 || (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0 || (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
		const bool inGame     = foreground != nullptr && GetWindowThreadProcessId(foreground, &process) != 0 && process == GetCurrentProcessId();
		// A modified or out-of-game right-click cancels an earlier plain one.
		rightClickAt = inGame && !modifier ? time : 0;
	}
	rightButtonDown = rightDown;
	leftButtonDown  = leftDown;
}

static auto RecentRightClick(ULONGLONG time) noexcept -> bool {
	const ULONGLONG clicked = rightClickAt.load();
	return clicked != 0 && time - clicked <= RightClickWindowMs && clicked > leftClickAt.load();
}

static auto WasMerged(uint32_t runtimeId) noexcept -> bool {
	for (uint32_t index = 0; index < mergedIdCount; ++index) {
		if (mergedIds[index] == runtimeId) {
			return true;
		}
	}
	return false;
}

// ---- Snapshots ------------------------------------------------------------

static void Store(Snapshot* snapshot, const D2RL::Items::ItemInfo* item, D2RL::Items::ItemContainer container) noexcept {
	if (container == D2RL::Items::ItemContainer::Belt) {
		const int slot = BeltSlot(item->x, item->y);
		if (slot >= 0) {
			snapshot->beltUsed[slot] = true;
		}
	}
	if (PotionIndex(item->code) < 0) {
		return;
	}
	if (snapshot->count == MaxPotions) {
		snapshot->overflow = true;
		return;
	}
	snapshot->potions[snapshot->count++] = { item->handle, item->runtimeId, item->code, container, item->x, item->y, item->quantity, item->itemLevel };
}

static auto __cdecl CaptureItem(const D2RL::PluginContext*, const D2RL::Items::ItemInfo* item, void* userData) noexcept -> D2RL::Inventory::IterationAction {
	auto* snapshot = static_cast<Snapshot*>(userData);
	if (snapshot == nullptr || item == nullptr || item->structSize < D2RL::Items::ItemInfoRequiredSize) {
		return D2RL::Inventory::IterationAction::Stop;
	}
	Store(snapshot, item, item->container);
	return D2RL::Inventory::IterationAction::Continue;
}

// Everywhere a potion can go without being destroyed, so a move between
// containers is never mistaken for a drink.
static auto Capture(D2RL::PlayerHandle player, Snapshot* snapshot) noexcept -> bool {
	using D2RL::Items::ContainerBit;
	using D2RL::Items::ItemContainer;

	snapshot->count      = 0;
	snapshot->overflow   = false;
	snapshot->cursorUsed = false;
	std::memset(snapshot->beltUsed, 0, sizeof(snapshot->beltUsed));

	const uint32_t masks[] = {
		ContainerBit(ItemContainer::Belt) | ContainerBit(ItemContainer::Inventory) | ContainerBit(ItemContainer::Cube) | ContainerBit(ItemContainer::PersonalStash)
			| ContainerBit(ItemContainer::SharedStash) | ContainerBit(ItemContainer::CustomPage),
		ContainerBit(ItemContainer::Belt) | ContainerBit(ItemContainer::Inventory) | ContainerBit(ItemContainer::Cube) | ContainerBit(ItemContainer::PersonalStash),
	};
	bool read = false;
	for (const uint32_t mask : masks) {
		const D2RL::Inventory::ItemFilter filter { .structSize = D2RL::Inventory::ItemFilterSize, .containerMask = mask };
		snapshot->count = 0;
		if (inventory->forEachInventoryItem(plugin, player, &filter, CaptureItem, snapshot) == D2RL::Inventory::Result::Success) {
			read = true;
			break;
		}
	}
	if (!read) {
		return false;
	}

	D2RL::ItemHandle belt = D2RL::InvalidItemHandle;
	snapshot->beltId      = 0;
	if (inventory->getEquippedItem(plugin, player, BeltBodyLocation, &belt) == D2RL::Inventory::Result::Success && belt != D2RL::InvalidItemHandle) {
		D2RL::Items::ItemInfo info { .structSize = D2RL::Items::ItemInfoSize };
		if (items->getItemInfo(plugin, belt, &info) == D2RL::Items::Result::Success) {
			snapshot->beltId = info.runtimeId;
		}
	}

	D2RL::ItemHandle cursor = D2RL::InvalidItemHandle;
	if (inventory->getCursorItem(plugin, player, &cursor) == D2RL::Inventory::Result::Success && cursor != D2RL::InvalidItemHandle) {
		D2RL::Items::ItemInfo info { .structSize = D2RL::Items::ItemInfoSize };
		snapshot->cursorUsed = true;
		if (items->getItemInfo(plugin, cursor, &info) == D2RL::Items::Result::Success) {
			Store(snapshot, &info, D2RL::Items::ItemContainer::Cursor);
		}
	}
	return true;
}

static auto Find(const Snapshot& snapshot, uint32_t runtimeId) noexcept -> const StoredPotion* {
	for (int index = 0; index < snapshot.count; ++index) {
		if (snapshot.potions[index].runtimeId == runtimeId) {
			return &snapshot.potions[index];
		}
	}
	return nullptr;
}

static auto InventorySignature(const Snapshot& snapshot) noexcept -> uint64_t {
	uint64_t hash = 1'469'598'103'934'665'603ULL;
	for (int index = 0; index < snapshot.count; ++index) {
		const StoredPotion& potion = snapshot.potions[index];
		if (potion.container == D2RL::Items::ItemContainer::Inventory) {
			hash = (hash ^ potion.runtimeId) * 1'099'511'628'211ULL;
			hash = (hash ^ static_cast<uint32_t>(potion.quantity)) * 1'099'511'628'211ULL;
		}
	}
	return hash;
}

// ---- Restoring a drunk stack ----------------------------------------------

static auto CreateStack(D2RL::PlayerHandle player, const StoredPotion& potion, uint32_t quantity, D2RL::Items::ItemContainer container, D2RL::Items::Placement placement, int32_t x, int32_t y,
                        D2RL::ItemHandle* created = nullptr) noexcept -> D2RL::Items::Result {
	const D2RL::Items::ItemCreateSpec spec {
		.structSize      = D2RL::Items::ItemCreateSpecSize,
		.code            = potion.code,
		.quality         = D2RL::Items::Quality::Normal,
		.qualityRecordId = D2RL::Items::RandomQualityRecord,
		.itemLevel       = potion.itemLevel != 0 ? potion.itemLevel : 1,
		.seedMode        = D2RL::Items::SeedMode::Random,
		.quantity        = quantity,
		.durability      = D2RL::Items::DefaultValue,
		.stateFlags      = D2RL::Items::ItemStateIdentified,
		.destination     = {
			.structSize = D2RL::Items::ItemDestinationSize,
			.container  = container,
			.placement  = placement,
			.x          = static_cast<uint32_t>(x),
			.y          = static_cast<uint32_t>(y),
		},
	};
	D2RL::ItemHandle handle = D2RL::InvalidItemHandle;
	const auto       result = items->createItem(plugin, player, &spec, created != nullptr ? created : &handle);
	if (config.logActions && result != D2RL::Items::Result::Success) {
		char code[5] {};
		Logf(true, "create %u %s in container %u at %d,%d failed (result %u).", quantity, CodeText(potion.code, code), static_cast<uint32_t>(container), x, y, static_cast<uint32_t>(result));
	}
	return result;
}

// The highest row of a belt column that holds a potion.
static auto TopRow(const Snapshot& snapshot, int column) noexcept -> int {
	int maxRow = 0;
	for (int row = 0; row < BeltRows; ++row) {
		maxRow = snapshot.beltUsed[row * BeltColumns + column] ? row : maxRow;
	}
	return maxRow;
}

// A free belt slot in the drunk potion's column that the belt is known to have:
// the drunk slot itself, or the lowest free row up to the highest row that held
// a potion before the drink (the column shifts down after a drink).
static auto FreeBeltSlot(const StoredPotion& drunk, int maxRow, const Snapshot& now) noexcept -> int {
	const int drunkSlot = BeltSlot(drunk.x, drunk.y);
	if (drunkSlot < 0) {
		return -1;
	}
	if (!now.beltUsed[drunkSlot]) {
		return drunkSlot;
	}
	const int column = drunkSlot % BeltColumns;
	for (int row = 0; row <= maxRow; ++row) {
		if (!now.beltUsed[row * BeltColumns + column]) {
			return row * BeltColumns + column;
		}
	}
	return -1;
}

// The item service cannot create items in the belt, so a drunk belt stack is
// created on the cursor and the server is asked to put it in a free slot of the
// same column. A drunk inventory stack goes back to the cell it used. Returns
// true when the restored stack took the cursor.
static auto RestoreStack(D2RL::PlayerHandle player, const StoredPotion& drunk, int maxRow, const Snapshot& now, bool cursorFree) noexcept -> bool {
	using D2RL::Items::ItemContainer;
	using D2RL::Items::Placement;

	const uint32_t quantity = static_cast<uint32_t>(drunk.quantity - 1);
	char           code[5] {};
	auto           result = D2RL::Items::Result::Unsupported;
	const char*    where  = "";
	bool           onCursor = false;

	if (drunk.container == ItemContainer::Belt) {
		const int slot = FreeBeltSlot(drunk, maxRow, now);
		if (slot >= 0 && sendPacket != nullptr && cursorFree) {
			D2RL::ItemHandle handle = D2RL::InvalidItemHandle;
			result                  = CreateStack(player, drunk, quantity, ItemContainer::Cursor, Placement::Automatic, 0, 0, &handle);
			onCursor                = result == D2RL::Items::Result::Success;
			D2RL::Items::ItemInfo info { .structSize = D2RL::Items::ItemInfoSize };
			if (result == D2RL::Items::Result::Success && items->getItemInfo(plugin, handle, &info) == D2RL::Items::Result::Success) {
				beltPlacement = { info.runtimeId, static_cast<uint32_t>(slot) };
				(void)threads->runOnUiThread(plugin, SendCursorToBelt, &beltPlacement);
				where = "belt";
			}
		}
	} else {
		result = CreateStack(player, drunk, quantity, ItemContainer::Inventory, Placement::Exact, drunk.x, drunk.y);
		where  = "inventory";
	}
	if (result != D2RL::Items::Result::Success) {
		result = CreateStack(player, drunk, quantity, ItemContainer::Inventory, Placement::Automatic, 0, 0);
		where  = "inventory";
	}
	if (result != D2RL::Items::Result::Success) {
		result = CreateStack(player, drunk, quantity, ItemContainer::Cube, Placement::Automatic, 0, 0);
		where  = "Cube";
	}

	if (result != D2RL::Items::Result::Success) {
		Logf(true, "Could not put back %u %s after a drink (result %u). Make room in the inventory.", quantity, CodeText(drunk.code, code), static_cast<uint32_t>(result));
	} else if (config.logActions) {
		Logf(false, "Drank one %s from a stack of %d; put %u back in the %s.", CodeText(drunk.code, code), drunk.quantity, quantity, where);
	}
	return onCursor;
}

static auto IsDrunkStack(const StoredPotion& potion) noexcept -> bool {
	return potion.quantity >= 2 && StackLimit(potion.code) > 1;
}

// Potions that vanished from the belt or inventory without showing up anywhere
// else. Each belt hotkey drinks from its own column, so pressing 1 and 2
// together can empty two columns in one update. Anything else vanishing at the
// same time, or the belt itself leaving (death, removing or swapping it), means
// it was not a drink. An inventory stack also needs a plain right-click.
// Returns true when a stack is being restored.
static auto DetectDrink(D2RL::PlayerHandle player, const Snapshot& now, ULONGLONG time) noexcept -> bool {
	using D2RL::Items::ItemContainer;

	if (previous.overflow || now.overflow) {
		return false;
	}
	const StoredPotion* vanished[BeltColumns] {};
	int                 vanishedCount = 0;
	for (int index = 0; index < previous.count; ++index) {
		const StoredPotion& potion = previous.potions[index];
		if (Find(now, potion.runtimeId) != nullptr || WasMerged(potion.runtimeId)) {
			continue;
		}
		if (vanishedCount == BeltColumns) {
			return false;
		}
		vanished[vanishedCount++] = &potion;
	}
	if (vanishedCount == 0) {
		return false;
	}

	if (vanishedCount == 1 && vanished[0]->container == ItemContainer::Inventory) {
		if (!IsDrunkStack(*vanished[0]) || !RecentRightClick(time)) {
			return false;
		}
		RestoreStack(player, *vanished[0], 0, now, false);
		return true;
	}

	bool columnUsed[BeltColumns] {};
	for (int index = 0; index < vanishedCount; ++index) {
		const int slot = vanished[index]->container == ItemContainer::Belt ? BeltSlot(vanished[index]->x, vanished[index]->y) : -1;
		if (slot < 0 || columnUsed[slot % BeltColumns] || previous.beltId != now.beltId) {
			if (config.logActions && vanishedCount > 1) {
				Logf(false, "%d potions vanished at once, not from separate belt columns. Not a drink.", vanishedCount);
			}
			return false;
		}
		columnUsed[slot % BeltColumns] = true;
	}

	bool queued = false;
	for (int index = 0; index < vanishedCount; ++index) {
		const StoredPotion& drunk = *vanished[index];
		if (!IsDrunkStack(drunk)) {
			continue;
		}
		const int maxRow = TopRow(previous, BeltSlot(drunk.x, drunk.y) % BeltColumns);
		if (pendingRestoreCount == BeltSlots) {
			RestoreStack(player, drunk, maxRow, now, false);
		} else {
			pendingRestores[pendingRestoreCount++] = { drunk, maxRow, time };
		}
		queued = true;
	}
	return queued;
}

// Puts back the queued belt stacks, one per update while the cursor is free.
// Returns true while stacks are still waiting.
static auto RestorePending(D2RL::PlayerHandle player, const Snapshot& now, ULONGLONG time) noexcept -> bool {
	bool cursorFree = !now.cursorUsed;
	while (pendingRestoreCount > 0) {
		const PendingRestore next = pendingRestores[0];
		if (!cursorFree && time - next.since < CursorWaitMs) {
			return true;
		}
		for (int index = 1; index < pendingRestoreCount; ++index) {
			pendingRestores[index - 1] = pendingRestores[index];
		}
		--pendingRestoreCount;
		if (RestoreStack(player, next.drunk, next.maxRow, now, cursorFree)) {
			cursorFree = false;
		}
	}
	return false;
}

// ---- Merging loose potions ------------------------------------------------

// Merges one kind of inventory potion when its items fit in fewer stacks. The
// transaction removes the old items and creates the stacks in one step, or
// changes nothing.
static void MergeLoosePotions(D2RL::PlayerHandle player, const Snapshot& now, ULONGLONG time) noexcept {
	if (now.cursorUsed || now.overflow || time - inventoryChangedAt < MergeSettleMs || inventorySignature == failedMergeSignature) {
		return;
	}
	for (int kind = 0; kind < PotionCodeCount; ++kind) {
		const uint32_t limit = stackLimits[kind];
		if (limit < 2) {
			continue;
		}
		D2RL::Items::TransactionInput inputs[D2RL::Items::MaxTransactionInputs] {};
		uint32_t                      inputIds[D2RL::Items::MaxTransactionInputs] {};
		uint32_t                      inputCount = 0;
		uint32_t                      total      = 0;
		const StoredPotion*           first      = nullptr;
		for (int index = 0; index < now.count && inputCount < D2RL::Items::MaxTransactionInputs; ++index) {
			const StoredPotion& potion = now.potions[index];
			if (potion.container != D2RL::Items::ItemContainer::Inventory || potion.code != PotionCodes[kind].code) {
				continue;
			}
			first                = first != nullptr ? first : &potion;
			inputIds[inputCount] = potion.runtimeId;
			inputs[inputCount++] = { potion.handle, LogicalQuantity(potion), D2RL::Items::SocketedItemPolicy::RejectIfNotEmpty };
			total += LogicalQuantity(potion);
		}
		const uint32_t stacks = (total + limit - 1) / limit;
		if (inputCount < 2 || stacks >= inputCount) {
			continue;
		}
		std::memcpy(mergedIds, inputIds, sizeof(inputIds));
		mergedIdCount = inputCount;

		D2RL::Items::ItemCreateSpec outputs[D2RL::Items::MaxTransactionOutputs] {};
		for (uint32_t index = 0; index < stacks; ++index) {
			outputs[index] = {
				.structSize      = D2RL::Items::ItemCreateSpecSize,
				.code            = PotionCodes[kind].code,
				.quality         = D2RL::Items::Quality::Normal,
				.qualityRecordId = D2RL::Items::RandomQualityRecord,
				.itemLevel       = first->itemLevel != 0 ? first->itemLevel : 1,
				.seedMode        = D2RL::Items::SeedMode::Random,
				.quantity        = index + 1 < stacks ? limit : total - limit * (stacks - 1),
				.durability      = D2RL::Items::DefaultValue,
				.stateFlags      = D2RL::Items::ItemStateIdentified,
				.destination     = {
					.structSize = D2RL::Items::ItemDestinationSize,
					.container  = D2RL::Items::ItemContainer::Inventory,
					.placement  = D2RL::Items::Placement::Automatic,
				},
			};
		}
		D2RL::ItemHandle                    created[D2RL::Items::MaxTransactionOutputs] {};
		const D2RL::Items::Transaction      transaction {
			.structSize     = D2RL::Items::TransactionSize,
			.player         = player,
			.inputCount     = inputCount,
			.outputCount    = stacks,
			.inputs         = inputs,
			.outputs        = outputs,
			.outputItems    = created,
			.outputCapacity = D2RL::Items::MaxTransactionOutputs,
		};
		D2RL::Items::TransactionResult result { .structSize = D2RL::Items::TransactionResultSize };
		const auto                     status = items->executeTransaction(plugin, &transaction, &result);
		char                           code[5] {};
		CodeText(PotionCodes[kind].code, code);
		if (status == D2RL::Items::Result::Success) {
			if (config.logActions) {
				Logf(false, "Merged %u %s (%u potions) into %u stacks.", inputCount, code, total, stacks);
			}
			return;
		}

		// Some D2RLoader builds refuse the exchange. Do it in two steps instead:
		// the new stacks need fewer cells than the potions they replace, so once
		// those are gone the stacks always fit.
		uint32_t destroyed = 0;
		for (uint32_t index = 0; index < inputCount; ++index) {
			if (items->destroyItem(plugin, player, inputs[index].item, D2RL::Items::SocketedItemPolicy::RejectIfNotEmpty) == D2RL::Items::Result::Success) {
				destroyed += inputs[index].quantity;
			}
		}
		uint32_t placed = 0;
		for (uint32_t index = 0; index < stacks && placed < destroyed; ++index) {
			const uint32_t quantity = destroyed - placed < limit ? destroyed - placed : limit;
			if (CreateStack(player, *first, quantity, D2RL::Items::ItemContainer::Inventory, D2RL::Items::Placement::Automatic, 0, 0) == D2RL::Items::Result::Success
			    || CreateStack(player, *first, quantity, D2RL::Items::ItemContainer::Cube, D2RL::Items::Placement::Automatic, 0, 0) == D2RL::Items::Result::Success) {
				placed += quantity;
			}
		}
		if (placed != destroyed || destroyed == 0) {
			Logf(true, "Merging %s went wrong: removed %u potions, created stacks holding %u (exchange result %u).", code, destroyed, placed,
			     static_cast<uint32_t>(status));
			failedMergeSignature = inventorySignature;
		} else if (config.logActions) {
			Logf(false, "Merged %u %s (%u potions) into %u stacks in two steps (exchange result %u).", inputCount, code, total, stacks, static_cast<uint32_t>(status));
		}
		return;
	}
}

// ---- Loops ----------------------------------------------------------------

static void __cdecl GameTick(const D2RL::PluginContext*, void*) noexcept {
	gameTickQueued = false;
	const D2RL::PlayerHandle player = localPlayer.load();
	if (player == D2RL::InvalidPlayerHandle) {
		haveSnapshot        = false;
		pendingRestoreCount = 0;
		return;
	}
	if (player != snapshotPlayer) {
		haveSnapshot        = false;
		pendingRestoreCount = 0;
		snapshotPlayer      = player;
	}
	RefreshStackLimits();
	if (!Capture(player, &current)) {
		return;
	}
	if (!beltSeen && current.beltId != 0) {
		beltSeen = true;
		plugin->LogInfo("Found the equipped belt. Belt stacks lost on death are not restored.");
	}

	const ULONGLONG time      = GetTickCount64();
	const uint64_t  signature = InventorySignature(current);
	if (signature != inventorySignature) {
		inventorySignature = signature;
		inventoryChangedAt = time;
	}
	const bool drunk     = haveSnapshot && DetectDrink(player, current, time);
	const bool restoring = RestorePending(player, current, time);
	if (!drunk && !restoring && config.mergeLoose) {
		MergeLoosePotions(player, current, time);
	}
	previous     = current;
	haveSnapshot = true;
}

// The UI loop finds the local player and queues one game-thread tick at a time.
// A session change discards queued game work, so a tick that never ran is
// queued again after a while.
static void __cdecl UiTick(const D2RL::PluginContext* context, void*) noexcept {
	D2RL::PlayerHandle player = D2RL::InvalidPlayerHandle;
	if (inventory->getLocalPlayer(context, &player) != D2RL::Inventory::Result::Success) {
		player = D2RL::InvalidPlayerHandle;
	}
	localPlayer = player;

	const ULONGLONG time = GetTickCount64();
	WatchMouse(time);
	if (config.enabled && player != D2RL::InvalidPlayerHandle && (!gameTickQueued || time - gameTickQueuedAt > GameQueueRetryMs)) {
		gameTickQueued   = threads->runOnGameThread(context, GameTick, nullptr) == D2RL::Threads::Result::Success;
		gameTickQueuedAt = time;
	}
	uiLoopRunning = threads->runOnUiThread(context, UiTick, nullptr) == D2RL::Threads::Result::Success;
}

static void StartLoop() noexcept {
	if (!uiLoopRunning) {
		uiLoopRunning = threads->runOnUiThread(plugin, UiTick, nullptr) == D2RL::Threads::Result::Success;
	}
}

static void __cdecl OnLocalPlayerReady(const D2RL::PluginContext*, const D2RL::Lifecycle::GameplayEvent*, void*) noexcept {
	StartLoop();
}

// ---- Diagnostics ----------------------------------------------------------

static void __cdecl DumpPotions(const D2RL::PluginContext* context, void*) noexcept {
	static Snapshot    snapshot;
	D2RL::PlayerHandle player = D2RL::InvalidPlayerHandle;
	if (inventory->getLocalPlayer(context, &player) != D2RL::Inventory::Result::Success || !Capture(player, &snapshot)) {
		context->WriteConsoleMessage("stackable-potions: no local player.");
		return;
	}
	char line[160] {};
	char code[5] {};
	for (int index = 0; index < snapshot.count; ++index) {
		const StoredPotion& potion = snapshot.potions[index];
		std::snprintf(line, sizeof(line), "%s container=%u x=%d y=%d quantity=%d limit=%u", CodeText(potion.code, code), static_cast<uint32_t>(potion.container), potion.x, potion.y, potion.quantity,
		              StackLimit(potion.code));
		context->WriteConsoleMessage(line);
		Logf(false, "dump: %s", line);
	}
}

static auto DumpCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept -> D2RL::ConsoleCommandResult {
	if (command == nullptr || command->plugin == nullptr) {
		return D2RL::ConsoleCommandResult::Failed;
	}
	return threads->runOnGameThread(command->plugin, DumpPotions, nullptr) == D2RL::Threads::Result::Success ? D2RL::ConsoleCommandResult::Handled : D2RL::ConsoleCommandResult::Failed;
}

// ---- Plugin entry points --------------------------------------------------

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept -> const D2RL::PluginInfo* {
	return &StackablePotionsPluginInfo;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(const D2RL::PluginContext* context) noexcept -> bool {
	if (context == nullptr) {
		return false;
	}
	plugin = context;

	if (context->QueryService(&inventory) != D2RL::ServiceQueryResult::Success || !D2RL::HasInventoryServiceField(inventory, D2RL::InventoryServiceRequiredSize)) {
		context->LogError("The inventory service is unavailable.");
		return false;
	}
	if (context->QueryService(&items) != D2RL::ServiceQueryResult::Success || !D2RL::HasItemServiceField(items, D2RL::ItemServiceRequiredSize)) {
		context->LogError("The item service is unavailable.");
		return false;
	}
	if (context->QueryService(&threads) != D2RL::ServiceQueryResult::Success || !D2RL::HasThreadServiceField(threads, D2RL::ThreadServiceRequiredSize)) {
		context->LogError("The thread service is unavailable.");
		return false;
	}
	if (context->QueryService(&dataTables) != D2RL::ServiceQueryResult::Success || !D2RL::HasDataTableServiceField(dataTables, D2RL::DataTableServiceRequiredSize)) {
		context->LogError("The data table service is unavailable.");
		return false;
	}

	const D2RL::LifecycleService* lifecycle = nullptr;
	if (context->QueryService(&lifecycle) != D2RL::ServiceQueryResult::Success || !D2RL::HasLifecycleServiceField(lifecycle, D2RL::LifecycleServiceRequiredSize)) {
		context->LogError("The lifecycle service is unavailable.");
		return false;
	}
	const D2RL::Lifecycle::DataTablesLoadedListener tablesListener {
		.structSize = D2RL::Lifecycle::DataTablesLoadedListenerSize,
		.callback   = OnDataTablesLoaded,
	};
	D2RL::Lifecycle::ListenerHandle handle = D2RL::Lifecycle::InvalidHandle;
	(void)lifecycle->registerDataTablesLoadedListener(context, &tablesListener, &handle);
	const D2RL::Lifecycle::GameplayEventListener readyListener {
		.structSize = D2RL::Lifecycle::GameplayEventListenerSize,
		.kind       = D2RL::Lifecycle::GameplayEventKind::LocalPlayerReady,
		.callback   = OnLocalPlayerReady,
	};
	(void)lifecycle->registerGameplayEventListener(context, &readyListener, &handle);

	LoadConfig();
	sendPacket = ResolveSendPacket();
	StartLoop();
	(void)context->RegisterConsoleCommand("stackable-potions-dump", DumpCommand, "List potions and their stack sizes as Stackable Potions sees them.");
	Logf(false, "Stackable Potions loaded: enabled=%s merge_loose=%s.", config.enabled ? "true" : "false", config.mergeLoose ? "true" : "false");
	return true;
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
	uiLoopRunning       = false;
	haveSnapshot        = false;
	gameTickQueued      = false;
	pendingRestoreCount = 0;
}
