#include "project_document.h"

#include "grid_geometry.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <utility>

namespace {

template <typename Layer>
void WriteTileLayer(std::ostream &output, const char *name, const Layer &layer) {
    output << name << ' ' << layer.size() << '\n';
    for (const auto &[key, value] : layer) {
        auto [column, row] = grid_geometry::Unpack(key);
        output << column << ' ' << row << ' ' << static_cast<int>(value) << '\n';
    }
}

bool ReadSectionCount(std::istream &input, const char *expectedName, std::size_t &count) {
    std::string actualName;
    input >> actualName >> count;
    return static_cast<bool>(input) && actualName == expectedName;
}

bool IsUnitColor(const Vec3 &color) {
    const auto valid = [](float value) {
        return std::isfinite(value) && value >= 0.0f && value <= 1.0f;
    };
    return valid(color.r) && valid(color.g) && valid(color.b);
}

template <typename Layer>
bool ReadUnsignedTileLayer(std::istream &input, const char *name, Layer &layer, int maximumValue) {
    std::size_t count = 0;
    if (!ReadSectionCount(input, name, count)) return false;
    layer.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        int32_t column = 0;
        int32_t row = 0;
        int value = 0;
        input >> column >> row >> value;
        if (!input || value <= 0 || value > maximumValue) return false;
        layer[grid_geometry::Pack(column, row)] = static_cast<uint8_t>(value);
    }
    return true;
}

template <typename Layer>
bool ReadSignedTileLayer(std::istream &input, const char *name, Layer &layer,
                         int minimumValue, int maximumValue) {
    std::size_t count = 0;
    if (!ReadSectionCount(input, name, count)) return false;
    layer.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        int32_t column = 0;
        int32_t row = 0;
        int value = 0;
        input >> column >> row >> value;
        if (!input || value == 0 || value < minimumValue || value > maximumValue) return false;
        layer[grid_geometry::Pack(column, row)] = static_cast<int8_t>(value);
    }
    return true;
}

} // namespace

bool SaveProjectDocument(const std::string &path, const ProjectDocument &document,
                         std::string &errorMessage) {
    std::ofstream output(path);
    if (!output) {
        errorMessage = "could not open file for writing";
        return false;
    }
    output << std::setprecision(17);
    output << "MAP_DRAWER_PROJECT " << kProjectVersion << '\n';
    output << "DOCUMENT "
           << (document.standaloneDungeon ? "DUNGEON" :
               document.standaloneEncounter ? "ENCOUNTER" :
               document.standaloneCreature ? "CREATURE" : "WORLD") << '\n';
    output << "GRID " << (document.hexGrid ? 1 : 0) << '\n';
    output << "TERRAIN_TYPES " << document.terrainDefinitions.size() << '\n';
    for (const TerrainDefinition &terrain : document.terrainDefinitions) {
        output << terrain.color.r << ' ' << terrain.color.g << ' ' << terrain.color.b << ' '
               << std::quoted(terrain.name) << '\n';
    }
    output << "ELEVATION_SETTINGS " << document.metresPerElevationLevel << ' '
           << document.seaLevel << ' ' << document.contourInterval << ' '
           << (document.elevationView ? 1 : 0) << ' ' << (document.showContours ? 1 : 0) << ' '
           << (document.showHillshade ? 1 : 0) << '\n';
    WriteTileLayer(output, "TERRAIN", document.terrain);
    WriteTileLayer(output, "ELEVATION", document.elevation);
    WriteTileLayer(output, "FOG", document.fog);

    output << "REGIONS " << document.regions.size() << '\n';
    for (const auto &[id, region] : document.regions) {
        (void)id;
        output << region.id << ' ' << region.color.r << ' ' << region.color.g << ' '
               << region.color.b << ' ' << std::quoted(region.name) << ' '
               << std::quoted(region.ruler) << '\n';
    }
    WriteTileLayer(output, "REGION_TILES", document.regionsByTile);

    output << "CITIES " << document.cities.size() << '\n';
    for (const City &city : document.cities) {
        output << city.col << ' ' << city.row << ' ' << std::quoted(city.name) << ' '
               << std::quoted(city.ruler) << '\n';
    }

    output << "POIS " << document.pointsOfInterest.size() << '\n';
    for (const PointOfInterest &poi : document.pointsOfInterest) {
        output << poi.col << ' ' << poi.row << ' ' << static_cast<int>(poi.kind) << ' '
               << std::quoted(poi.name) << ' ' << std::quoted(poi.description) << '\n';
    }

    output << "ENCOUNTERS " << document.encounters.size() << '\n';
    for (const Encounter &encounter : document.encounters) {
        output << encounter.col << ' ' << encounter.row << ' ' << std::quoted(encounter.name) << ' '
               << std::quoted(encounter.description) << '\n';
        output << "ENCOUNTER_CREATURES " << encounter.creatures.size() << '\n';
        for (const EncounterCreature &creature : encounter.creatures)
            output << creature.count << ' ' << std::quoted(creature.name) << ' '
                   << std::quoted(creature.sourceFile) << '\n';
        output << "ENCOUNTER_EFFECTS " << encounter.effects.size() << '\n';
        for (const EncounterEffect &effect : encounter.effects)
            output << std::quoted(effect.description) << '\n';
        output << "ENCOUNTER_TABLES " << encounter.rollTables.size() << '\n';
        for (const EncounterRollTable &table : encounter.rollTables) {
            output << table.dieSides << ' ' << table.entries.size() << ' '
                   << std::quoted(table.name) << '\n';
            for (const EncounterTableEntry &entry : table.entries)
                output << entry.minimumRoll << ' ' << entry.maximumRoll << ' '
                       << std::quoted(entry.result) << '\n';
        }
        output << "ENCOUNTER_DETAILS " << std::quoted(encounter.sourceFile) << ' '
               << std::quoted(encounter.trigger) << ' ' << std::quoted(encounter.objective) << ' '
               << std::quoted(encounter.environment) << ' '
               << std::quoted(encounter.gameMasterNotes) << ' ' << std::quoted(encounter.rewards) << ' '
               << std::quoted(encounter.successOutcome) << ' '
               << std::quoted(encounter.failureOutcome) << '\n';
        output << "ENCOUNTER_RUN " << encounter.currentRound << ' '
               << encounter.activeParticipant << ' ' << encounter.participants.size() << '\n';
        for (const EncounterParticipant &participant : encounter.participants)
            output << participant.initiative << ' ' << participant.currentHitPoints << ' '
                   << (participant.defeated ? 1 : 0) << ' ' << std::quoted(participant.name) << ' '
                   << std::quoted(participant.conditions) << '\n';
    }

    output << "CREATURES " << document.creatures.size() << '\n';
    for (const CreatureStatBlock &creature : document.creatures) {
        output << std::quoted(creature.name) << ' ' << std::quoted(creature.classification) << ' '
               << std::quoted(creature.armorClass) << ' ' << std::quoted(creature.hitPoints) << ' '
               << std::quoted(creature.speed);
        for (int score : creature.abilityScores) output << ' ' << score;
        output << ' '
               << std::quoted(creature.savesAndSkills) << ' '
               << std::quoted(creature.sensesAndLanguages) << ' '
               << std::quoted(creature.challenge) << ' ' << std::quoted(creature.traits) << ' '
               << std::quoted(creature.actions) << ' ' << std::quoted(creature.reactions) << ' '
               << std::quoted(creature.legendaryActions) << '\n';
        output << "CREATURE_ABILITIES " << creature.specialAbilities.size() << '\n';
        for (const CreatureStatBlock::Ability &ability : creature.specialAbilities)
            output << std::quoted(ability.name) << ' ' << std::quoted(ability.description) << '\n';
        output << "CREATURE_DETAILS " << std::quoted(creature.damageVulnerabilities) << ' '
               << std::quoted(creature.damageResistances) << ' '
               << std::quoted(creature.damageImmunities) << ' '
               << std::quoted(creature.conditionImmunities) << ' '
               << std::quoted(creature.proficiencyBonus) << ' '
               << std::quoted(creature.passivePerception) << ' '
               << std::quoted(creature.spellcasting) << ' '
               << std::quoted(creature.portraitFile) << '\n';
    }

    output << "DUNGEONS " << document.dungeons.size() << '\n';
    for (const Dungeon &dungeon : document.dungeons) {
        output << dungeon.worldCol << ' ' << dungeon.worldRow << ' '
               << (dungeon.hasEntrance ? 1 : 0) << ' ' << dungeon.entranceCol << ' '
               << dungeon.entranceRow << ' ' << (dungeon.hasExit ? 1 : 0) << ' '
               << dungeon.exitCol << ' ' << dungeon.exitRow << ' ' << dungeon.tiles.size() << ' '
               << std::quoted(dungeon.name) << ' ' << std::quoted(dungeon.description) << ' '
               << std::quoted(dungeon.sourceFile) << '\n';
        for (const auto &[key, value] : dungeon.tiles) {
            auto [column, row] = grid_geometry::Unpack(key);
            output << column << ' ' << row << ' ' << static_cast<int>(value) << '\n';
        }
        output << "DUNGEON_TERRAINS " << dungeon.terrainDefinitions.size() << '\n';
        for (const TerrainDefinition &terrain : dungeon.terrainDefinitions) {
            output << terrain.color.r << ' ' << terrain.color.g << ' ' << terrain.color.b << ' '
                   << std::quoted(terrain.name) << '\n';
        }
        WriteTileLayer(output, "DUNGEON_ELEVATION", dungeon.elevation);
        WriteTileLayer(output, "DUNGEON_FOG", dungeon.fog);
        output << "DUNGEON_MARKERS " << dungeon.markers.size() << '\n';
        for (const DungeonMarker &marker : dungeon.markers)
            output << marker.col << ' ' << marker.row << ' ' << static_cast<int>(marker.kind) << ' '
                   << (marker.gameMasterOnly ? 1 : 0) << ' ' << std::quoted(marker.name) << ' '
                   << std::quoted(marker.description) << ' ' << std::quoted(marker.sourceFile) << '\n';
    }

    output << "ROUTES " << document.routes.size() << '\n';
    for (const Route &route : document.routes) {
        output << static_cast<int>(route.kind) << ' ' << route.points.size() << ' '
               << std::quoted(route.name) << '\n';
        for (const auto &[x, y] : route.points) output << x << ' ' << y << '\n';
    }
    output << "END\n";

    if (!output) {
        errorMessage = "write failed before the document was complete";
        return false;
    }
    return true;
}

bool LoadProjectDocument(const std::string &path, ProjectDocument &document,
                         int &loadedVersion, std::string &errorMessage) {
    std::ifstream input(path);
    if (!input) {
        errorMessage = "could not open file for reading";
        return false;
    }

    ProjectDocument loaded;
    std::string tag;
    input >> tag >> loadedVersion;
    if (!input || tag != "MAP_DRAWER_PROJECT" || loadedVersion < 1 ||
        loadedVersion > kProjectVersion) {
        errorMessage = "unsupported or invalid project header";
        return false;
    }

    if (loadedVersion >= 9) {
        std::string documentKind;
        input >> tag >> documentKind;
        if (!input || tag != "DOCUMENT" ||
            (documentKind != "WORLD" && documentKind != "DUNGEON" &&
             (loadedVersion < 11 || documentKind != "ENCOUNTER") &&
             (loadedVersion < 12 || documentKind != "CREATURE"))) {
            errorMessage = "invalid DOCUMENT section";
            return false;
        }
        loaded.standaloneDungeon = documentKind == "DUNGEON";
        loaded.standaloneEncounter = documentKind == "ENCOUNTER";
        loaded.standaloneCreature = documentKind == "CREATURE";
    }

    int gridValue = 0;
    input >> tag >> gridValue;
    if (!input || tag != "GRID" || (gridValue != 0 && gridValue != 1)) {
        errorMessage = "invalid GRID section";
        return false;
    }
    loaded.hexGrid = gridValue != 0;

    if (loadedVersion >= 5) {
        std::size_t terrainTypeCount = 0;
        if (!ReadSectionCount(input, "TERRAIN_TYPES", terrainTypeCount) || terrainTypeCount == 0 ||
            terrainTypeCount > kMaxTerrainTypes) {
            errorMessage = "invalid TERRAIN_TYPES section";
            return false;
        }
        loaded.terrainDefinitions.clear();
        loaded.terrainDefinitions.reserve(terrainTypeCount);
        for (std::size_t index = 0; index < terrainTypeCount; ++index) {
            TerrainDefinition terrain;
            input >> terrain.color.r >> terrain.color.g >> terrain.color.b >> std::quoted(terrain.name);
            const auto validColor = [](float value) {
                return std::isfinite(value) && value >= 0.0f && value <= 1.0f;
            };
            if (!input || terrain.name.empty() || !validColor(terrain.color.r) ||
                !validColor(terrain.color.g) || !validColor(terrain.color.b)) {
                errorMessage = "invalid terrain definition";
                return false;
            }
            loaded.terrainDefinitions.push_back(std::move(terrain));
        }
    }

    if (loadedVersion >= 3) {
        int elevationView = 0;
        int contours = 0;
        int hillshade = 0;
        input >> tag >> loaded.metresPerElevationLevel >> loaded.seaLevel >> loaded.contourInterval
              >> elevationView >> contours >> hillshade;
        if (!input || tag != "ELEVATION_SETTINGS" || loaded.metresPerElevationLevel <= 0 ||
            loaded.metresPerElevationLevel > 100000 || loaded.seaLevel < kMinElevation ||
            loaded.seaLevel > kMaxElevation || loaded.contourInterval <= 0 ||
            loaded.contourInterval > kMaxElevation - kMinElevation ||
            (elevationView != 0 && elevationView != 1) || (contours != 0 && contours != 1) ||
            (hillshade != 0 && hillshade != 1)) {
            errorMessage = "invalid ELEVATION_SETTINGS section";
            return false;
        }
        loaded.elevationView = elevationView != 0;
        loaded.showContours = contours != 0;
        loaded.showHillshade = hillshade != 0;
    }

    if (!ReadUnsignedTileLayer(input, "TERRAIN", loaded.terrain,
                               static_cast<int>(loaded.terrainDefinitions.size()) - 1)) {
        errorMessage = "invalid TERRAIN section";
        return false;
    }

    std::size_t elevationCount = 0;
    if (!ReadSectionCount(input, "ELEVATION", elevationCount)) {
        errorMessage = "invalid ELEVATION section";
        return false;
    }
    loaded.elevation.reserve(elevationCount);
    for (std::size_t index = 0; index < elevationCount; ++index) {
        int32_t column = 0;
        int32_t row = 0;
        int value = 0;
        input >> column >> row >> value;
        if (!input || value < kMinElevation || value > kMaxElevation || value == 0) {
            errorMessage = "invalid elevation tile";
            return false;
        }
        loaded.elevation[grid_geometry::Pack(column, row)] = static_cast<int8_t>(value);
    }

    if (loadedVersion >= 2 && !ReadUnsignedTileLayer(input, "FOG", loaded.fog, 1)) {
        errorMessage = "invalid FOG section";
        return false;
    }

    std::size_t regionCount = 0;
    if (!ReadSectionCount(input, "REGIONS", regionCount)) {
        errorMessage = "invalid REGIONS section";
        return false;
    }
    for (std::size_t index = 0; index < regionCount; ++index) {
        Region region;
        input >> region.id >> region.color.r >> region.color.g >> region.color.b
              >> std::quoted(region.name) >> std::quoted(region.ruler);
        if (!input || region.id <= 0 || region.id > kMaxRegions) {
            errorMessage = "invalid region metadata";
            return false;
        }
        loaded.regions[region.id] = std::move(region);
    }
    if (!ReadUnsignedTileLayer(input, "REGION_TILES", loaded.regionsByTile, kMaxRegions)) {
        errorMessage = "invalid REGION_TILES section";
        return false;
    }

    std::size_t cityCount = 0;
    if (!ReadSectionCount(input, "CITIES", cityCount)) {
        errorMessage = "invalid CITIES section";
        return false;
    }
    loaded.cities.reserve(cityCount);
    for (std::size_t index = 0; index < cityCount; ++index) {
        City city;
        input >> city.col >> city.row >> std::quoted(city.name) >> std::quoted(city.ruler);
        if (!input) {
            errorMessage = "invalid city data";
            return false;
        }
        loaded.cities.push_back(std::move(city));
    }

    std::size_t poiCount = 0;
    if (!ReadSectionCount(input, "POIS", poiCount)) {
        errorMessage = "invalid POIS section";
        return false;
    }
    loaded.pointsOfInterest.reserve(poiCount);
    for (std::size_t index = 0; index < poiCount; ++index) {
        PointOfInterest poi;
        int kind = 0;
        input >> poi.col >> poi.row >> kind >> std::quoted(poi.name) >> std::quoted(poi.description);
        if (!input || kind < 0 || kind >= kPoiKindCount) {
            errorMessage = "invalid point-of-interest data";
            return false;
        }
        poi.kind = static_cast<PoiKind>(kind);
        loaded.pointsOfInterest.push_back(std::move(poi));
    }

    if (loadedVersion >= 4) {
        std::size_t encounterCount = 0;
        if (!ReadSectionCount(input, "ENCOUNTERS", encounterCount)) {
            errorMessage = "invalid ENCOUNTERS section";
            return false;
        }
        loaded.encounters.reserve(encounterCount);
        for (std::size_t index = 0; index < encounterCount; ++index) {
            Encounter encounter;
            input >> encounter.col >> encounter.row >> std::quoted(encounter.name)
                  >> std::quoted(encounter.description);
            if (!input) {
                errorMessage = "invalid encounter data";
                return false;
            }
            if (loadedVersion >= 10) {
                std::size_t creatureCount = 0;
                if (!ReadSectionCount(input, "ENCOUNTER_CREATURES", creatureCount) ||
                    creatureCount > 1000) {
                    errorMessage = "invalid ENCOUNTER_CREATURES section";
                    return false;
                }
                encounter.creatures.reserve(creatureCount);
                for (std::size_t creatureIndex = 0; creatureIndex < creatureCount; ++creatureIndex) {
                    EncounterCreature creature;
                    input >> creature.count >> std::quoted(creature.name);
                    if (loadedVersion >= 12) input >> std::quoted(creature.sourceFile);
                    if (!input || creature.count <= 0 || creature.count > 10000 ||
                        creature.name.empty()) {
                        errorMessage = "invalid encounter creature";
                        return false;
                    }
                    encounter.creatures.push_back(std::move(creature));
                }
                std::size_t effectCount = 0;
                if (!ReadSectionCount(input, "ENCOUNTER_EFFECTS", effectCount) ||
                    effectCount > 1000) {
                    errorMessage = "invalid ENCOUNTER_EFFECTS section";
                    return false;
                }
                encounter.effects.reserve(effectCount);
                for (std::size_t effectIndex = 0; effectIndex < effectCount; ++effectIndex) {
                    EncounterEffect effect;
                    input >> std::quoted(effect.description);
                    if (!input || effect.description.empty()) {
                        errorMessage = "invalid encounter effect";
                        return false;
                    }
                    encounter.effects.push_back(std::move(effect));
                }
                std::size_t tableCount = 0;
                if (!ReadSectionCount(input, "ENCOUNTER_TABLES", tableCount) || tableCount > 100) {
                    errorMessage = "invalid ENCOUNTER_TABLES section";
                    return false;
                }
                encounter.rollTables.reserve(tableCount);
                for (std::size_t tableIndex = 0; tableIndex < tableCount; ++tableIndex) {
                    EncounterRollTable table;
                    std::size_t entryCount = 0;
                    input >> table.dieSides >> entryCount >> std::quoted(table.name);
                    if (!input || table.dieSides <= 0 || table.dieSides > 1000 ||
                        entryCount == 0 || entryCount > 1000 || table.name.empty()) {
                        errorMessage = "invalid encounter roll table";
                        return false;
                    }
                    table.entries.reserve(entryCount);
                    for (std::size_t entryIndex = 0; entryIndex < entryCount; ++entryIndex) {
                        EncounterTableEntry entry;
                        input >> entry.minimumRoll >> entry.maximumRoll >> std::quoted(entry.result);
                        if (!input || entry.minimumRoll <= 0 ||
                            entry.minimumRoll > entry.maximumRoll ||
                            entry.maximumRoll > table.dieSides || entry.result.empty()) {
                            errorMessage = "invalid encounter table row";
                            return false;
                        }
                        table.entries.push_back(std::move(entry));
                    }
                    std::sort(table.entries.begin(), table.entries.end(),
                              [](const EncounterTableEntry &left,
                                 const EncounterTableEntry &right) {
                                  return left.minimumRoll < right.minimumRoll;
                              });
                    for (std::size_t entryIndex = 1; entryIndex < table.entries.size();
                         ++entryIndex) {
                        if (table.entries[entryIndex].minimumRoll <=
                            table.entries[entryIndex - 1].maximumRoll) {
                            errorMessage = "encounter table roll ranges overlap";
                            return false;
                        }
                    }
                    encounter.rollTables.push_back(std::move(table));
                }
            }
            if (loadedVersion >= 15) {
                input >> tag >> std::quoted(encounter.sourceFile) >> std::quoted(encounter.trigger)
                      >> std::quoted(encounter.objective) >> std::quoted(encounter.environment)
                      >> std::quoted(encounter.gameMasterNotes) >> std::quoted(encounter.rewards)
                      >> std::quoted(encounter.successOutcome) >> std::quoted(encounter.failureOutcome);
                if (!input || tag != "ENCOUNTER_DETAILS") {
                    errorMessage = "invalid ENCOUNTER_DETAILS section";
                    return false;
                }
                std::size_t participantCount = 0;
                input >> tag >> encounter.currentRound >> encounter.activeParticipant >> participantCount;
                if (!input || tag != "ENCOUNTER_RUN" || encounter.currentRound < 0 ||
                    participantCount > 10000 || encounter.activeParticipant < 0) {
                    errorMessage = "invalid ENCOUNTER_RUN section";
                    return false;
                }
                encounter.participants.reserve(participantCount);
                for (std::size_t participantIndex = 0; participantIndex < participantCount;
                     ++participantIndex) {
                    EncounterParticipant participant;
                    int defeated = 0;
                    input >> participant.initiative >> participant.currentHitPoints >> defeated
                          >> std::quoted(participant.name) >> std::quoted(participant.conditions);
                    if (!input || participant.name.empty() ||
                        (defeated != 0 && defeated != 1)) {
                        errorMessage = "invalid encounter participant";
                        return false;
                    }
                    participant.defeated = defeated != 0;
                    encounter.participants.push_back(std::move(participant));
                }
                if (!encounter.participants.empty() &&
                    encounter.activeParticipant >= static_cast<int>(encounter.participants.size())) {
                    errorMessage = "encounter active participant is out of range";
                    return false;
                }
            }
            loaded.encounters.push_back(std::move(encounter));
        }
    }

    if (loadedVersion >= 12) {
        std::size_t creatureCount = 0;
        if (!ReadSectionCount(input, "CREATURES", creatureCount) || creatureCount > 10000) {
            errorMessage = "invalid CREATURES section";
            return false;
        }
        loaded.creatures.reserve(creatureCount);
        for (std::size_t index = 0; index < creatureCount; ++index) {
            CreatureStatBlock creature;
            input >> std::quoted(creature.name) >> std::quoted(creature.classification)
                  >> std::quoted(creature.armorClass) >> std::quoted(creature.hitPoints)
                  >> std::quoted(creature.speed);
            if (loadedVersion >= 14) {
                for (int &score : creature.abilityScores) input >> score;
            } else {
                std::string legacyScores;
                input >> std::quoted(legacyScores);
                std::istringstream scores(legacyScores);
                std::string label;
                for (int &score : creature.abilityScores) {
                    int parsed = 10;
                    if (!(scores >> label >> parsed)) break;
                    score = parsed;
                }
            }
            input >> std::quoted(creature.savesAndSkills)
                  >> std::quoted(creature.sensesAndLanguages) >> std::quoted(creature.challenge)
                  >> std::quoted(creature.traits) >> std::quoted(creature.actions)
                  >> std::quoted(creature.reactions) >> std::quoted(creature.legendaryActions);
            if (!input || creature.name.empty()) {
                errorMessage = "invalid creature stat block";
                return false;
            }
            for (int score : creature.abilityScores) {
                if (score < 0 || score > 99) {
                    errorMessage = "invalid creature ability score";
                    return false;
                }
            }
            if (loadedVersion >= 13) {
                std::size_t abilityCount = 0;
                if (!ReadSectionCount(input, "CREATURE_ABILITIES", abilityCount) ||
                    abilityCount > 1000) {
                    errorMessage = "invalid CREATURE_ABILITIES section";
                    return false;
                }
                creature.specialAbilities.reserve(abilityCount);
                for (std::size_t abilityIndex = 0; abilityIndex < abilityCount; ++abilityIndex) {
                    CreatureStatBlock::Ability ability;
                    input >> std::quoted(ability.name) >> std::quoted(ability.description);
                    if (!input || ability.name.empty()) {
                        errorMessage = "invalid creature special ability";
                        return false;
                    }
                    creature.specialAbilities.push_back(std::move(ability));
                }
            }
            if (loadedVersion >= 15) {
                input >> tag >> std::quoted(creature.damageVulnerabilities)
                      >> std::quoted(creature.damageResistances)
                      >> std::quoted(creature.damageImmunities)
                      >> std::quoted(creature.conditionImmunities)
                      >> std::quoted(creature.proficiencyBonus)
                      >> std::quoted(creature.passivePerception)
                      >> std::quoted(creature.spellcasting) >> std::quoted(creature.portraitFile);
                if (!input || tag != "CREATURE_DETAILS") {
                    errorMessage = "invalid CREATURE_DETAILS section";
                    return false;
                }
            }
            loaded.creatures.push_back(std::move(creature));
        }
    }

    if (loadedVersion >= 6) {
        std::size_t dungeonCount = 0;
        if (!ReadSectionCount(input, "DUNGEONS", dungeonCount)) {
            errorMessage = "invalid DUNGEONS section";
            return false;
        }
        loaded.dungeons.reserve(dungeonCount);
        for (std::size_t index = 0; index < dungeonCount; ++index) {
            Dungeon dungeon;
            int hasEntrance = 0;
            int hasExit = 0;
            std::size_t tileCount = 0;
            input >> dungeon.worldCol >> dungeon.worldRow >> hasEntrance >> dungeon.entranceCol
                  >> dungeon.entranceRow >> hasExit >> dungeon.exitCol >> dungeon.exitRow
                  >> tileCount >> std::quoted(dungeon.name) >> std::quoted(dungeon.description);
            if (loadedVersion >= 9) input >> std::quoted(dungeon.sourceFile);
            if (!input || dungeon.name.empty() || (hasEntrance != 0 && hasEntrance != 1) ||
                (hasExit != 0 && hasExit != 1)) {
                errorMessage = "invalid dungeon metadata";
                return false;
            }
            dungeon.hasEntrance = hasEntrance != 0;
            dungeon.hasExit = hasExit != 0;
            dungeon.tiles.reserve(tileCount);
            for (std::size_t tileIndex = 0; tileIndex < tileCount; ++tileIndex) {
                int32_t column = 0;
                int32_t row = 0;
                int value = 0;
                input >> column >> row >> value;
                if (!input || value <= 0 || value >= kMaxTerrainTypes) {
                    errorMessage = "invalid dungeon tile";
                    return false;
                }
                dungeon.tiles[grid_geometry::Pack(column, row)] = static_cast<uint8_t>(value);
            }
            if (loadedVersion >= 8) {
                std::size_t terrainCount = 0;
                if (!ReadSectionCount(input, "DUNGEON_TERRAINS", terrainCount) ||
                    terrainCount < 2 || terrainCount > static_cast<std::size_t>(kMaxTerrainTypes)) {
                    errorMessage = "invalid dungeon terrain definitions";
                    return false;
                }
                dungeon.terrainDefinitions.clear();
                dungeon.terrainDefinitions.reserve(terrainCount);
                for (std::size_t terrainIndex = 0; terrainIndex < terrainCount; ++terrainIndex) {
                    TerrainDefinition terrain;
                    input >> terrain.color.r >> terrain.color.g >> terrain.color.b >> std::quoted(terrain.name);
                    if (!input || terrain.name.empty() || !IsUnitColor(terrain.color)) {
                        errorMessage = "invalid dungeon terrain definition";
                        return false;
                    }
                    dungeon.terrainDefinitions.push_back(std::move(terrain));
                }
                if (!ReadSignedTileLayer(input, "DUNGEON_ELEVATION", dungeon.elevation,
                                         kMinElevation, kMaxElevation)) {
                    errorMessage = "invalid dungeon elevation layer";
                    return false;
                }
                if (!ReadUnsignedTileLayer(input, "DUNGEON_FOG", dungeon.fog, 1)) {
                    errorMessage = "invalid dungeon fog layer";
                    return false;
                }
                for (const auto &[key, terrain] : dungeon.tiles) {
                    (void)key;
                    if (terrain >= dungeon.terrainDefinitions.size()) {
                        errorMessage = "dungeon tile references an unknown terrain";
                        return false;
                    }
                }
            }
            if (loadedVersion >= 15) {
                std::size_t markerCount = 0;
                if (!ReadSectionCount(input, "DUNGEON_MARKERS", markerCount) ||
                    markerCount > 10000) {
                    errorMessage = "invalid DUNGEON_MARKERS section";
                    return false;
                }
                dungeon.markers.reserve(markerCount);
                for (std::size_t markerIndex = 0; markerIndex < markerCount; ++markerIndex) {
                    DungeonMarker marker;
                    int kind = 0;
                    int gameMasterOnly = 0;
                    input >> marker.col >> marker.row >> kind >> gameMasterOnly
                          >> std::quoted(marker.name) >> std::quoted(marker.description)
                          >> std::quoted(marker.sourceFile);
                    if (!input || kind < 0 || kind > static_cast<int>(DungeonMarkerKind::Note) ||
                        (gameMasterOnly != 0 && gameMasterOnly != 1) || marker.name.empty()) {
                        errorMessage = "invalid dungeon marker";
                        return false;
                    }
                    marker.kind = static_cast<DungeonMarkerKind>(kind);
                    marker.gameMasterOnly = gameMasterOnly != 0;
                    dungeon.markers.push_back(std::move(marker));
                }
            }
            loaded.dungeons.push_back(std::move(dungeon));
        }
    }

    std::size_t routeCount = 0;
    if (!ReadSectionCount(input, "ROUTES", routeCount)) {
        errorMessage = "invalid ROUTES section";
        return false;
    }
    loaded.routes.reserve(routeCount);
    for (std::size_t index = 0; index < routeCount; ++index) {
        Route route;
        int kind = 0;
        std::size_t pointCount = 0;
        input >> kind >> pointCount >> std::quoted(route.name);
        if (!input || (kind != 0 && kind != 1)) {
            errorMessage = "invalid route data";
            return false;
        }
        route.kind = kind == 0 ? RouteKind::River : RouteKind::TradeRoute;
        route.points.reserve(pointCount);
        for (std::size_t pointIndex = 0; pointIndex < pointCount; ++pointIndex) {
            double x = 0.0;
            double y = 0.0;
            input >> x >> y;
            if (!input) {
                errorMessage = "invalid route point";
                return false;
            }
            route.points.emplace_back(x, y);
        }
        loaded.routes.push_back(std::move(route));
    }

    input >> tag;
    if (!input || tag != "END") {
        errorMessage = "missing END marker";
        return false;
    }

    if (loaded.standaloneDungeon && loaded.dungeons.size() != 1) {
        errorMessage = "a standalone dungeon file must contain exactly one dungeon map";
        return false;
    }
    if (loaded.standaloneEncounter && loaded.encounters.size() != 1) {
        errorMessage = "a standalone encounter file must contain exactly one encounter";
        return false;
    }
    if (loaded.standaloneCreature && loaded.creatures.size() != 1) {
        errorMessage = "a standalone creature file must contain exactly one stat block";
        return false;
    }

    // Dungeon POIs are the overworld identity and access point for dungeon maps. Projects
    // created before this relationship was enforced may only contain the dungeon record, so
    // synthesize the matching marker during load rather than leaving that map unreachable.
    for (const Dungeon &dungeon : loaded.dungeons) {
        if (loaded.standaloneDungeon) break;
        const auto poi = std::find_if(
            loaded.pointsOfInterest.begin(), loaded.pointsOfInterest.end(),
            [&](const PointOfInterest &candidate) {
                return candidate.kind == PoiKind::Dungeon &&
                       candidate.col == dungeon.worldCol && candidate.row == dungeon.worldRow;
            });
        if (poi == loaded.pointsOfInterest.end()) {
            loaded.pointsOfInterest.push_back(
                {dungeon.worldCol, dungeon.worldRow, PoiKind::Dungeon,
                 dungeon.name, dungeon.description});
        }
    }

    document = std::move(loaded);
    return true;
}
