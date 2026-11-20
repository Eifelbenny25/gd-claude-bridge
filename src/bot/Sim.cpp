#include "Sim.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace bot {

namespace {

constexpr float PI = 3.14159265f;

// ---------------------------------------------------------------------------
// GD-Konstanten (native Einheiten: 1 Geschwindigkeits-Einheit = 54 E/s)
// Quelle: GD-Remake gd/constants.py (aus dekompiliertem GD 2.2081)
// ---------------------------------------------------------------------------
struct SpeedRow { float playerSpeed, yStart, gravity, mult; };
constexpr SpeedRow SPEED[5] = {
    {0.7f, 10.620032f, 0.940199f, 5.980002f},   // 0.5x
    {0.9f, 11.180032f, 0.958199f, 5.770002f},   // 1x
    {1.1f, 11.420032f, 0.957199f, 5.870002f},   // 2x
    {1.3f, 11.230032f, 0.961199f, 6.000002f},   // 3x
    {1.6f, 11.230032f, 0.961199f, 6.000002f},   // 4x
};
constexpr float DT = 0.25f;
constexpr float DTS = 0.225f;           // DT * 0.9
constexpr float GRAV_FLY = 0.958199f;
constexpr float MINI_SCALE = 0.6f, MINI_JUMP = 0.8f, MINI_FLY = 0.85f;
constexpr float TERMINAL = 15.f;
constexpr float SNAP = 10.f, SNAP_FLY = 6.f;
constexpr float INNER = 0.3f;
constexpr int FLIP_GRACE_TICKS = 24;    // 0.1 s
constexpr float MAX_Y_LEVEL = 1476.3f;

double round3(double v) {
    double whole = std::trunc(v);
    if (whole != v) {
        double frac = (v - whole) * 1000.0;
        frac = frac >= 0 ? std::floor(frac + 0.5) : -std::floor(-frac + 0.5);
        v = whole + frac / 1000.0;
    }
    return v;
}

bool isFlyingMode(Mode m) {
    return m == Mode::Ship || m == Mode::Ufo || m == Mode::Wave || m == Mode::Swing;
}
bool isOrb(Kind k) { return k >= Kind::OrbYellow && k <= Kind::OrbDashGravity; }
bool isPad(Kind k) { return k >= Kind::PadYellow && k <= Kind::PadBlue; }

// ---------------------------------------------------------------------------
// Zustand des Spielers in einem Tick
// ---------------------------------------------------------------------------
struct State {
    float x = 0, y = 0, lastY = 0;
    double yv = 0;
    float robotCharge = 0, dashY = 0, slopeVel = 0;
    float floorB = 90, ceilB = -1;      // Spielfeld-Grenzen (ceilB < 0: keine Decke)
    int8_t speed = 1;
    Mode mode = Mode::Cube;
    bool mini = false, flipped = false;
    bool holding = false, jumpUnused = false, holdDelayed = false, canRing = false, touchedRing = false;
    bool canJump = false, onGround = false, isJumping = false, uncapped = false;
    bool robotInvalid = false, dashing = false, onSlope = false, wasOnSlope = false;
    bool dead = false, finished = false, stopped = false;
    uint8_t sinceFlip = 255, slopeTicks = 0;
    int16_t usedOrb = -1, usedPad = -1, usedPortal = -1, slope = -1;
    int16_t killer = -1;
    bool terminal() const { return finished || stopped; }
    float flip() const { return flipped ? -1.f : 1.f; }
};

float halfSize(State const& s) {
    float base = s.mode == Mode::Wave ? 10.f : s.mode == Mode::Spider ? 27.f : 30.f;
    return base * (s.mini ? MINI_SCALE : 1.f) / 2.f;
}
float xVel(State const& s) { return SPEED[s.speed].playerSpeed * SPEED[s.speed].mult; }

struct Key {
    int32_t xq, yq, vq;
    int16_t rq, fq, cq;
    int16_t usedOrb, usedPad, usedPortal, slope;
    int16_t pad0;
    uint32_t bits;
    uint32_t pad;
    bool operator==(Key const& o) const { return std::memcmp(this, &o, sizeof(Key)) == 0; }
    bool operator<(Key const& o) const { return std::memcmp(this, &o, sizeof(Key)) < 0; }
};
static_assert(sizeof(Key) == 36, "Key ohne Padding");

struct KeyHash {
    size_t operator()(Key const& k) const {
        uint64_t h = 1469598103934665603ull;
        auto p = reinterpret_cast<unsigned char const*>(&k);
        for (size_t i = 0; i < sizeof(Key); ++i) {
            h ^= p[i];
            h *= 1099511628211ull;
        }
        return static_cast<size_t>(h);
    }
};

Key keyOf(State const& s) {
    Key k;
    std::memset(&k, 0, sizeof(k));
    k.xq = static_cast<int32_t>(std::lround(s.x * 4.f));
    k.yq = static_cast<int32_t>(std::lround(s.y * 2.f));
    k.vq = static_cast<int32_t>(std::lround(s.yv * 20.0));
    k.rq = static_cast<int16_t>(std::lround(s.robotCharge * 10.f));
    k.fq = static_cast<int16_t>(std::lround(s.floorB));
    k.cq = static_cast<int16_t>(std::lround(s.ceilB));
    k.usedOrb = s.usedOrb;
    k.usedPad = s.usedPad;
    k.usedPortal = s.usedPortal;
    k.slope = s.slope;
    uint32_t b = 0;
    auto put = [&](bool v) { b = (b << 1) | (v ? 1u : 0u); };
    put(s.mini); put(s.flipped); put(s.holding); put(s.jumpUnused); put(s.holdDelayed);
    put(s.canRing); put(s.canJump); put(s.onGround); put(s.isJumping); put(s.uncapped);
    put(s.robotInvalid); put(s.dashing); put(s.onSlope); put(s.finished); put(s.stopped);
    put(s.sinceFlip < FLIP_GRACE_TICKS);
    b = (b << 3) | static_cast<uint32_t>(s.speed);
    b = (b << 3) | static_cast<uint32_t>(s.mode);
    k.bits = b;
    return k;
}

// Grobe Version des Schluessels (2 Einheiten / 0.4 Geschwindigkeit) fuer das
// Nachschlagen, wenn der genaue Zustand beim Ausduennen weggefallen ist.
Key coarseOf(Key k) {
    auto fdiv = [](int32_t a, int32_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); };
    k.yq = fdiv(k.yq, 4);
    k.vq = fdiv(k.vq, 8);
    k.xq = fdiv(k.xq, 4);
    return k;
}

// ---------------------------------------------------------------------------
// Welt: Objekte + schnelle Suche nach x
// ---------------------------------------------------------------------------
struct World {
    std::vector<LevelObj> const& objs;
    Physics const& phys;
    std::vector<int> order;   // nach rect.x0 sortiert
    float maxWidth = 0;
    float endX = 0;

    World(std::vector<LevelObj> const& o, Physics const& p) : objs(o), phys(p) {
        order.resize(objs.size());
        for (size_t i = 0; i < objs.size(); ++i) order[i] = static_cast<int>(i);
        std::sort(order.begin(), order.end(), [&](int a, int b) {
            return objs[a].rect.x0 < objs[b].rect.x0;
        });
        for (auto const& ob : objs) {
            maxWidth = std::max(maxWidth, ob.rect.x1 - ob.rect.x0);
            endX = std::max(endX, ob.rect.x1);
        }
        endX += 60.f;
    }

    template <class F>
    void query(Rect const& r, F&& f) const {
        float lo = r.x0 - maxWidth - 1.f;
        auto it = std::lower_bound(order.begin(), order.end(), lo, [&](int i, float v) {
            return objs[i].rect.x0 < v;
        });
        for (; it != order.end(); ++it) {
            auto const& ob = objs[*it];
            if (ob.rect.x0 > r.x1) break;
            if (ob.rect.touches(r)) f(*it, ob);
        }
    }
};

Rect boxOf(State const& s, float h) { return {s.x - h, s.y - h, s.x + h, s.y + h}; }

// ---------------------------------------------------------------------------
// Spieler-Physik (PlayerObject)
// ---------------------------------------------------------------------------
bool fallingBugged(State const& s) {
    double thr = 2.0 * SPEED[s.speed].gravity;
    if (s.mode == Mode::Swing) return s.flipped ? s.yv > -thr : s.yv < thr;
    return s.flipped ? s.yv > thr : s.yv < thr;
}

void hitGround(State& s) {
    s.yv = 0;
    s.canJump = true;
    s.onGround = true;
    s.isJumping = false;
}

bool flipGravity(State& s, bool f) {
    if (s.flipped == f) return false;
    s.flipped = f;
    s.sinceFlip = 0;
    s.yv *= 0.5;
    s.canJump = false;
    return true;
}

void boundsAround(State& s, float portalY, Mode m, float floorY) {
    float b = 0;
    switch (m) {
        case Mode::Ship: case Mode::Ufo: case Mode::Wave: case Mode::Swing: b = 300.f; break;
        case Mode::Ball: case Mode::Spider: b = 240.f; break;
        default: break;
    }
    if (b == 0) {
        s.floorB = floorY;
        s.ceilB = -1;
        return;
    }
    float lvlY = portalY - floorY;
    float fl = std::max(0.f, 30.f * std::floor((lvlY - b / 2.f) / 30.f));
    s.floorB = floorY + fl;
    s.ceilB = s.floorB + b;
}

bool setMode(State& s, Mode m) {
    if (s.mode == m) return false;
    for (Mode x : {s.mode, m}) {
        if (isFlyingMode(x)) {
            s.yv *= 0.5;
            s.onGround = false;
            s.canJump = false;
        }
    }
    if (m == Mode::Robot || m == Mode::Spider) s.robotCharge = 1.5f;
    s.mode = m;
    return true;
}

void updateJump(State& s, float dt) {
    Mode mode = s.mode;
    bool up = s.flipped;
    double flip = up ? -1.0 : 1.0;
    bool holding = s.holding;
    bool isHolding = holding;
    if (mode == Mode::Robot && !s.jumpUnused) isHolding = false;
    bool flying = isFlyingMode(mode);
    double gravity = (flying || mode == Mode::Ball || mode == Mode::Spider) ? GRAV_FLY
                                                                           : SPEED[s.speed].gravity;
    double scale = s.mini ? MINI_JUMP : 1.0;

    if (flying) {
        if (s.mini) scale = MINI_FLY;
        double maxY = 8.0 / scale, minY = -6.4 / scale;
        double v = s.yv;
        bool recap = !up ? ((0.0 <= v && v < maxY) || (minY < v && v <= 0.0))
                         : ((-maxY < v && v <= 0.0) || (0.0 <= v && v < -minY));
        if (recap) s.uncapped = false;

        if (mode == Mode::Ship) {
            double shipMod;
            if (holding && !s.uncapped) shipMod = -1.0;
            else {
                if (s.uncapped) {
                    if (up) shipMod = s.yv > 0 ? -1.0 : 0.8;
                    else shipMod = s.yv < 0 ? -1.0 : 0.8;
                }
                else shipMod = 0.8;
                if (!holding && !fallingBugged(s)) shipMod = 1.2;
            }
            double boost = (holding && fallingBugged(s)) ? 0.5 : 0.4;
            s.yv = round3(s.yv - (gravity * dt * flip * shipMod * boost) / scale);
            if (holding) s.onGround = false;
        }
        else if (mode == Mode::Ufo) {
            if (s.jumpUnused && holding) {
                s.jumpUnused = false;
                double j = (s.mini ? 8.0 : 7.0) * flip * scale;
                if ((up && j < s.yv) || (!up && j > s.yv)) s.yv = round3(j);
            }
            double birdMod = fallingBugged(s) ? 0.8 : 1.2;
            s.yv = round3(s.yv - (gravity * dt * flip * birdMod * 0.5) / scale);
            if (holding) s.onGround = false;
        }
        else if (mode == Mode::Wave) {
            s.yv = round3(xVel(s) * flip * (holding ? 1.0 : -1.0));
        }
        else { // Swing
            if (s.jumpUnused && holding) {
                s.jumpUnused = false;
                double before = s.yv;
                flipGravity(s, !up);
                s.yv = round3(before * 0.8);
                up = s.flipped;
                flip = -flip;
            }
            double swingMod = s.mini ? 0.6 : 0.4;
            s.yv = round3(s.yv - swingMod * (gravity * dt * flip));
            if (holding) s.onGround = false;
            scale = 1.0;
        }

        double modeScale = mode == Mode::Swing ? 1.0 : 0.8;
        if (!s.uncapped && mode != Mode::Wave) {
            if (!up) {
                s.yv = round3(std::max(s.yv, modeScale * -8.0 / scale));
                s.yv = round3(std::min(s.yv, 8.0 / scale));
            }
            else {
                s.yv = round3(std::max(s.yv, -8.0 / scale));
                s.yv = round3(std::min(s.yv, modeScale * 8.0 / scale));
            }
        }
        if (fallingBugged(s)) s.isJumping = false;
        return;
    }

    // Cube, Ball, Robot, Spider
    double gs = (mode == Mode::Ball || mode == Mode::Spider) ? 0.6 : mode == Mode::Robot ? 0.9 : 1.0;
    if (isHolding && s.canJump && mode != Mode::Spider && !s.dashing) {
        s.isJumping = true;
        s.onGround = false;
        s.canJump = false;
        s.jumpUnused = false;
        s.robotInvalid = false;
        s.robotCharge = 0;
        double j = SPEED[s.speed].yStart;
        if (mode == Mode::Robot) j *= 0.5;
        s.yv = round3(j * flip * scale);
        if ((s.wasOnSlope || s.onSlope) && s.slopeVel * flip > 0.0) {
            double thr = s.yv * 1.4;
            double sv = s.slopeVel;
            if (mode != Mode::Ball) {
                double t = s.slopeTicks / static_cast<double>(TPS);
                if (t < 0.1) sv *= std::max(0.4, t * 10.0);
            }
            s.yv = round3(s.yv + sv * 0.25);
            s.yv = round3(!up ? std::min(s.yv, thr) : std::max(s.yv, thr));
        }
        if (mode == Mode::Ball) {
            flipGravity(s, !up);
            s.yv *= 0.6;
            s.holding = false;
        }
    }
    else if (isHolding && s.canJump && mode == Mode::Spider && !s.dashing) {
        // Spider-Teleport wird in pushButton() ausgefuehrt (braucht die Welt)
    }
    else if (s.isJumping) {
        if (mode == Mode::Robot && holding && !s.robotInvalid && s.robotCharge < 1.5f) {
            s.robotCharge += dt * 0.1f;
            s.yv = round3(s.yv + gravity * dt * flip * gs);
        }
        s.yv = round3(s.yv - gs * gravity * dt * flip);
        if (fallingBugged(s)) {
            s.isJumping = false;
            s.onGround = false;
        }
    }
    else {
        if (fallingBugged(s)) s.canJump = false;
        s.yv = round3(s.yv - gs * gravity * dt * flip);
        s.yv = round3(!up ? std::max(s.yv, -static_cast<double>(TERMINAL))
                          : std::min(s.yv, static_cast<double>(TERMINAL)));
        if (fallingBugged(s) && ((up && s.yv > 4.0) || (!up && s.yv < -4.0))) s.onGround = false;
    }
}

// Spider-Teleport: zur naechsten Flaeche in Richtung "oben"
void spiderTeleport(World const& w, State& s) {
    float hh = halfSize(s);
    float dir = s.flipped ? -1.f : 1.f;
    float edge = s.y + dir * hh;
    float far = dir > 0 ? w.phys.floorY + MAX_Y_LEVEL : w.phys.floorY - 30.f;
    Rect col{s.x - hh + 1.f, std::min(edge, far), s.x + hh - 1.f, std::max(edge, far)};
    float best = 1e9f;
    int bestObj = -1;
    w.query(col, [&](int i, LevelObj const& ob) {
        if (ob.kind != Kind::Solid && ob.kind != Kind::Hazard) return;
        float face = dir > 0 ? ob.rect.y0 : ob.rect.y1;
        float dist = (face - edge) * dir;
        if (dist >= -1e-3f && dist < best) { best = dist; bestObj = i; }
    });
    float bound = dir > 0 ? s.ceilB : s.floorB;
    bool hasBound = dir > 0 ? s.ceilB >= 0 : true;
    float boundDist = hasBound ? (bound - edge) * dir : 1e9f;

    flipGravity(s, !s.flipped);
    s.isJumping = false;
    if (bestObj >= 0 && best <= boundDist) {
        s.y = edge + dir * best - dir * hh;
        if (w.objs[bestObj].kind == Kind::Hazard) {
            s.dead = true;
            s.killer = static_cast<int16_t>(bestObj);
            return;
        }
        s.yv = 0;
    }
    else if (hasBound) {
        s.y = bound - dir * hh;
        s.yv = 0;
    }
    s.holding = false;
}

void pushButton(World const& w, State& s) {
    s.holding = true;
    s.jumpUnused = true;
    bool spiderJump = s.mode == Mode::Spider && s.canJump && !s.dashing;
    updateJump(s, 0.f);
    if (spiderJump) {
        s.jumpUnused = false;
        s.canJump = false;
        spiderTeleport(w, s);
    }
}

void releaseButton(State& s) {
    s.holding = false;
    if (s.dashing) s.dashing = false;
    if (s.mode == Mode::Robot) s.robotInvalid = true;
}

void startDashing(State& s, LevelObj const& ob) {
    if (s.dashing) return;
    s.dashing = true;
    auto snap360 = [](float a) {
        a = std::fmod(a, 360.f);
        if (a > 180.f) a -= 360.f;
        else if (a <= -180.f) a += 360.f;
        return a;
    };
    float ringRot = ob.rotation + (ob.flipX ? 180.f : 0.f);
    float angle = snap360(std::fmod(-ringRot, 360.f));
    if (std::abs(angle) > 90.f) angle = snap360((angle <= 0.f ? -180.f : 180.f) - angle);
    angle = std::clamp(angle, -70.f, 70.f);
    float rad = angle * PI / 180.f;
    s.dashY = std::sin(rad) / std::abs(std::cos(rad));
}

void ringJump(State& s, LevelObj const& ob) {
    Mode m = s.mode;
    bool mini = s.mini;
    switch (ob.kind) {
        case Kind::OrbDash: startDashing(s, ob); return;
        case Kind::OrbDashGravity:
            flipGravity(s, !s.flipped);
            startDashing(s, ob);
            return;
        case Kind::OrbBlack: {
            s.jumpUnused = false;
            double v;
            if (isFlyingMode(m)) {
                v = s.flip() * -14.0;
                if (m == Mode::Ufo) v *= 0.8;
            }
            else {
                v = s.flip() * -15.0;
                if (m == Mode::Spider) v *= 1.1;
            }
            s.yv = round3(v);
            s.uncapped = true;
            if (m == Mode::Ball || m == Mode::Swing) s.holding = false;
            return;
        }
        default: break;
    }
    s.jumpUnused = false;
    s.isJumping = true;
    s.onGround = false;
    s.canJump = false;
    double scale = mini ? MINI_JUMP : 1.0;
    double vel = SPEED[s.speed].yStart;
    switch (ob.kind) {
        case Kind::OrbBlue: vel *= 0.8; break;
        case Kind::OrbGreen: if (m == Mode::Ship) vel *= 0.7; break;
        case Kind::OrbPink:
            vel *= m == Mode::Ship ? 0.37 : m == Mode::Ufo ? 0.42 : m == Mode::Ball ? 0.77 : 0.72;
            break;
        case Kind::OrbRed:
            if (m == Mode::Ship) vel *= mini ? 1.4 : 1.0;
            else if (m == Mode::Ufo) vel *= mini ? 1.36 : 1.02;
            else if (m == Mode::Ball || m == Mode::Spider) vel *= 1.34;
            else if (m == Mode::Robot) vel *= 1.28;
            else vel *= 1.38;
            break;
        default: // gelb
            if (m == Mode::Robot) vel *= 0.9;
            break;
    }
    if (ob.kind == Kind::OrbGreen) flipGravity(s, !s.flipped);
    s.yv = round3(s.flip() * vel * scale);
    if (m == Mode::Ball || m == Mode::Spider) {
        s.yv *= 0.7;
        s.holding = false;
    }
    else if (m == Mode::Swing) {
        s.yv *= 0.6;
        s.holding = false;
    }
    if (ob.kind == Kind::OrbBlue) flipGravity(s, !s.flipped);
    if (ob.kind == Kind::OrbRed) s.uncapped = true;
}

void propel(State& s, double bumpMod) {
    s.isJumping = true;
    s.onGround = false;
    s.canJump = false;
    s.onSlope = false;
    s.wasOnSlope = false;
    double scale = s.mini ? MINI_JUMP : 1.0;
    s.yv = round3(bumpMod * 16.0 * s.flip() * scale);
    if (s.mode == Mode::Ball || s.mode == Mode::Spider || s.mode == Mode::Swing) s.yv *= 0.6;
}

void bump(State& s, Kind k) {
    s.robotInvalid = true;
    Mode m = s.mode;
    double mod = 1.0;
    if (k == Kind::PadPink) {
        mod = m == Mode::Ship ? 0.35 : m == Mode::Ufo ? 0.4 : m == Mode::Ball ? 0.7 : 0.65;
    }
    else if (k == Kind::PadRed) {
        if (m == Mode::Ship) mod = s.mini ? 0.95 : 0.63;
        else if (m == Mode::Ufo) mod = s.mini ? 0.98 : 0.6;
        else mod = 1.25;
    }
    propel(s, mod);
    s.uncapped = (k == Kind::PadRed);
}

// ---------------------------------------------------------------------------
// Kollisionen (GJBaseGameLayer::checkCollisions / collidedWithObjectInternal)
// ---------------------------------------------------------------------------
void killBy(State& s, int i) {
    if (s.dead) return;
    s.dead = true;
    s.killer = static_cast<int16_t>(i);
}

void solidCollide(State& s, Rect const& o, int idx) {
    float hh = halfSize(s);
    Rect p = boxOf(s, hh);
    if (!p.touches(o)) return;
    bool up = s.flipped;
    bool fly = isFlyingMode(s.mode);
    float thr = fly ? SNAP_FLY : SNAP;
    float dy = s.y - s.lastY;
    double yv = s.yv;
    float g = up ? -1.f : 1.f;
    bool lethal = dy * g > 0.f;
    bool headCandidate = false;

    if (s.mode == Mode::Wave) {
        lethal = true;
    }
    else {
        bool bumpsHead = fly || s.mode == Mode::Ball;
        if (up && !bumpsHead) {
            lethal = true;
        }
        else {
            float low = p.y0 + thr;
            if (low < o.y1 && low - dy < o.y1) lethal = true;
            else if (yv > 0.0) lethal = false;
            else {
                s.y = o.y1 + hh;
                if (!up) hitGround(s);
                else if (s.yv < 0) s.yv = 0;
                return;
            }
        }
        if (lethal && (up || bumpsHead)) {
            float high = p.y1 - thr;
            if (!(o.y0 < high && o.y0 < high - dy)) {
                if (yv >= 0.0) {
                    s.y = o.y0 - hh;
                    if (up) hitGround(s);
                    else if (s.yv > 0) s.yv = 0;
                    return;
                }
            }
        }
        if (up) headCandidate = (p.y1 - thr >= o.y1) || (p.y1 - thr - dy >= o.y1);
        else headCandidate = (p.y0 + thr <= o.y0) || (p.y0 + thr - dy <= o.y0);
    }
    if (!lethal) return;
    if (!boxOf(s, hh * INNER).touches(o)) return;
    if (headCandidate && s.sinceFlip < FLIP_GRACE_TICKS) {
        s.y = up ? o.y1 + hh : o.y0 - hh;
        hitGround(s);
        s.onGround = false;
        return;
    }
    killBy(s, idx);
}

void bounds(World const& w, State& s) {
    float hh = halfSize(s);
    bool up = s.flipped;
    float ext = s.mode == Mode::Wave ? hh * 2.f : hh;
    float floor = s.floorB;
    bool hasCeil = s.ceilB >= 0;
    float ceil = s.ceilB;
    if (s.y - ext <= floor) {
        if (s.mode == Mode::Wave) s.y = floor + ext;
        else if (!up) {
            if (s.yv <= 0.0) {
                s.y = floor + ext;
                hitGround(s);
            }
        }
        else if (s.y - ext < floor) {
            if (!hasCeil) {
                s.dead = true; // umgedreht auf den Boden
                return;
            }
            s.y = floor + ext;
            if (s.yv < 0) s.yv = 0;
        }
    }
    if (hasCeil && s.y + ext >= ceil) {
        if (s.mode == Mode::Wave) s.y = ceil - ext;
        else if (up) {
            if (s.yv >= 0.0) {
                s.y = ceil - ext;
                hitGround(s);
            }
        }
        else if (s.y + ext > ceil) {
            s.y = ceil - ext;
            if (s.yv > 0) s.yv = 0;
        }
    }
    if (s.y - w.phys.floorY > MAX_Y_LEVEL) s.dead = true;
}

void collide(World const& w, State& s) {
    s.wasOnSlope = s.onSlope;
    s.onSlope = false;
    bounds(w, s);
    if (s.dead) return;

    float hh = halfSize(s);
    std::vector<int> solids, hazards;
    bool launch = false;
    float launchY = 0;

    w.query(boxOf(s, hh), [&](int i, LevelObj const& ob) {
        if (s.dead || s.stopped) return;
        float h = halfSize(s);
        if (!boxOf(s, h).touches(ob.rect)) return;
        switch (ob.kind) {
            case Kind::Solid: solids.push_back(i); return;
            case Kind::Hazard: hazards.push_back(i); return;
            case Kind::Speed05: s.speed = 0; return;
            case Kind::Speed1: s.speed = 1; return;
            case Kind::Speed2: s.speed = 2; return;
            case Kind::Speed3: s.speed = 3; return;
            case Kind::Speed4: s.speed = 4; return;
            case Kind::GravityNormal: flipGravity(s, false); return;
            case Kind::GravityFlip: flipGravity(s, true); return;
            case Kind::GravityToggle:
                if (s.usedPortal != i) {
                    s.usedPortal = static_cast<int16_t>(i);
                    flipGravity(s, !s.flipped);
                }
                return;
            case Kind::SizeMini: s.mini = true; return;
            case Kind::SizeNormal: s.mini = false; return;
            case Kind::ModeCube: case Kind::ModeShip: case Kind::ModeBall: case Kind::ModeUfo:
            case Kind::ModeWave: case Kind::ModeRobot: case Kind::ModeSpider: case Kind::ModeSwing: {
                Mode m = static_cast<Mode>(static_cast<int>(ob.kind) - static_cast<int>(Kind::ModeCube));
                if (s.usedPortal != i) {
                    s.usedPortal = static_cast<int16_t>(i);
                    setMode(s, m);
                    boundsAround(s, ob.y, m, w.phys.floorY);
                }
                return;
            }
            case Kind::Stop:
                s.stopped = true;
                s.killer = static_cast<int16_t>(i);
                return;
            case Kind::Slope: {
                float w0 = std::max(1.f, ob.rect.x1 - ob.rect.x0);
                float ratio = (ob.rect.y1 - ob.rect.y0) / w0;
                float angle = std::atan(ratio);
                bool rises = ob.sy1 > ob.sy0;
                float surf = rises ? ob.rect.y0 + (s.x - ob.rect.x0) * ratio
                                   : ob.rect.y1 - (s.x - ob.rect.x0) * ratio;
                float rad = h / std::cos(angle);
                float limit = ob.rect.y1 + h;
                float target = std::max(std::min(surf + rad, limit), ob.rect.y0);
                if (s.flipped) {
                    // Diagonale auf der Kopfseite
                    if (!(s.y < target)) return;
                    if (s.mode == Mode::Wave) { killBy(s, i); return; }
                    if (isFlyingMode(s.mode) || s.mode == Mode::Ball) {
                        s.y = target;
                        if (s.yv < 0) s.yv = 0;
                    }
                    else if (target - s.y <= 2.f) {
                        s.y = target;
                        s.yv = round3(std::max(s.yv, 2.0));
                        s.canJump = false;
                        s.onGround = false;
                    }
                    else killBy(s, i);
                    return;
                }
                bool attached = s.wasOnSlope && s.slope == i;
                if (!attached && !rises && s.x < ob.rect.x0) {
                    // senkrechte Seite einer abfallenden Schraege wirkt wie ein duenner Block
                    Rect wall{ob.rect.x0, ob.rect.y0, ob.rect.x0 + 1.f, ob.rect.y1};
                    if (boxOf(s, h).touches(wall)) {
                        solidCollide(s, wall, i);
                        return;
                    }
                }
                bool inside = s.y < target;
                bool uphill = rises;
                bool collided = inside || (attached && !uphill && s.yv <= 0 && s.x <= ob.rect.x1);
                if (!collided) return;
                if (s.mode == Mode::Wave) { killBy(s, i); return; }
                float slopeYVel = ratio * xVel(s);
                float l = std::min(1.12f / std::max(angle, 0.01f), 1.54f) * slopeYVel;
                if (isFlyingMode(s.mode) || s.mode == Mode::Ball) l *= 0.75f;
                s.slopeVel = l * (uphill ? 1.f : -1.f);
                if (attached && uphill && target == limit) {
                    launch = true;
                    launchY = target;
                    return;
                }
                double before = s.yv;
                s.y = target;
                hitGround(s);
                if (before > 5.0) {
                    s.yv = before;
                    s.canJump = false;
                }
                if (!s.wasOnSlope) s.slopeTicks = 0;
                s.slope = static_cast<int16_t>(i);
                s.onSlope = true;
                return;
            }
            default: break;
        }
        if (isOrb(ob.kind)) {
            if (s.usedOrb == i || !s.canRing || s.dashing || !s.holdDelayed || s.touchedRing) return;
            s.usedOrb = static_cast<int16_t>(i);
            s.touchedRing = true;
            ringJump(s, ob);
        }
        else if (isPad(ob.kind)) {
            if (s.usedPad == i) return;
            if (ob.kind == Kind::PadBlue) {
                if (ob.upside != s.flipped) return;
                s.usedPad = static_cast<int16_t>(i);
                s.robotInvalid = true;
                propel(s, 0.8);
                flipGravity(s, !s.flipped);
                s.uncapped = false;
            }
            else {
                s.usedPad = static_cast<int16_t>(i);
                bump(s, ob.kind);
            }
        }
    });
    if (s.dead || s.stopped) return;

    if (launch && !s.onSlope) {
        s.y = launchY;
        double sv = s.slopeVel;
        double t = s.slopeTicks / static_cast<double>(TPS);
        if (t < 0.1) sv *= std::max(0.4, t * 10.0);
        s.yv = round3(sv);
        s.isJumping = true;
        s.canJump = false;
        s.onGround = false;
        s.slope = -1;
    }

    // Bloecke (auf einer Schraege: Bloecke unter ihrer Oberflaeche ignorieren)
    for (int i : solids) {
        auto const& o = w.objs[i].rect;
        if (s.slope >= 0 && (s.onSlope || s.wasOnSlope)) {
            auto const& so = w.objs[s.slope];
            float sw = so.rect.x1 - so.rect.x0, sh = so.rect.y1 - so.rect.y0;
            float cx = (so.rect.x0 + so.rect.x1) / 2, cy = (so.rect.y0 + so.rect.y1) / 2;
            Rect big{cx - sw * 0.6f, cy - sh * 0.55f, cx + sw * 0.6f, cy + sh * 0.55f};
            if (o.touches(big)) {
                float ex = std::clamp((o.x0 + o.x1) / 2, so.rect.x0, so.rect.x1);
                float r = sh / std::max(1.f, sw);
                bool rises = so.sy1 > so.sy0;
                float surf = rises ? so.rect.y0 + (ex - so.rect.x0) * r : so.rect.y1 - (ex - so.rect.x0) * r;
                if (o.y1 <= surf + 2.f) continue;
            }
        }
        solidCollide(s, o, i);
        if (s.dead) return;
    }

    Rect p = boxOf(s, halfSize(s));
    for (int i : hazards) {
        if (p.touches(w.objs[i].rect)) {
            killBy(s, i);
            return;
        }
    }

    if (s.wasOnSlope && !s.onSlope && s.slope >= 0) {
        auto const& so = w.objs[s.slope];
        bool uphill = so.sy1 > so.sy0;
        if (!uphill && !s.isJumping && s.x > so.rect.x1) {
            float r = (so.rect.y1 - so.rect.y0) / std::max(1.f, so.rect.x1 - so.rect.x0);
            s.yv = round3(-s.flip() * xVel(s) * r);
            s.canJump = false;
        }
        s.slope = -1;
    }
}

// Ein Tick. action = Knopf umschalten (druecken bzw. loslassen).
State step(World const& w, State s, bool action) {
    if (action) {
        if (s.holding) releaseButton(s);
        else pushButton(w, s);
        if (s.dead) return s;
    }
    // PlayerObject::update
    s.yv = std::clamp(s.yv, -1000.0, 1000.0);
    s.lastY = s.y;
    updateJump(s, DTS);
    if (s.dashing) s.yv = 0;
    float dx = xVel(s) * DT;
    float dy = static_cast<float>(s.yv) * DTS;
    if (s.mode == Mode::Wave && !s.dashing) {
        dy = std::abs(dx) * s.flip() * (s.holding ? 1.f : -1.f);
        if (s.mini) dy += dy;
    }
    else if (s.dashing) {
        dy = dx * s.dashY;
    }
    s.x += dx;
    s.y += dy;
    s.holdDelayed = s.holding;
    s.canRing = s.jumpUnused;
    s.touchedRing = false;
    if (s.sinceFlip < 255) ++s.sinceFlip;
    if (s.slopeTicks < 255) ++s.slopeTicks;

    collide(w, s);
    if (!s.dead && s.x >= w.endX) s.finished = true;
    return s;
}

// ---------------------------------------------------------------------------
// Suche (Breitensuche ueber alle Ticks, Zustaende werden zusammengelegt)
// ---------------------------------------------------------------------------
struct Node {
    State s;
    int parent;
};
struct Edge {
    int child, parent;
};

struct Search {
    std::vector<std::vector<Node>> layers;
    std::vector<std::vector<Edge>> extras;
    std::vector<std::vector<std::pair<Key, int>>> keys;
    std::vector<std::vector<char>> good;
    std::vector<std::vector<Key>> goodCoarse;              // sortiert, pro Tick
    float reachedX = 0;
    float deathX = -1e9f, deathY = 0;
    int furthestKiller = -1;
    bool pruned = false;
    long long explored = 0;

    bool isGood(int tick, State const& s) const {
        if (tick < 0 || tick >= static_cast<int>(keys.size())) return false;
        if (s.dead) return false;
        Key k = keyOf(s);
        auto const& v = keys[tick];
        auto it = std::lower_bound(v.begin(), v.end(), k, [](auto const& a, Key const& b) {
            return a.first < b;
        });
        if (it != v.end() && it->first == k) return good[tick][it->second] != 0;
        // Genau dieser Zustand fehlt (Rundung/Ausduennen): grob nachschauen
        Key c = coarseOf(k);
        auto const& g = goodCoarse[tick];
        return std::binary_search(g.begin(), g.end(), c);
    }
};

void runSearch(World const& w, State const& start, Settings const& st, Search& S, float stopAtX) {
    S.layers.push_back({Node{start, -1}});
    S.extras.push_back({});
    int maxTicks = st.maxSeconds * TPS;

    for (int t = 0; t < maxTicks; ++t) {
        auto const& cur = S.layers[t];
        std::vector<Node> next;
        std::vector<Edge> ex;
        std::unordered_map<Key, int, KeyHash> map;
        map.reserve(cur.size() * 2 + 4);
        bool anyAlive = false;

        auto add = [&](State const& c, int parent) {
            ++S.explored;
            if (c.dead) {
                if (c.x > S.deathX) {
                    S.deathX = c.x;
                    S.deathY = c.y;
                    S.furthestKiller = c.killer;
                }
                return;
            }
            State cc = c;
            if (stopAtX > 0 && cc.x >= stopAtX) cc.finished = true;
            S.reachedX = std::max(S.reachedX, cc.x);
            Key k = keyOf(cc);
            auto [it, inserted] = map.try_emplace(k, static_cast<int>(next.size()));
            if (inserted) {
                next.push_back(Node{cc, parent});
                if (!cc.terminal()) anyAlive = true;
            }
            else {
                ex.push_back(Edge{it->second, parent});
            }
        };

        for (int i = 0; i < static_cast<int>(cur.size()); ++i) {
            State const& s = cur[i].s;
            if (s.terminal()) continue;
            add(step(w, s, false), i);
            add(step(w, s, true), i);
        }

        if (static_cast<int>(next.size()) > st.frontierCap) {
            S.pruned = true;
            std::vector<int> idx(next.size());
            for (size_t i = 0; i < idx.size(); ++i) idx[i] = static_cast<int>(i);
            std::sort(idx.begin(), idx.end(), [&](int a, int b) {
                auto const& A = next[a].s;
                auto const& B = next[b].s;
                if (A.terminal() != B.terminal()) return A.terminal();
                if (A.y != B.y) return A.y < B.y;
                return A.yv < B.yv;
            });
            std::vector<char> keep(next.size(), 0);
            double stride = static_cast<double>(idx.size()) / st.frontierCap;
            for (int j = 0; j < st.frontierCap; ++j) keep[idx[static_cast<size_t>(j * stride)]] = 1;
            for (auto i : idx) if (next[i].s.terminal()) keep[i] = 1;
            std::vector<int> remap(next.size(), -1);
            std::vector<Node> kept;
            for (size_t i = 0; i < next.size(); ++i) {
                if (keep[i]) {
                    remap[i] = static_cast<int>(kept.size());
                    kept.push_back(next[i]);
                }
            }
            std::vector<Edge> ex2;
            for (auto const& e : ex) {
                if (remap[e.child] >= 0) ex2.push_back(Edge{remap[e.child], e.parent});
            }
            next.swap(kept);
            ex.swap(ex2);
            anyAlive = std::any_of(next.begin(), next.end(), [](Node const& n) { return !n.s.terminal(); });
        }

        if (next.empty()) break;
        S.layers.push_back(std::move(next));
        S.extras.push_back(std::move(ex));
        if (!anyAlive) break;
    }

    S.keys.resize(S.layers.size());
    S.good.resize(S.layers.size());
    for (size_t t = 0; t < S.layers.size(); ++t) {
        auto& kv = S.keys[t];
        kv.reserve(S.layers[t].size());
        for (int i = 0; i < static_cast<int>(S.layers[t].size()); ++i) {
            kv.emplace_back(keyOf(S.layers[t][i].s), i);
        }
        std::sort(kv.begin(), kv.end(), [](auto const& a, auto const& b) { return a.first < b.first; });
        S.good[t].assign(S.layers[t].size(), 0);
        for (size_t i = 0; i < S.layers[t].size(); ++i) {
            if (S.layers[t][i].s.terminal()) S.good[t][i] = 1;
        }
    }
    for (int t = static_cast<int>(S.layers.size()) - 1; t > 0; --t) {
        for (size_t i = 0; i < S.layers[t].size(); ++i) {
            if (S.good[t][i]) S.good[t - 1][S.layers[t][i].parent] = 1;
        }
        for (auto const& e : S.extras[t]) {
            if (S.good[t][e.child]) S.good[t - 1][e.parent] = 1;
        }
    }
    S.goodCoarse.resize(S.layers.size());
    for (size_t t = 0; t < S.layers.size(); ++t) {
        auto& g = S.goodCoarse[t];
        for (size_t i = 0; i < S.layers[t].size(); ++i) {
            if (S.good[t][i]) g.push_back(coarseOf(keyOf(S.layers[t][i].s)));
        }
        std::sort(g.begin(), g.end());
        g.erase(std::unique(g.begin(), g.end()), g.end());
    }
}

std::string fmt(char const* f, double a) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), f, a);
    return buf;
}

// Welcher Orb wuerde bei einem Druck jetzt ausloesen? (nur fuer die Beschriftung)
int orbForLabel(World const& w, State const& s) {
    int found = -1;
    w.query(boxOf(s, halfSize(s)), [&](int i, LevelObj const& ob) {
        if (found < 0 && isOrb(ob.kind) && i != s.usedOrb) found = i;
    });
    return found;
}

std::string actionName(World const& w, State const& s) {
    if (s.holding) return s.dashing ? "Dash loslassen" : "Loslassen";
    int o = orbForLabel(w, s);
    if (o >= 0) return kindName(w.objs[o].kind);
    bool groundMode = s.mode == Mode::Cube || s.mode == Mode::Robot || s.mode == Mode::Ball ||
                      s.mode == Mode::Spider;
    if (groundMode && !s.canJump) return "Druecken (vorhalten fuer naechsten Orb/Landung)";
    switch (s.mode) {
        case Mode::Cube: return "Sprung";
        case Mode::Ship: return "Halten (Ship hoch)";
        case Mode::Ball: return "Ball-Wechsel";
        case Mode::Ufo: return "UFO-Klick";
        case Mode::Wave: return "Halten (Wave hoch)";
        case Mode::Robot: return "Robot-Sprung";
        case Mode::Spider: return "Spider-Teleport";
        case Mode::Swing: return "Swing-Wechsel";
    }
    return "Klick";
}

// ---------------------------------------------------------------------------
// Klicks + Fenster entlang eines Weges bestimmen
// "Idle" = Knopf so lassen wie er ist, "Aktion" = umschalten.
// ---------------------------------------------------------------------------
void extractClicks(World const& w, Search const& S, State start, Result& R, std::vector<char>& usedObjs) {
    State cur = start;
    int t = 0;
    int limit = static_cast<int>(S.layers.size()) + 4;
    auto markUsed = [&](State const& before, State const& after) {
        if (after.usedOrb != before.usedOrb && after.usedOrb >= 0) usedObjs[after.usedOrb] = 1;
        if (after.usedPad != before.usedPad && after.usedPad >= 0) usedObjs[after.usedPad] = 1;
    };

    R.path.push_back({cur.x, cur.y});
    while (!cur.terminal() && t < limit) {
        std::vector<State> idle{cur};
        struct Win { int a, b; };
        std::vector<Win> wins;
        int needAt = -1;
        bool reachedEnd = false;
        int u = t;
        for (;; ++u) {
            State const& s = idle.back();
            if (s.terminal()) { reachedEnd = true; break; }
            if (u - t > limit) break;
            bool pg = S.isGood(u + 1, step(w, s, true));
            if (pg) {
                if (!wins.empty() && wins.back().b == u - 1) wins.back().b = u;
                else wins.push_back({u, u});
            }
            State n = step(w, s, false);
            if (n.dead || !S.isGood(u + 1, n)) {
                needAt = u;
                break;
            }
            idle.push_back(n);
        }

        if (reachedEnd) {
            for (size_t i = 1; i < idle.size(); ++i) {
                markUsed(idle[i - 1], idle[i]);
                if (i % 2 == 0) R.path.push_back({idle[i].x, idle[i].y});
            }
            cur = idle.back();
            break;
        }
        if (needAt < 0 || wins.empty()) {
            R.warnings.push_back("Weg-Rekonstruktion ab x=" + fmt("%.0f", idle.back().x) +
                                 " ungenau (Rundung). Ergebnis trotzdem 'moeglich'.");
            break;
        }

        Win wsel = wins.back();
        int c = (wsel.a + wsel.b) / 2;
        State const& before = idle[c - t];
        Click k;
        k.tick = c;
        k.windowStart = wsel.a;
        k.windowEnd = wsel.b;
        k.x = before.x;
        k.y = before.y;
        k.release = before.holding;
        k.what = actionName(w, before);
        R.clicks.push_back(k);

        for (int i = 1; i <= c - t; ++i) {
            markUsed(idle[i - 1], idle[i]);
            if (i % 2 == 0) R.path.push_back({idle[i].x, idle[i].y});
        }
        State after = step(w, before, true);
        markUsed(before, after);
        cur = after;
        t = c + 1;
        R.path.push_back({cur.x, cur.y});
    }
    R.path.push_back({cur.x, cur.y});
}

State initialState(Start const& start, Physics const& phys) {
    State s0;
    s0.x = start.x;
    s0.y = start.y;
    s0.lastY = start.y;
    s0.speed = static_cast<int8_t>(std::clamp(start.speed, 0, 4));
    s0.flipped = start.gravityFlipped;
    s0.mode = start.mode;
    s0.mini = start.mini;
    if (s0.mode == Mode::Robot || s0.mode == Mode::Spider) s0.robotCharge = 1.5f;
    boundsAround(s0, start.y, s0.mode, phys.floorY);
    if (!s0.flipped && s0.y - halfSize(s0) <= s0.floorB + 0.01f) {
        s0.y = s0.floorB + halfSize(s0);
        hitGround(s0);
    }
    return s0;
}

Result analyzeOnce(std::vector<LevelObj> const& objects, Start const& start,
                   Physics const& phys, Settings const& st, float stopAtX) {
    Result R;
    World w(objects, phys);
    R.endX = w.endX;
    State s0 = initialState(start, phys);

    Search S;
    runSearch(w, s0, st, S, stopAtX);
    R.statesExplored = S.explored;
    R.reachedX = S.reachedX;
    R.possible = !S.good.empty() && S.good[0][0];
    if (S.pruned) {
        R.warnings.push_back("Sehr viele Moeglichkeiten - Suche wurde ausgeduennt. "
                             "Ein 'unmoeglich' kann hier falsch sein.");
    }

    if (!R.possible) {
        R.deathX = S.deathX;
        R.deathY = S.deathY;
        int best = S.furthestKiller;
        if (best >= 0) {
            R.killerUid = objects[best].uid;
            R.killerObjectId = objects[best].objectId;
            R.killerWhat = kindName(objects[best].kind);
        }
        return R;
    }
    if (stopAtX > 0) return R;

    std::vector<char> used(objects.size(), 0);
    extractClicks(w, S, s0, R, used);

    for (auto const& layer : S.layers) {
        for (auto const& n : layer) {
            if (n.s.stopped) R.stoppedAtUnsupported = true;
        }
    }

    float lastX = R.path.empty() ? 0.f : R.path.back().x;
    for (size_t i = 0; i < objects.size(); ++i) {
        auto const& ob = objects[i];
        if ((isOrb(ob.kind) || isPad(ob.kind)) && !used[i] && ob.x < lastX) {
            R.unusedObjects.push_back(std::string(kindName(ob.kind)) + " bei x=" + fmt("%.0f", ob.x) +
                                      " (uid " + std::to_string(ob.uid) + ")");
        }
    }
    return R;
}

} // namespace

// ---------------------------------------------------------------------------
char const* modeName(Mode m) {
    switch (m) {
        case Mode::Cube: return "Cube";
        case Mode::Ship: return "Ship";
        case Mode::Ball: return "Ball";
        case Mode::Ufo: return "UFO";
        case Mode::Wave: return "Wave";
        case Mode::Robot: return "Robot";
        case Mode::Spider: return "Spider";
        case Mode::Swing: return "Swing";
    }
    return "?";
}

char const* kindName(Kind k) {
    switch (k) {
        case Kind::Solid: return "Block";
        case Kind::Slope: return "Schraege";
        case Kind::Hazard: return "Stachel/Gefahr";
        case Kind::OrbYellow: return "gelber Orb";
        case Kind::OrbPink: return "pinker Orb";
        case Kind::OrbRed: return "roter Orb";
        case Kind::OrbBlue: return "blauer Orb";
        case Kind::OrbGreen: return "gruener Orb";
        case Kind::OrbBlack: return "schwarzer Orb";
        case Kind::OrbDash: return "gruener Dash-Orb";
        case Kind::OrbDashGravity: return "pinker Dash-Orb";
        case Kind::PadYellow: return "gelbes Pad";
        case Kind::PadPink: return "pinkes Pad";
        case Kind::PadRed: return "rotes Pad";
        case Kind::PadBlue: return "blaues Pad";
        case Kind::GravityNormal: return "Gravitations-Portal (normal)";
        case Kind::GravityFlip: return "Gravitations-Portal (umgedreht)";
        case Kind::GravityToggle: return "Gravitations-Wechsel-Portal";
        case Kind::Speed05: return "0.5x-Portal";
        case Kind::Speed1: return "1x-Portal";
        case Kind::Speed2: return "2x-Portal";
        case Kind::Speed3: return "3x-Portal";
        case Kind::Speed4: return "4x-Portal";
        case Kind::ModeCube: return "Cube-Portal";
        case Kind::ModeShip: return "Ship-Portal";
        case Kind::ModeBall: return "Ball-Portal";
        case Kind::ModeUfo: return "UFO-Portal";
        case Kind::ModeWave: return "Wave-Portal";
        case Kind::ModeRobot: return "Robot-Portal";
        case Kind::ModeSpider: return "Spider-Portal";
        case Kind::ModeSwing: return "Swing-Portal";
        case Kind::SizeMini: return "Mini-Portal";
        case Kind::SizeNormal: return "Normal-Groesse-Portal";
        case Kind::Stop: return "Dual-/Teleport-Portal";
        case Kind::Unsupported: return "nicht unterstuetztes Objekt";
    }
    return "?";
}

std::string clickLabel(Click const& c) {
    int w = c.windowTicks();
    char buf[96];
    if (w <= 1) std::snprintf(buf, sizeof(buf), "1 Tick - 240fps frame perfect");
    else if (w <= 4) std::snprintf(buf, sizeof(buf), "%d Ticks - 60fps frame perfect", w);
    else std::snprintf(buf, sizeof(buf), "%d Ticks = %.1f Frames@60", w, w / 4.0);
    return buf;
}

std::string summary(Result const& r) {
    std::string s;
    if (r.possible) {
        s += r.stoppedAtUnsupported ? "MOEGLICH bis zum ersten Dual-/Teleport-Portal.\n" : "MOEGLICH.\n";
        s += std::to_string(r.clicks.size()) + " Eingaben (Druecken/Loslassen).";
        int hardest = 1 << 30;
        Click const* hc = nullptr;
        for (auto const& c : r.clicks) {
            if (c.windowTicks() < hardest) { hardest = c.windowTicks(); hc = &c; }
        }
        if (hc) s += " Engste Stelle bei x=" + fmt("%.0f", hc->x) + " (" + hc->what + "): " + clickLabel(*hc) + ".";
        s += "\n";
        for (auto const& u : r.unusedObjects) s += "Nicht gebraucht (Skip?): " + u + "\n";
    }
    else {
        s += "UNMOEGLICH (laut Simulation).\n";
        s += "Weiteste Stelle: x=" + fmt("%.0f", r.deathX) + ", y=" + fmt("%.0f", r.deathY - 90.0) + ".";
        if (r.killerUid) {
            s += " Gestorben an: " + r.killerWhat + " (uid " + std::to_string(r.killerUid) + ", ID " +
                 std::to_string(r.killerObjectId) + ").";
        }
        s += "\n";
        bool any = false;
        for (auto const& n : r.nerfs) {
            if (!n.makesPossible) continue;
            if (!any) s += "So wird es schaffbar:\n";
            any = true;
            s += " - " + n.change + "\n";
        }
        if (!any) {
            for (auto const& n : r.nerfs) {
                s += " - Teilweise besser: " + n.change + " (kommt bis x=" + fmt("%.0f", n.reachedX) + ")\n";
            }
            if (r.nerfs.empty()) s += "Kein einfacher Nerf gefunden (Objekt verschieben probiert).\n";
        }
    }
    for (auto const& w : r.warnings) s += "Hinweis: " + w + "\n";
    return s;
}

Result analyze(std::vector<LevelObj> const& objects, Start const& start, Physics const& phys,
               Settings const& st) {
    Result R = analyzeOnce(objects, start, phys, st, -1.f);

    for (auto const& ob : objects) {
        if (ob.kind == Kind::Unsupported) {
            R.warnings.push_back("Objekt ID " + std::to_string(ob.objectId) + " bei x=" + fmt("%.0f", ob.x) +
                                 " wird ignoriert (Spider-Orb/-Pad, Custom-Orb ...).");
            break;
        }
    }

    if (R.possible || !st.suggestNerfs || R.killerUid == 0) return R;

    int killerIdx = -1;
    for (size_t i = 0; i < objects.size(); ++i) {
        if (objects[i].uid == R.killerUid) { killerIdx = static_cast<int>(i); break; }
    }
    if (killerIdx < 0) return R;

    struct Mod { float dx, dy, scale; bool remove; std::string text; };
    std::vector<Mod> mods;
    auto const& K = objects[killerIdx];
    auto blocks = [](float u) { return fmt("%.2f", u / 30.0) + " Bloecke"; };
    for (float d : {5.f, 10.f, 15.f, 30.f}) {
        mods.push_back({0, d, 1, false, fmt("%.0f", d) + " Einheiten (" + blocks(d) + ") nach oben"});
        mods.push_back({0, -d, 1, false, fmt("%.0f", d) + " Einheiten (" + blocks(d) + ") nach unten"});
        mods.push_back({d, 0, 1, false, fmt("%.0f", d) + " Einheiten (" + blocks(d) + ") nach rechts"});
        mods.push_back({-d, 0, 1, false, fmt("%.0f", d) + " Einheiten (" + blocks(d) + ") nach links"});
    }
    if (K.kind == Kind::Hazard) mods.push_back({0, 0, 0.7f, false, "auf 70 % verkleinern"});
    mods.push_back({0, 0, 1, true, "entfernen"});

    Settings fast = st;
    fast.suggestNerfs = false;
    fast.frontierCap = std::min(st.frontierCap, 6000);
    float stopAt = R.deathX + 450.f;

    Nerf bestPartial;
    bool havePartial = false;
    int found = 0;
    for (auto const& m : mods) {
        std::vector<LevelObj> copy = objects;
        if (m.remove) {
            copy.erase(copy.begin() + killerIdx);
        }
        else {
            auto& o = copy[killerIdx];
            float cx = (o.rect.x0 + o.rect.x1) / 2 + m.dx;
            float cy = (o.rect.y0 + o.rect.y1) / 2 + m.dy;
            float hw = (o.rect.x1 - o.rect.x0) / 2 * m.scale;
            float hh = (o.rect.y1 - o.rect.y0) / 2 * m.scale;
            o.rect = {cx - hw, cy - hh, cx + hw, cy + hh};
            o.x += m.dx;
            o.y += m.dy;
            o.sy0 += m.dy;
            o.sy1 += m.dy;
        }
        Result r2 = analyzeOnce(copy, start, phys, fast, stopAt);
        Nerf n;
        n.uid = K.uid;
        n.objectId = K.objectId;
        n.x = K.x;
        n.y = K.y;
        n.change = std::string(kindName(K.kind)) + " (uid " + std::to_string(K.uid) + ") bei x=" +
                   fmt("%.0f", K.x) + " " + m.text;
        n.reachedX = r2.possible ? stopAt : r2.reachedX;
        n.makesPossible = r2.possible;
        if (r2.possible) {
            R.nerfs.push_back(n);
            if (++found >= 3) break;
        }
        else if (r2.reachedX > R.reachedX + 30.f && (!havePartial || r2.reachedX > bestPartial.reachedX)) {
            bestPartial = n;
            havePartial = true;
        }
    }
    if (found == 0 && havePartial) R.nerfs.push_back(bestPartial);
    return R;
}

} // namespace bot
