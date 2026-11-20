#pragma once

// Bot-Simulator fuer den Level-Editor.
// Reines C++ (keine Geode-Abhaengigkeit), damit er auch ausserhalb von GD
// kompiliert/getestet werden kann (siehe tests/sim_test.cpp).
//
// Physik: GD 2.2081, klassischer Modus, nach den dekompilierten Funktionen
// (PlayerObject::update/updateJump/ringJump/propellPlayer, collidedWithObjectInternal)
// wie sie in github.com/alpbayrak031-hash/GD-Remake (gd/constants.py, gd/player.py,
// gd/sim.py) mit Quellen dokumentiert sind. Alle 8 Spielmodi + Mini.
//
// Koordinaten: GD-intern wie im Editor (Boden bei y = 90, Spieler auf dem
// Boden hat die Mitte bei y = 105). 240 Physik-Ticks pro Sekunde.

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
    ModeCube, ModeShip, ModeBall, ModeUfo, ModeWave, ModeRobot, ModeSpider, ModeSwing,
    SizeMini, SizeNormal,
    Stop,                  // Dual / Teleport: Analyse endet hier
    Unsupported,           // wird ignoriert (Warnung), z. B. Spider-Orb
};

enum class Mode : uint8_t { Cube, Ship, Ball, Ufo, Wave, Robot, Spider, Swing };

struct Rect {
    float x0, y0, x1, y1; // links, unten, rechts, oben
    bool overlaps(Rect const& o) const {
        return x0 < o.x1 && o.x0 < x1 && y0 < o.y1 && o.y0 < y1;
    }
    // cocos2d intersectsRect: Kanten, die sich nur beruehren, zaehlen
    bool touches(Rect const& o) const {
        return !(x1 < o.x0 || o.x1 < x0 || y1 < o.y0 || o.y1 < y0);
    }
};

struct LevelObj {
    Kind kind;
    Rect rect;
    int uid = 0;
    int objectId = 0;
    float x = 0, y = 0;      // Mitte (fuer Meldungen, Portal-Hoehe)
    float rotation = 0;      // fuer Dash-Orbs
    bool flipX = false;      // fuer Dash-Orbs
    bool upside = false;     // Pad/Orb an der Decke (180 Grad oder gespiegelt)
    float sy0 = 0, sy1 = 0;  // Schraege: Oberflaechen-y bei rect.x0 und rect.x1
};

struct Start {
    float x = 0, y = 105;
    int speed = 1;           // 0 = 0.5x, 1 = 1x, 2 = 2x, 3 = 3x, 4 = 4x
    bool gravityFlipped = false;
    Mode mode = Mode::Cube;
    bool mini = false;
};

struct Physics {
    float floorY = 90.f;     // Boden in GD-internen Koordinaten
};

struct Settings {
    int maxSeconds = 600;
    int frontierCap = 25000;
    bool suggestNerfs = true;
};

struct PathPoint { float x, y; };

struct Click {
    int tick = 0;            // gewaehlter Tick (Mitte des Fensters)
    int windowStart = 0;     // erster funktionierender Tick
    int windowEnd = 0;       // letzter funktionierender Tick
    float x = 0, y = 0;      // Spielerposition beim Klick
    bool release = false;    // Loslassen statt Druecken
    std::string what;        // "Sprung", "gelber Orb", "Loslassen" ...
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
    bool stoppedAtUnsupported = false; // Ende der Analyse an Dual/Teleport
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
char const* modeName(Mode m);

} // namespace bot
