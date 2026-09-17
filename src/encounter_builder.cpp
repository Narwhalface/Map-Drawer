#include "encounter_builder.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace {

std::string Trim(std::string text) {
    const auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
    text.erase(text.begin(), std::find_if_not(text.begin(), text.end(), isSpace));
    text.erase(std::find_if_not(text.rbegin(), text.rend(), isSpace).base(), text.end());
    return text;
}

std::vector<std::string> Split(const std::string &text, const std::string &separator) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find(separator, start);
        parts.push_back(Trim(text.substr(start, end == std::string::npos ? end : end - start)));
        if (end == std::string::npos) break;
        start = end + separator.size();
    }
    return parts;
}

bool ParsePositiveInt(const std::string &text, int &value) {
    if (text.empty() || !std::all_of(text.begin(), text.end(), [](unsigned char c) {
            return std::isdigit(c) != 0;
        }))
        return false;
    try {
        value = std::stoi(text);
    } catch (...) {
        return false;
    }
    return value > 0 && value <= 10000;
}

} // namespace

std::string FormatEncounterCreatures(const Encounter &encounter) {
    std::string text;
    for (const EncounterCreature &creature : encounter.creatures) {
        if (!text.empty()) text += "; ";
        text += std::to_string(creature.count) + " " + creature.name;
    }
    return text;
}

std::string FormatEncounterEffects(const Encounter &encounter) {
    std::string text;
    for (const EncounterEffect &effect : encounter.effects) {
        if (!text.empty()) text += " > ";
        text += effect.description;
    }
    return text;
}

std::string FormatEncounterTables(const Encounter &encounter) {
    std::string text;
    for (const EncounterRollTable &table : encounter.rollTables) {
        if (!text.empty()) text += " || ";
        text += table.name + " d" + std::to_string(table.dieSides) + ": ";
        for (size_t index = 0; index < table.entries.size(); ++index) {
            if (index != 0) text += "; ";
            const EncounterTableEntry &entry = table.entries[index];
            text += std::to_string(entry.minimumRoll);
            if (entry.maximumRoll != entry.minimumRoll)
                text += "-" + std::to_string(entry.maximumRoll);
            text += "=" + entry.result;
        }
    }
    return text;
}

bool ParseEncounterBuilderFields(const std::string &creaturesText,
                                 const std::string &effectsText,
                                 const std::string &tablesText,
                                 Encounter &encounter,
                                 std::string &errorMessage) {
    encounter.creatures.clear();
    encounter.effects.clear();
    encounter.rollTables.clear();
    errorMessage.clear();

    for (const std::string &part : Split(creaturesText, ";")) {
        if (part.empty()) continue;
        size_t space = part.find(' ');
        EncounterCreature creature;
        if (space == std::string::npos || !ParsePositiveInt(part.substr(0, space), creature.count)) {
            errorMessage = "CREATURES USE: 3 GOBLINS; 1 OGRE";
            return false;
        }
        creature.name = Trim(part.substr(space + 1));
        if (creature.name.empty()) {
            errorMessage = "EACH CREATURE GROUP NEEDS A NAME";
            return false;
        }
        encounter.creatures.push_back(std::move(creature));
    }

    for (const std::string &part : Split(effectsText, ">")) {
        if (!part.empty()) encounter.effects.push_back({part});
    }

    for (const std::string &tableText : Split(tablesText, "||")) {
        if (tableText.empty()) continue;
        size_t colon = tableText.find(':');
        size_t dieMarker = colon == std::string::npos ? std::string::npos
                                                      : tableText.rfind(" d", colon);
        if (dieMarker == std::string::npos && colon != std::string::npos)
            dieMarker = tableText.rfind(" D", colon);
        if (colon == std::string::npos || dieMarker == std::string::npos) {
            errorMessage = "TABLE USE: WEATHER D6: 1-2=RAIN; 3-6=CLEAR";
            return false;
        }
        EncounterRollTable table;
        table.name = Trim(tableText.substr(0, dieMarker));
        if (table.name.empty() ||
            !ParsePositiveInt(Trim(tableText.substr(dieMarker + 2,
                                                     colon - dieMarker - 2)), table.dieSides) ||
            table.dieSides > 1000) {
            errorMessage = "TABLE NAME AND DIE SIZE ARE REQUIRED";
            return false;
        }
        for (const std::string &entryText : Split(tableText.substr(colon + 1), ";")) {
            if (entryText.empty()) continue;
            size_t equals = entryText.find('=');
            if (equals == std::string::npos) {
                errorMessage = "TABLE ROWS USE: 1-2=RESULT";
                return false;
            }
            std::string range = Trim(entryText.substr(0, equals));
            EncounterTableEntry entry;
            size_t dash = range.find('-');
            if (!ParsePositiveInt(Trim(range.substr(0, dash)), entry.minimumRoll)) {
                errorMessage = "TABLE ROLL RANGES MUST BE POSITIVE NUMBERS";
                return false;
            }
            entry.maximumRoll = entry.minimumRoll;
            if (dash != std::string::npos &&
                !ParsePositiveInt(Trim(range.substr(dash + 1)), entry.maximumRoll)) {
                errorMessage = "TABLE ROLL RANGES MUST BE POSITIVE NUMBERS";
                return false;
            }
            entry.result = Trim(entryText.substr(equals + 1));
            if (entry.minimumRoll > entry.maximumRoll || entry.maximumRoll > table.dieSides ||
                entry.result.empty()) {
                errorMessage = "TABLE ROWS MUST FIT THE DIE AND HAVE A RESULT";
                return false;
            }
            table.entries.push_back(std::move(entry));
        }
        if (table.entries.empty()) {
            errorMessage = "EACH TABLE NEEDS AT LEAST ONE ROW";
            return false;
        }
        std::sort(table.entries.begin(), table.entries.end(), [](const auto &left, const auto &right) {
            return left.minimumRoll < right.minimumRoll;
        });
        for (size_t index = 1; index < table.entries.size(); ++index) {
            if (table.entries[index].minimumRoll <= table.entries[index - 1].maximumRoll) {
                errorMessage = "TABLE ROLL RANGES CANNOT OVERLAP";
                return false;
            }
        }
        encounter.rollTables.push_back(std::move(table));
    }
    return true;
}
