#include "campaign_tools.h"

#include <algorithm>
#include <utility>

namespace campaign_tools {

int AbilityModifier(int score) {
    // C++ integer division truncates toward zero, so handle negative odd values explicitly.
    const int difference = score - 10;
    return difference >= 0 ? difference / 2 : -((-difference + 1) / 2);
}

const EncounterTableEntry *ResolveRoll(const EncounterRollTable &table, int roll) {
    if (roll < 1 || roll > table.dieSides) return nullptr;
    for (const EncounterTableEntry &entry : table.entries)
        if (roll >= entry.minimumRoll && roll <= entry.maximumRoll) return &entry;
    return nullptr;
}

bool AdvanceTurn(Encounter &encounter, int delta) {
    if (encounter.participants.empty() || delta == 0) return false;
    const int count = static_cast<int>(encounter.participants.size());
    encounter.activeParticipant = std::clamp(encounter.activeParticipant, 0, count - 1);
    int next = (encounter.activeParticipant + delta) % count;
    if (next < 0) next += count;
    encounter.activeParticipant = next;
    return true;
}

void AdvanceRound(Encounter &encounter) {
    encounter.currentRound = std::max(0, encounter.currentRound) + 1;
    encounter.activeParticipant = 0;
}

bool AdjustActiveHitPoints(Encounter &encounter, int delta) {
    if (encounter.participants.empty()) return false;
    encounter.activeParticipant = std::clamp(
        encounter.activeParticipant, 0, static_cast<int>(encounter.participants.size()) - 1);
    encounter.participants[static_cast<std::size_t>(encounter.activeParticipant)].currentHitPoints += delta;
    return true;
}

bool ToggleActiveDefeated(Encounter &encounter) {
    if (encounter.participants.empty()) return false;
    encounter.activeParticipant = std::clamp(
        encounter.activeParticipant, 0, static_cast<int>(encounter.participants.size()) - 1);
    EncounterParticipant &participant =
        encounter.participants[static_cast<std::size_t>(encounter.activeParticipant)];
    participant.defeated = !participant.defeated;
    return true;
}

bool AddDungeonMarker(Dungeon &dungeon, DungeonMarker marker) {
    if (marker.name.empty()) return false;
    const int kind = static_cast<int>(marker.kind);
    if (kind < static_cast<int>(DungeonMarkerKind::Room) ||
        kind > static_cast<int>(DungeonMarkerKind::Note))
        return false;
    dungeon.markers.push_back(std::move(marker));
    return true;
}

} // namespace campaign_tools
