#pragma once

#include "app_types.h"

namespace campaign_tools {

int AbilityModifier(int score);
const EncounterTableEntry *ResolveRoll(const EncounterRollTable &table, int roll);
bool AdvanceTurn(Encounter &encounter, int delta);
void AdvanceRound(Encounter &encounter);
bool AdjustActiveHitPoints(Encounter &encounter, int delta);
bool ToggleActiveDefeated(Encounter &encounter);
bool AddDungeonMarker(Dungeon &dungeon, DungeonMarker marker);

} // namespace campaign_tools
