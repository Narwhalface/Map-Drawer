#pragma once

#include "app_types.h"

#include <string>

std::string FormatEncounterCreatures(const Encounter &encounter);
std::string FormatEncounterEffects(const Encounter &encounter);
std::string FormatEncounterTables(const Encounter &encounter);

bool ParseEncounterBuilderFields(const std::string &creaturesText,
                                 const std::string &effectsText,
                                 const std::string &tablesText,
                                 Encounter &encounter,
                                 std::string &errorMessage);
