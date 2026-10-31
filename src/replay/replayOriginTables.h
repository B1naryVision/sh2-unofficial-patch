#pragma once

#include <cstdint>

// Command provenance by posting routine, for replayTrace.cpp. Every entry is
// keyed to the Steam v1.5.0 executable (RVAs). How each one was found, and why
// some routines are deliberately absent, is in docs/features/multiplayer-replays.md
// ("Command origin").
//
// An admission is classified by its caller and one frame up. The AI and system
// tests run first, so an autonomous class posted through an input body is still
// classified by its RTTI name.

// [begin, end) of a whole function body.
struct ReplayRoutineRange {
    uint32_t begin;
    uint32_t end;
};

// Pane process entries with a proven human-control interpretation. A caller
// within the first 0x600 bytes of one counts as UI.
static const uint32_t REPLAY_UI_PROCESS_RVAS[] = {
    0x203990, // Base
    0x20a210, // Armoury
    0x20b150, // Blacksmith workshop
    0x20e450, // Carter post
    0x210ea0, // Granary
    0x214820, // Inn
    0x216340, // Kitchen
    0x21da50, // Market
    0x21e400, // Treasury
    0x2206d0, // Barracks
    0x223be0, // Engineers guild
    0x225490, // Mercenary post
    0x2284d0, // Monastery
    0x228c50, // Siege camp
    0x22c090, // Church
    0x22d5d0, // Constructing building
    0x22e7d0, // Troops
    0x2329c0, // Worker
    0x207150, // Keep (return Lord to keep)
    0x31e700, // Actor event / selected actor controls
};
static const uint32_t REPLAY_UI_PROCESS_SPAN = 0x600;

// Autonomous routines whose Chores are not named AI*.
static const ReplayRoutineRange REPLAY_AI_POST_RANGES[] = {
    {0x19e1a0, 0x19e390}, // FUN_0059e1a0: clan move + stance pass
};

// Input code: the mouse dispatcher FUN_005f6710, the GetAsyncKeyState hotkey
// dispatcher FUN_006af0d0, and pane bodies and posters reached only from them.
static const ReplayRoutineRange REPLAY_UI_POST_RANGES[] = {
    {0x1f3820, 0x1f45f2}, // FUN_005f3820, a mouse-dispatcher branch
    {0x20ee90, 0x20f0d2}, // carter pane, CarterTradeChore
    {0x20e450, 0x20eb5c}, // Carter post pane (past the 0x600 window), StopCarterTradeChore
    {0x208b90, 0x208e02}, // estate pane, BuyVillageChore
    {0x267ca0, 0x267ea9}, // promotion/rank screen, ChangeRankChore
    {0x0f2df0, 0x0f3fd8}, // S2ActorHandler troop-command posters (helper 0x0f30b0)
    {0x1ea280, 0x1ea517}, // place-building dispatcher, the parent frame
    {0x1eb080, 0x1eb119}, // delete-building dispatcher, the parent frame
    {0x231320, 0x232446}, // troop-move dispatcher, the parent frame
    {0x1f6710, 0x1f7590}, // FUN_005f6710, the mouse/command dispatcher
    {0x1f75b0, 0x1f77b3}, // FUN_005f75b0, the frame above it
    {0x2af0d0, 0x2af608}, // FUN_006af0d0, the GetAsyncKeyState hotkey path
    {0x2aff30, 0x2aff3d}, // FUN_006afef0, the frame above it
    {0x1eb610, 0x1eb8c3}, // AssemblyPointChore poster
    {0x1ebe50, 0x1ec250}, // PlaceTowerEquipmentChore poster
    {0x1eb4a0, 0x1eb54e}, // DeleteWallsChore UI parent frame
    {0x1ec310, 0x1ec345}, // its one non-dispatcher caller, itself UI-only
    {0x1ea710, 0x1ea74e}, // PlaceWallsChore UI parent frame
    {0x1ec350, 0x1ec3a2}, // GameGroundFeatureChore UI parent frame
    {0x1ec3b0, 0x1ec3ea}, // GameGroundFeatureChore UI parent frame
    {0x20fae0, 0x20fcb2}, // workshop pane, MakeWeaponChore
    {0x218860, 0x218a32}, // second workshop pane, MakeWeaponChore
    {0x206650, 0x206835}, // gatehouse pane, OpenGateHouseChore
    {0x2052b0, 0x2054fe}, // tower pane, burning logs and stone tipper
    {0x208250, 0x2083d8}, // repair pane, RepairBuildingChore
    {0x22ad30, 0x22aeb4}, // tunnel pane, TunnelAddEngineerChore
    {0x0ef700, 0x0ef7ed}, // FUN_004ef700, recruit post helper (shiftRecruit retargets its callers)
    {0x24d8e0, 0x24da0f}, // FUN_0064d8e0, ChatChore poster
};

// The autonomous roots: the economy pass FUN_0045b9b0, the policy pass
// FUN_0045d9b0, the per-player pass FUN_00467190, and what they reach.
static const ReplayRoutineRange REPLAY_SYSTEM_POST_RANGES[] = {
    {0x05be80, 0x05c1ff}, // the stock/gold economy pass (TradeChore)
    {0x05e050, 0x05ea4c}, // the player-policy update pass
    {0x067190, 0x067246}, // FUN_00467190, the per-player pass root
    {0x0670d0, 0x067183}, // FUN_004670d0
    {0x066e40, 0x0670cb}, // FUN_00466e40
    {0x05f160, 0x05feaa}, // FUN_0045f160, posts CreateTroopChore
    {0x065470, 0x0660d5}, // FUN_00465470, posts CreateTroopChore
    {0x0645b0, 0x064c22}, // FUN_004645b0, posts policy and CarterTradeChore
    {0x0619d0, 0x061c17}, // FUN_004619d0
    {0x05de20, 0x05e049}, // FUN_0045de20, posts BuyVillageChore
    {0x061f80, 0x062ab9}, // FUN_00461f80, posts HarassClanInstructionsChore
    {0x0564c0, 0x056f12}, // FUN_004564c0, posts walls, ground and buildings
    {0x37e9e0, 0x37ea5b}, // FUN_0077e9e0, the shared poster trampoline
    {0x057de0, 0x057ff4}, // FUN_00457de0, the construction hub
    {0x057500, 0x057639}, // FUN_00457500
    {0x0578c0, 0x057bf9}, // FUN_004578c0, posts PlaceBuildingChore
    {0x057780, 0x0578b9}, // FUN_00457780, posts PlaceBuildingChore
    {0x057640, 0x057779}, // FUN_00457640
    {0x055660, 0x0556e7}, // FUN_00455660
    {0x0556f0, 0x055777}, // FUN_004556f0
    {0x055780, 0x055864}, // FUN_00455780
    {0x056240, 0x05630a}, // FUN_00456240
    {0x056310, 0x05645c}, // FUN_00456310
    {0x054dd0, 0x054e7e}, // FUN_00454dd0
    {0x0584e0, 0x05894a}, // FUN_004584e0
    {0x01e4f0, 0x01e7a8}, // FUN_0041e4f0, also posts DeleteWallsChore
};
