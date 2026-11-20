// Test des Bot-Simulators ausserhalb von GD.
// Bauen (Developer-Prompt):  cl /std:c++20 /EHsc /O2 /I..\src tests\sim_test.cpp src\bot\Sim.cpp
//
// Hitboxen hier sind Naeherungen (in GD liest die Mod die echten Rects aus).
// Koordinaten: Level-y + 90 = GD-intern.

#include "bot/Sim.hpp"

#include <cstdio>

using namespace bot;

static LevelObj mk(Kind k, float x, float y, float w, float h, int uid, int id, float rot = 0) {
    LevelObj o;
    o.kind = k;
    o.x = x;
    o.y = y + 90;
    o.rect = {x - w / 2, o.y - h / 2, x + w / 2, o.y + h / 2};
    o.uid = uid;
    o.objectId = id;
    o.rotation = rot;
    return o;
}

static void report(char const* name, Result const& r) {
    std::printf("=== %s ===\n%s", name, summary(r).c_str());
    for (auto const& c : r.clicks) {
        std::printf("  Klick x=%.0f y=%.0f  %s  -> %s\n", c.x, c.y - 90, c.what.c_str(),
                    clickLabel(c).c_str());
    }
    std::printf("  (Zustaende: %lld, Pfadpunkte: %zu)\n\n", r.statesExplored, r.path.size());
}

int main() {
    // 1) Leeres Level mit einem Stachel: ein Sprung noetig
    {
        std::vector<LevelObj> o{mk(Kind::Hazard, 300, 15, 6, 12, 1, 8),
                                mk(Kind::Solid, 600, 15, 30, 30, 2, 1)};
        report("Ein Stachel", analyze(o, {}));
    }
    // 2) Drei Stachel hintereinander (Triple Spike, 1x) - bekannt eng
    {
        std::vector<LevelObj> o{mk(Kind::Hazard, 300, 15, 6, 12, 1, 8),
                                mk(Kind::Hazard, 330, 15, 6, 12, 2, 8),
                                mk(Kind::Hazard, 360, 15, 6, 12, 3, 8),
                                mk(Kind::Solid, 700, 15, 30, 30, 4, 1)};
        report("Triple Spike 1x", analyze(o, {}));
    }
    // 3) Sechs Stachel: unmoeglich ohne Hilfe
    {
        std::vector<LevelObj> o;
        for (int i = 0; i < 6; ++i) o.push_back(mk(Kind::Hazard, 300 + 30 * i, 15, 6, 12, i + 1, 8));
        o.push_back(mk(Kind::Solid, 800, 15, 30, 30, 99, 1));
        report("Sechs Stachel", analyze(o, {}));
    }
    // 4) "Greed"-Ausschnitt (Stand aus dem Editor): Pad, 3x-Portal unter dem Boden,
    //    Dash-Orb, Bloecke mit Stachel oben und haengendem Stachel
    {
        std::vector<LevelObj> o{
            mk(Kind::PadYellow, 5, 2, 25, 4, 15, 35),
            mk(Kind::Speed3, 13, -25, 34, 88, 16, 203),
            mk(Kind::OrbDash, 245, 101, 36, 36, 14, 1704),
            mk(Kind::Solid, 319, 49, 30, 30, 17, 469),
            mk(Kind::Hazard, 319, 79, 6, 12, 18, 8),
            mk(Kind::Hazard, 355.5, 121, 6, 12, 19, 8),
            mk(Kind::Solid, 349, 49, 30, 30, 20, 469),
            mk(Kind::Solid, 379, 49, 30, 30, 21, 469),
            mk(Kind::Solid, 600, 15, 30, 30, 22, 1),
        };
        report("Greed-Ausschnitt", analyze(o, {}));
    }
    // 5) Unmoeglicher Spalt: Wand mit nur 25 Einheiten Luecke
    {
        std::vector<LevelObj> o;
        for (int r = 0; r < 10; ++r) {
            if (r == 1) continue;
            o.push_back(mk(Kind::Hazard, 400, 15 + 30 * r, 30, 30, 100 + r, 8));
        }
        o.push_back(mk(Kind::Hazard, 400, 15 + 30 + 2.5f, 30, 5, 200, 8));
        o.push_back(mk(Kind::Hazard, 400, 15 + 30 - 2.5f + 0, 30, 0.1f, 201, 8));
        o.push_back(mk(Kind::Solid, 700, 15, 30, 30, 300, 1));
        report("Enge Wand", analyze(o, {}));
    }
    return 0;
}
