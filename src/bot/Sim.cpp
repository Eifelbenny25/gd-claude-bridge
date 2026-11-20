#include "Sim.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace bot {

namespace {

constexpr float DT = 1.f / TPS;
constexpr float PI = 3.14159265f;

// ---------------------------------------------------------------------------
// Zustand des Spielers in einem Tick
// ---------------------------------------------------------------------------
struct State {
    float x = 0, y = 0, vy = 0;
    int8_t speed = 1;
    bool flipped = false;   // Gravitation umgedreht
    bool ground = false;
    bool dashing = false;
    bool dead = false;
    bool finished = false;  // Levelende erreicht
    bool stopped = false;   // nicht unterstuetzter Modus erreicht
    int16_t usedOrb = -1;
    int16_t usedPad = -1;
    int16_t usedPortal = -1;
    int16_t dashOrb = -1;
    int16_t killer = -1;    // Objekt, das getoetet / gestoppt hat
    bool terminal() const { return finished || stopped; }
};

struct Key {
    int32_t xq, yq, vq;
    uint32_t bits;
    int16_t usedOrb, usedPad, usedPortal, dashOrb;
    bool operator==(Key const& o) const { return std::memcmp(this, &o, sizeof(Key)) == 0; }
    bool operator<(Key const& o) const { return std::memcmp(this, &o, sizeof(Key)) < 0; }
};
static_assert(sizeof(Key) == 24, "Key ohne Padding");

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
    Key k{};
    std::memset(&k, 0, sizeof(k));
    k.xq = static_cast<int32_t>(std::lround(s.x * 4.f));
    k.yq = static_cast<int32_t>(std::lround(s.y * 2.f));
    k.vq = static_cast<int32_t>(std::lround(s.vy / 3.f));
    k.bits = static_cast<uint32_t>(s.speed) | (s.flipped << 3) | (s.ground << 4) |
             (s.dashing << 5) | (s.finished << 6) | (s.stopped << 7);
    k.usedOrb = s.usedOrb;
    k.usedPad = s.usedPad;
    k.usedPortal = s.usedPortal;
    k.dashOrb = s.dashOrb;
    return k;
}

bool isOrb(Kind k) {
    return k >= Kind::OrbYellow && k <= Kind::OrbDashGravity;
}
bool isPad(Kind k) {
    return k >= Kind::PadYellow && k <= Kind::PadBlue;
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
        float lo = r.x0 - maxWidth;
        auto it = std::lower_bound(order.begin(), order.end(), lo, [&](int i, float v) {
            return objs[i].rect.x0 < v;
        });
        for (; it != order.end(); ++it) {
            auto const& ob = objs[*it];
            if (ob.rect.x0 >= r.x1) break;
            if (ob.rect.overlaps(r)) f(*it, ob);
        }
    }
};

Rect playerRect(State const& s, float h) {
    return {s.x - h, s.y - h, s.x + h, s.y + h};
}

// Welcher Orb waere bei einem Klick jetzt aktiv? (-1 = keiner)
int orbAt(World const& w, State const& s) {
    int found = -1;
    w.query(playerRect(s, w.phys.half), [&](int i, LevelObj const& ob) {
        if (found < 0 && isOrb(ob.kind) && i != s.usedOrb) found = i;
    });
    return found;
}

bool actionPossible(World const& w, State const& s) {
    if (s.dead || s.terminal()) return false;
    if (s.dashing) return true;          // Loslassen
    if (orbAt(w, s) >= 0) return true;
    return s.ground;
}

std::string actionName(World const& w, State const& s) {
    if (s.dashing) return "Dash loslassen";
    int o = orbAt(w, s);
    if (o >= 0) return kindName(w.objs[o].kind);
    return "Sprung";
}

// Ein Physik-Tick. action = Klick (bzw. Loslassen beim Dash) in diesem Tick.
State step(World const& w, State s, bool action) {
    auto const& P = w.phys;
    float up = s.flipped ? -1.f : 1.f;

    if (action) {
        if (s.dashing) {
            s.dashing = false;
            s.dashOrb = -1;
        }
        else if (int o = orbAt(w, s); o >= 0) {
            s.usedOrb = static_cast<int16_t>(o);
            s.ground = false;
            switch (w.objs[o].kind) {
                case Kind::OrbYellow: s.vy = up * P.orbYellow; break;
                case Kind::OrbPink: s.vy = up * P.orbPink; break;
                case Kind::OrbRed: s.vy = up * P.orbRed; break;
                case Kind::OrbBlack: s.vy = -up * P.orbBlack; break;
                case Kind::OrbBlue:
                    s.flipped = !s.flipped;
                    up = -up;
                    s.vy = -up * P.orbBlue;
                    break;
                case Kind::OrbGreen:
                    s.flipped = !s.flipped;
                    up = -up;
                    s.vy = up * P.orbYellow;
                    break;
                case Kind::OrbDashGravity:
                    s.flipped = !s.flipped;
                    up = -up;
                    [[fallthrough]];
                case Kind::OrbDash:
                    s.dashing = true;
                    s.dashOrb = static_cast<int16_t>(o);
                    break;
                default: break;
            }
        }
        else if (s.ground) {
            s.vy = up * P.jump;
            s.ground = false;
        }
    }

    float vx = P.speeds[std::clamp<int>(s.speed, 0, 4)];
    if (s.dashing && s.dashOrb >= 0) {
        // GD: Rotation im Uhrzeigersinn, 0 = nach rechts
        float a = -w.objs[s.dashOrb].rotation * PI / 180.f;
        a = std::clamp(a, -70.f * PI / 180.f, 70.f * PI / 180.f);
        s.vy = vx * std::tan(a);
    }
    else {
        s.vy -= up * P.gravity * DT;
        if (s.vy * up < -P.maxFall) s.vy = -up * P.maxFall;
    }

    float prevY = s.y;
    s.x += vx * DT;
    s.y += s.vy * DT;
    s.ground = false;

    // Portale, Pads, Stacheln
    Rect pr = playerRect(s, P.half);
    w.query(pr, [&](int i, LevelObj const& ob) {
        if (s.dead || s.stopped) return;
        float u = s.flipped ? -1.f : 1.f;
        switch (ob.kind) {
            case Kind::Hazard:
                s.dead = true;
                s.killer = static_cast<int16_t>(i);
                break;
            case Kind::Speed05: s.speed = 0; break;
            case Kind::Speed1: s.speed = 1; break;
            case Kind::Speed2: s.speed = 2; break;
            case Kind::Speed3: s.speed = 3; break;
            case Kind::Speed4: s.speed = 4; break;
            case Kind::GravityNormal:
                if (s.flipped) { s.flipped = false; s.vy *= 0.5f; }
                break;
            case Kind::GravityFlip:
                if (!s.flipped) { s.flipped = true; s.vy *= 0.5f; }
                break;
            case Kind::GravityToggle:
                if (s.usedPortal != i) {
                    s.flipped = !s.flipped;
                    s.vy *= 0.5f;
                    s.usedPortal = static_cast<int16_t>(i);
                }
                break;
            case Kind::ModeOther:
            case Kind::SizeMini:
                s.stopped = true;
                s.killer = static_cast<int16_t>(i);
                break;
            default:
                if (isPad(ob.kind) && s.usedPad != i && !s.dashing) {
                    s.usedPad = static_cast<int16_t>(i);
                    switch (ob.kind) {
                        case Kind::PadYellow: s.vy = u * P.padYellow; break;
                        case Kind::PadPink: s.vy = u * P.padPink; break;
                        case Kind::PadRed: s.vy = u * P.padRed; break;
                        case Kind::PadBlue:
                            s.flipped = !s.flipped;
                            s.vy = u * P.padBlue; // Richtung neuer "Boden"
                            break;
                        default: break;
                    }
                }
                break;
        }
    });
    if (s.dead || s.stopped) return s;

    float up2 = s.flipped ? -1.f : 1.f;

    // Feste Bloecke: draufkommen oder seitlich/von unten reinfliegen
    w.query(playerRect(s, P.half), [&](int, LevelObj const& ob) {
        if (ob.kind != Kind::Solid) return;
        if (up2 > 0) {
            if (s.vy <= 0 && prevY - P.half >= ob.rect.y1 - P.landTolerance) {
                s.y = ob.rect.y1 + P.half;
                s.vy = 0;
                s.ground = true;
            }
        }
        else {
            if (s.vy >= 0 && prevY + P.half <= ob.rect.y0 + P.landTolerance) {
                s.y = ob.rect.y0 - P.half;
                s.vy = 0;
                s.ground = true;
            }
        }
    });
    // Schraegen (nur Boden oben, normale Gravitation): GD rechnet mit der
    // vorderen unteren Ecke des Spielers, darum Hoehe am hoeheren Ende der
    // Spieler-Unterkante nehmen.
    auto surfaceAt = [](LevelObj const& ob, float x) {
        float t = (x - ob.rect.x0) / std::max(1.f, ob.rect.x1 - ob.rect.x0);
        t = std::clamp(t, 0.f, 1.f);
        return ob.sy0 + (ob.sy1 - ob.sy0) * t;
    };
    float vxNow = P.speeds[std::clamp<int>(s.speed, 0, 4)];
    w.query(playerRect(s, P.half), [&](int, LevelObj const& ob) {
        if (ob.kind != Kind::Slope) return;
        float tanA = (ob.sy1 - ob.sy0) / std::max(1.f, ob.rect.x1 - ob.rect.x0);
        if (up2 < 0) {
            // umgedrehte Gravitation: Schraege wie Block behandeln
            if (s.vy >= 0 && prevY + P.half <= ob.rect.y0 + P.landTolerance) {
                s.y = ob.rect.y0 - P.half;
                s.vy = 0;
                s.ground = true;
            }
            return;
        }
        float surf = std::max(surfaceAt(ob, s.x - P.half), surfaceAt(ob, s.x + P.half));
        float tol = P.landTolerance + vxNow * DT * std::abs(tanA) + 1.f;
        float bottom = s.y - P.half;
        if (bottom >= surf - tol && s.vy <= vxNow * tanA + 1.f) {
            s.y = surf + P.half;
            s.vy = vxNow * tanA;   // nach oben: Schwung beim Verlassen der Schraege
            s.ground = true;
        }
    });

    w.query(playerRect(s, P.innerHalf), [&](int i, LevelObj const& ob) {
        if (s.dead) return;
        if (ob.kind == Kind::Slope) {
            if (up2 < 0) {
                s.dead = true;
                s.killer = static_cast<int16_t>(i);
                return;
            }
            float ib = s.y - P.innerHalf;
            float surf = std::max(surfaceAt(ob, s.x - P.innerHalf), surfaceAt(ob, s.x + P.innerHalf));
            if (ib < surf - 1.f) {
                s.dead = true;
                s.killer = static_cast<int16_t>(i);
            }
            return;
        }
        if (ob.kind == Kind::Solid) {
            s.dead = true;
            s.killer = static_cast<int16_t>(i);
        }
    });
    if (s.dead) return s;

    // Boden
    if (s.y - P.half <= P.floorY) {
        s.y = P.floorY + P.half;
        if (up2 > 0) {
            if (s.vy < 0) s.vy = 0;
            s.ground = true;
        }
        else if (s.vy < 0) {
            s.vy = 0;
        }
    }
    if (s.y > P.floorY + 3000.f) s.dead = true; // nach oben aus dem Level

    if (s.x >= w.endX) s.finished = true;
    return s;
}

// ---------------------------------------------------------------------------
// Suche (Breitensuche ueber alle Ticks, Zustaende werden zusammengelegt)
// ---------------------------------------------------------------------------
struct Node {
    State s;
    int parent;
    bool action;
};
struct Edge {
    int child, parent;
};

struct Search {
    std::vector<std::vector<Node>> layers;
    std::vector<std::vector<Edge>> extras;                 // pro Kind-Tick
    std::vector<std::vector<std::pair<Key, int>>> keys;    // sortiert, pro Tick
    std::vector<std::vector<char>> good;
    float reachedX = 0;
    float deathX = -1e9f, deathY = 0;
    std::unordered_map<int, int> killerCount;              // nahe der weitesten Todesstelle
    int furthestKiller = -1;
    bool pruned = false;
    long long explored = 0;

    bool isGood(int tick, State const& s) const {
        if (tick < 0 || tick >= static_cast<int>(keys.size())) return false;
        Key k = keyOf(s);
        auto const& v = keys[tick];
        auto it = std::lower_bound(v.begin(), v.end(), k, [](auto const& a, Key const& b) {
            return a.first < b;
        });
        if (it == v.end() || !(it->first == k)) return false;
        return good[tick][it->second] != 0;
    }
};

void recordDeath(Search& S, State const& d) {
    if (d.x > S.deathX + 45.f) {
        S.deathX = d.x;
        S.deathY = d.y;
        S.furthestKiller = d.killer;
        S.killerCount.clear();
    }
    if (d.x >= S.deathX - 45.f && d.killer >= 0) S.killerCount[d.killer]++;
    if (d.x > S.deathX) {
        S.deathX = d.x;
        S.deathY = d.y;
        S.furthestKiller = d.killer;
    }
}

void runSearch(World const& w, State const& start, Settings const& st, Search& S,
               float stopAtX) {
    S.layers.push_back({Node{start, -1, false}});
    S.extras.push_back({});
    int maxTicks = st.maxSeconds * TPS;

    for (int t = 0; t < maxTicks; ++t) {
        auto const& cur = S.layers[t];
        std::vector<Node> next;
        std::vector<Edge> ex;
        std::unordered_map<Key, int, KeyHash> map;
        map.reserve(cur.size() * 2 + 4);
        bool anyAlive = false;

        auto add = [&](State const& c, int parent, bool act) {
            ++S.explored;
            if (c.dead) {
                recordDeath(S, c);
                return;
            }
            State cc = c;
            if (stopAtX > 0 && cc.x >= stopAtX) cc.finished = true;
            S.reachedX = std::max(S.reachedX, cc.x);
            Key k = keyOf(cc);
            auto [it, inserted] = map.try_emplace(k, static_cast<int>(next.size()));
            if (inserted) {
                next.push_back(Node{cc, parent, act});
                if (!cc.terminal()) anyAlive = true;
            }
            else {
                ex.push_back(Edge{it->second, parent});
            }
        };

        for (int i = 0; i < static_cast<int>(cur.size()); ++i) {
            State const& s = cur[i].s;
            if (s.terminal()) continue;
            add(step(w, s, false), i, false);
            if (actionPossible(w, s)) add(step(w, s, true), i, true);
        }

        // Zu viele Zustaende: gleichmaessig ausduennen (Endzustaende behalten)
        if (static_cast<int>(next.size()) > st.frontierCap) {
            S.pruned = true;
            std::vector<int> idx(next.size());
            for (size_t i = 0; i < idx.size(); ++i) idx[i] = static_cast<int>(i);
            std::sort(idx.begin(), idx.end(), [&](int a, int b) {
                auto const& A = next[a].s;
                auto const& B = next[b].s;
                if (A.terminal() != B.terminal()) return A.terminal();
                if (A.y != B.y) return A.y < B.y;
                return A.vy < B.vy;
            });
            std::vector<char> keep(next.size(), 0);
            double stride = static_cast<double>(idx.size()) / st.frontierCap;
            for (int j = 0; j < st.frontierCap; ++j) {
                keep[idx[static_cast<size_t>(j * stride)]] = 1;
            }
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
            anyAlive = std::any_of(next.begin(), next.end(),
                                   [](Node const& n) { return !n.s.terminal(); });
        }

        if (next.empty()) break;
        S.layers.push_back(std::move(next));
        S.extras.push_back(std::move(ex));
        if (!anyAlive) break;
    }

    // Schluessel-Tabellen
    S.keys.resize(S.layers.size());
    S.good.resize(S.layers.size());
    for (size_t t = 0; t < S.layers.size(); ++t) {
        auto& kv = S.keys[t];
        kv.reserve(S.layers[t].size());
        for (int i = 0; i < static_cast<int>(S.layers[t].size()); ++i) {
            kv.emplace_back(keyOf(S.layers[t][i].s), i);
        }
        std::sort(kv.begin(), kv.end(), [](auto const& a, auto const& b) {
            return a.first < b.first;
        });
        S.good[t].assign(S.layers[t].size(), 0);
        for (size_t i = 0; i < S.layers[t].size(); ++i) {
            if (S.layers[t][i].s.terminal()) S.good[t][i] = 1;
        }
    }
    // "Gut" = von hier aus ist das Ende erreichbar. Rueckwaerts verbreiten.
    for (int t = static_cast<int>(S.layers.size()) - 1; t > 0; --t) {
        for (size_t i = 0; i < S.layers[t].size(); ++i) {
            if (S.good[t][i]) S.good[t - 1][S.layers[t][i].parent] = 1;
        }
        for (auto const& e : S.extras[t]) {
            if (S.good[t][e.child]) S.good[t - 1][e.parent] = 1;
        }
    }
}

std::string fmt(char const* f, double a) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), f, a);
    return buf;
}

// ---------------------------------------------------------------------------
// Klicks + Fenster entlang eines Weges bestimmen
// ---------------------------------------------------------------------------
void extractClicks(World const& w, Search const& S, State start, Result& R,
                   std::vector<char>& usedObjs) {
    State cur = start;
    int t = 0;
    int limit = static_cast<int>(S.layers.size()) + 4;
    auto markUsed = [&](State const& before, State const& after) {
        if (after.usedOrb != before.usedOrb && after.usedOrb >= 0) usedObjs[after.usedOrb] = 1;
        if (after.usedPad != before.usedPad && after.usedPad >= 0) usedObjs[after.usedPad] = 1;
    };

    R.path.push_back({cur.x, cur.y});
    while (!cur.terminal() && t < limit) {
        // Ohne Klick weiterlaufen und dabei Klick-Fenster sammeln
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
            bool pg = false;
            if (actionPossible(w, s)) pg = S.isGood(u + 1, step(w, s, true));
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
        k.release = before.dashing;
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

Result analyzeOnce(std::vector<LevelObj> const& objects, Start const& start,
                   Physics const& phys, Settings const& st, float stopAtX) {
    Result R;
    World w(objects, phys);
    R.endX = w.endX;

    State s0;
    s0.x = start.x;
    s0.y = start.y;
    s0.speed = static_cast<int8_t>(start.speed);
    s0.flipped = start.gravityFlipped;
    s0.ground = !start.gravityFlipped && start.y - phys.half <= phys.floorY + 0.01f;

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
        // Das Objekt an der weitesten Todesstelle ist das eigentliche Hindernis
        int best = S.furthestKiller;
        if (best < 0) {
            int bestN = 0;
            for (auto [k, n] : S.killerCount) {
                if (n > bestN) { best = k; bestN = n; }
            }
        }
        if (best >= 0) {
            R.killerUid = objects[best].uid;
            R.killerObjectId = objects[best].objectId;
            R.killerWhat = kindName(objects[best].kind);
        }
        return R;
    }
    if (stopAtX > 0) return R; // Nerf-Test: nur "kommt vorbei?" zaehlt

    std::vector<char> used(objects.size(), 0);
    extractClicks(w, S, s0, R, used);

    // Endet der Weg an einem nicht unterstuetzten Portal?
    for (auto const& layer : S.layers) {
        for (auto const& n : layer) {
            if (n.s.stopped && n.s.killer >= 0) {
                R.stoppedAtUnsupported = true;
            }
        }
    }

    float lastX = R.path.empty() ? 0.f : R.path.back().x;
    for (size_t i = 0; i < objects.size(); ++i) {
        auto const& ob = objects[i];
        if ((isOrb(ob.kind) || isPad(ob.kind)) && !used[i] && ob.x < lastX) {
            R.unusedObjects.push_back(std::string(kindName(ob.kind)) + " bei x=" +
                                      fmt("%.0f", ob.x) + " (uid " +
                                      std::to_string(ob.uid) + ")");
        }
    }
    return R;
}

} // namespace

// ---------------------------------------------------------------------------
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
        case Kind::ModeOther: return "Spielmodus-Portal";
        case Kind::SizeMini: return "Mini-Portal";
        case Kind::SizeNormal: return "Normal-Groesse-Portal";
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
        s += r.stoppedAtUnsupported
                 ? "MOEGLICH bis zum ersten nicht unterstuetzten Portal.\n"
                 : "MOEGLICH.\n";
        s += std::to_string(r.clicks.size()) + " Klicks.";
        int hardest = 1 << 30;
        Click const* hc = nullptr;
        for (auto const& c : r.clicks) {
            if (c.windowTicks() < hardest) { hardest = c.windowTicks(); hc = &c; }
        }
        if (hc) {
            s += " Engster Klick bei x=" + fmt("%.0f", hc->x) + ": " + clickLabel(*hc) + ".";
        }
        s += "\n";
        for (auto const& u : r.unusedObjects) s += "Nicht gebraucht (Skip?): " + u + "\n";
    }
    else {
        s += "UNMOEGLICH (mit aktueller Physik-Annahme).\n";
        s += "Weiteste Stelle: x=" + fmt("%.0f", r.deathX) + ", y=" + fmt("%.0f", r.deathY) + ".";
        if (r.killerUid) {
            s += " Meistens gestorben an: " + r.killerWhat + " (uid " +
                 std::to_string(r.killerUid) + ", ID " + std::to_string(r.killerObjectId) + ").";
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
                s += " - Teilweise besser: " + n.change + " (kommt bis x=" +
                     fmt("%.0f", n.reachedX) + ")\n";
            }
            if (r.nerfs.empty()) s += "Kein einfacher Nerf gefunden (Objekte verschieben probiert).\n";
        }
    }
    for (auto const& w : r.warnings) s += "Hinweis: " + w + "\n";
    return s;
}

Result analyze(std::vector<LevelObj> const& objects, Start const& start,
               Physics const& phys, Settings const& st) {
    Result R = analyzeOnce(objects, start, phys, st, -1.f);

    for (auto const& ob : objects) {
        if (ob.kind == Kind::Unsupported) {
            R.warnings.push_back("Objekt ID " + std::to_string(ob.objectId) + " bei x=" +
                                 fmt("%.0f", ob.x) + " wird nicht simuliert.");
            break;
        }
    }

    if (R.possible || !st.suggestNerfs || R.killerUid == 0) return R;

    // Nerf-Vorschlaege: Killer-Objekt verschieben / verkleinern / entfernen
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
    fast.frontierCap = std::min(st.frontierCap, 3000);
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
        }
        Result r2 = analyzeOnce(copy, start, phys, fast, stopAt);
        Nerf n;
        n.uid = K.uid;
        n.objectId = K.objectId;
        n.x = K.x;
        n.y = K.y;
        n.change = std::string(kindName(K.kind)) + " (uid " + std::to_string(K.uid) +
                   ") bei x=" + fmt("%.0f", K.x) + " " + m.text;
        n.reachedX = r2.possible ? stopAt : r2.reachedX;
        n.makesPossible = r2.possible;
        if (r2.possible) {
            R.nerfs.push_back(n);
            if (++found >= 3) break;
        }
        else if (r2.reachedX > R.reachedX + 30.f &&
                 (!havePartial || r2.reachedX > bestPartial.reachedX)) {
            bestPartial = n;
            havePartial = true;
        }
    }
    if (found == 0 && havePartial) R.nerfs.push_back(bestPartial);
    return R;
}

} // namespace bot
