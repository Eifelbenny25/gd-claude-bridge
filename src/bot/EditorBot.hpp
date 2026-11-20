#pragma once

#include <Geode/Geode.hpp>

#include "Sim.hpp"

namespace bot {

// Liest das Level aus dem Editor (Hitboxen direkt aus GD). Muss im Haupt-Thread laufen.
std::vector<LevelObj> collectLevel(LevelEditorLayer* editor, Start& start,
                                   std::vector<std::string>& warnings);

// Ergebnis als JSON (fuer den MCP-Befehl "bot_analyze")
matjson::Value resultToJson(Result const& r);

} // namespace bot
