#pragma once

// Bot-Simulator fuer den Level-Editor.
// Reines C++ (keine Geode-Abhaengigkeit), damit er auch ausserhalb von GD
// getestet werden kann (siehe tests/sim_test.cpp).
//
// Koordinaten: GD-intern wie im Editor (Boden bei y = 90, Spieler auf dem
// Boden hat die Mitte bei y = 105). 240 Physik-Ticks pro Sekunde wie GD 2.2.

#include <cstdint>
#include <string>
#include <vector>

namespace bot {

constexpr int TPS = 240;

enum class Kind : uint8_t {
    Solid,
    Slope,                 // Schraege mit Boden oben (sy0/sy1 = Oberflaeche)
    Hazard,
    // Orbs (Klick noetig)
    OrbYellow, OrbPink, OrbRed, OrbBlue, OrbGreen, OrbBlack,
    OrbDash, OrbDashGravity,
    // Pads (automatisch)
    PadYellow, PadPink, PadRed, PadBlue,
    // Portale
    GravityNormal, GravityFlip, GravityToggle,
    Speed05, Speed1, Speed2, Speed3, Speed4,
    ModeCube, ModeOther,   // anderer Spielmodus = wird (noch) nicht simuliert
    SizeMini, SizeNormal,
    Unsupported,           // z. B. Teleport, Spider-Orb: Warnung
};

struct Rect {
    float x0, y0, x1, y1; // links, unten, rechts, oben
    bool overlaps(Rect const& o) const {
        return x0 < o.x1 && o.x0 < x1 && y0 < o.y1 && o.y0 < y1;
    }
};

struct LevelObj {
    Kind kind;
    Rect rect;
    int uid = 0;
    int objectId = 0;
    float x = 0, y = 0;      // Mitte (fuer Meldungen)
    float rotation = 0;      // fuer Dash-Orbs
    float sy0 = 0, sy1 = 0;  // Schraege: Oberflaechen-y bei rect.x0 und rect.x1
};

struct Start {
    float x = 0, y = 105;
    int speed = 1;           // 0 = 0.5x, 1 = 1x, 2 = 2x, 3 = 3x, 4 = 4x
    bool gravityFlipped = false;
};

// Werte in Einheiten/Sekunde bzw. E/s^2. Gemessen: Gravitation, rotes Pad,
// gelber Orb (siehe Skill-Notiz physik-messwerte). Der Rest ist ueber das
// Verhaeltnis der GD-Internwerte hochgerechnet (Faktor ~53.7) = Schaetzung.
struct Physics {
    float gravity = 2800.f;
    float maxFall = 810.f;
    float jump = 600.f;
    float padYellow = 859.f, padPink = 558.f, padRed = 1076.f, padBlue = 300.f;
    float orbYellow = 601.f, orbPink = 430.f, orbRed = 827.f, orbBlue = 300.f,
          orbBlack = 810.f;
    float speeds[5] = {251.16f, 311.58f, 387.42f, 468.0f, 576.0f};
    float half = 15.f;        // halbe Spielergroesse (30x30)
    float innerHalf = 4.5f;   // innere Hitbox (Seitenkollision mit Bloecken)
    float landTolerance = 6.f;
    float floorY = 90.f;
};

struct Settings {
    int maxSeconds = 600;
    int frontierCap = 6000;
    bool suggestNerfs = true;
};

struct PathPoint { float x, y; };

struct Click {
    int tick = 0;            // gewaehlter Tick (Mitte des Fensters)
    int windowStart = 0;     // erster funktionierender Tick
    int windowEnd = 0;       // letzter funktionierender Tick
    float x = 0, y = 0;      // Spielerposition beim Klick
    bool release = false;    // Loslassen (Dash-Orb) statt Druecken
    std::string what;        // "Sprung", "gelber Orb", ...
    int windowTicks() const { return windowEnd - windowStart + 1; }
};

struct Nerf {
    int uid = 0;
    int objectId = 0;
    float x = 0, y = 0;
    std::string change;      // Beschreibung
    bool makesPossible = false;
    float reachedX = 0;
};

struct Result {
    bool possible = false;
    bool stoppedAtUnsupported = false; // Ende der Analyse an einem Modus-Portal
    float endX = 0;
    float reachedX = 0;                // am weitesten gekommen
    float deathX = 0, deathY = 0;
    int killerUid = 0, killerObjectId = 0;
    std::string killerWhat;
    std::vector<PathPoint> path;
    std::vector<Click> clicks;
    std::vector<std::string> warnings;
    std::vector<std::string> unusedObjects; // Orbs/Pads, die der Bot nicht braucht
    std::vector<Nerf> nerfs;
    long long statesExplored = 0;
};

Result analyze(std::vector<LevelObj> const& objects, Start const& start,
               Physics const& phys = {}, Settings const& settings = {});

// Kurzer Klartext-Bericht (Deutsch) fuer Popup / Log
std::string summary(Result const& r);
std::string clickLabel(Click const& c);
char const* kindName(Kind k);

} // namespace bot
