#include "Commands.hpp"
#include "bot/EditorBot.hpp"

#include <Geode/Geode.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <unordered_map>
#include <vector>

using namespace geode::prelude;

namespace {

// ---------------------------------------------------------------------------
// GD speichert jedes Objekt als Text: "1,8,2,45,3,105,6,90,..."
// (Schluessel,Wert,Schluessel,Wert,...). Wir lesen und schreiben Objekte ueber
// genau dieses Format. Das ist robust, weil GD es selbst zum Speichern nutzt.
//
// Wichtige Schluessel:
//   1 = Objekt-ID      2 = X            3 = Y           4 = FlipX   5 = FlipY
//   6 = Rotation      20 = Editor-Layer 21 = Farbe 1 (Main)  22 = Farbe 2 (Detail)
//  24 = Z-Layer       25 = Z-Order     57 = Gruppen ("1.2.3")
// 128 = Skalierung X 129 = Skalierung Y  32 = alte einheitliche Skalierung
// ---------------------------------------------------------------------------
using Props = std::vector<std::pair<std::string, std::string>>;

Props parseObjectString(std::string s) {
    while (!s.empty() && (s.back() == ';' || s.back() == ' ')) s.pop_back();
    std::vector<std::string> parts;
    std::string current;
    for (char c : s) {
        if (c == ',') {
            parts.push_back(current);
            current.clear();
        }
        else {
            current += c;
        }
    }
    parts.push_back(current);

    Props out;
    for (size_t i = 0; i + 1 < parts.size(); i += 2) {
        out.emplace_back(parts[i], parts[i + 1]);
    }
    return out;
}

std::string buildObjectString(Props const& props) {
    std::string out;
    for (auto const& [key, value] : props) {
        if (!out.empty()) out += ',';
        out += key;
        out += ',';
        out += value;
    }
    return out;
}

std::optional<std::string> getProp(Props const& props, std::string const& key) {
    for (auto const& [k, v] : props) {
        if (k == key) return v;
    }
    return std::nullopt;
}

void setProp(Props& props, std::string const& key, std::string const& value) {
    for (auto& [k, v] : props) {
        if (k == key) {
            v = value;
            return;
        }
    }
    props.emplace_back(key, value);
}

void eraseProp(Props& props, std::string const& key) {
    std::erase_if(props, [&](auto const& p) { return p.first == key; });
}

double toDouble(std::optional<std::string> const& s, double fallback) {
    if (!s || s->empty()) return fallback;
    return std::strtod(s->c_str(), nullptr);
}

// Zahl -> Text, immer mit Punkt als Dezimaltrenner
std::string fmtNum(double d) {
    if (std::isfinite(d) && std::floor(d) == d && std::abs(d) < 1e15) {
        return std::to_string(static_cast<long long>(d));
    }
    char buf[64];
    auto res = std::to_chars(buf, buf + sizeof(buf), d);
    return std::string(buf, res.ptr);
}

std::vector<int> parseGroups(std::optional<std::string> const& s) {
    std::vector<int> out;
    if (!s) return out;
    std::string current;
    for (char c : *s + ".") {
        if (c == '.') {
            if (!current.empty()) out.push_back(std::atoi(current.c_str()));
            current.clear();
        }
        else {
            current += c;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Hilfsfunktionen fuer JSON-Parameter
// ---------------------------------------------------------------------------
bool has(matjson::Value const& obj, std::string_view key) {
    return obj.isObject() && obj.contains(key);
}

std::optional<double> optNum(matjson::Value const& obj, std::string_view key) {
    if (!has(obj, key)) return std::nullopt;
    auto r = obj[key].asDouble();
    if (r.isErr()) return std::nullopt;
    return r.unwrap();
}

std::optional<bool> optBool(matjson::Value const& obj, std::string_view key) {
    if (!has(obj, key)) return std::nullopt;
    auto r = obj[key].asBool();
    if (r.isErr()) return std::nullopt;
    return r.unwrap();
}

std::optional<std::string> optString(matjson::Value const& obj, std::string_view key) {
    if (!has(obj, key)) return std::nullopt;
    auto r = obj[key].asString();
    if (r.isErr()) return std::nullopt;
    return r.unwrap();
}

std::vector<matjson::Value> optArray(matjson::Value const& obj, std::string_view key) {
    if (!has(obj, key)) return {};
    matjson::Value copy = obj[key];
    auto r = copy.asArray();
    if (r.isErr()) return {};
    std::vector<matjson::Value> out = r.unwrap();
    return out;
}

std::vector<int> optIntList(matjson::Value const& obj, std::string_view key) {
    std::vector<int> out;
    for (auto const& v : optArray(obj, key)) {
        auto r = v.asDouble();
        if (r.isOk()) out.push_back(static_cast<int>(r.unwrap()));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Editor-Hilfsfunktionen
// ---------------------------------------------------------------------------
matjson::Value objectToJson(GameObject* obj, LevelEditorLayer* editor) {
    std::string save = obj->getSaveString(editor);
    auto props = parseObjectString(save);

    double legacyScale = toDouble(getProp(props, "32"), 1.0);
    std::vector<matjson::Value> groups;
    for (int g : parseGroups(getProp(props, "57"))) groups.push_back(g);

    return matjson::makeObject({
        {"uid", obj->m_uniqueID},
        {"id", obj->m_objectID},
        {"x", toDouble(getProp(props, "2"), obj->getPositionX())},
        {"y", toDouble(getProp(props, "3"), obj->getPositionY())},
        {"rotation", toDouble(getProp(props, "6"), 0.0)},
        {"scaleX", toDouble(getProp(props, "128"), legacyScale)},
        {"scaleY", toDouble(getProp(props, "129"), legacyScale)},
        {"flipX", toDouble(getProp(props, "4"), 0.0) != 0.0},
        {"flipY", toDouble(getProp(props, "5"), 0.0) != 0.0},
        {"color1", static_cast<int>(toDouble(getProp(props, "21"), 0.0))},
        {"color2", static_cast<int>(toDouble(getProp(props, "22"), 0.0))},
        {"groups", matjson::Value(groups)},
        {"editorLayer", static_cast<int>(toDouble(getProp(props, "20"), 0.0))},
        {"zOrder", static_cast<int>(toDouble(getProp(props, "25"), 0.0))},
        {"raw", save},
    });
}

std::unordered_map<int, GameObject*> objectsByUid(LevelEditorLayer* editor) {
    std::unordered_map<int, GameObject*> map;
    if (!editor->m_objects) return map;
    for (auto obj : geode::cocos::CCArrayExt<GameObject*>(editor->m_objects)) {
        map[obj->m_uniqueID] = obj;
    }
    return map;
}

// Loescht ueber "Auswaehlen + Entfernen-Knopf". So legt GD selbst einen
// Undo-Schritt an (Strg+Z holt die Objekte zurueck).
void deleteWithUndo(LevelEditorLayer* editor, std::vector<GameObject*> const& objects) {
    if (objects.empty()) return;
    auto ui = editor->m_editorUI;
    auto arr = CCArray::create();
    for (auto obj : objects) arr->addObject(obj);
    ui->deselectAll();
    ui->selectObjects(arr, true);
    ui->onDeleteSelected(nullptr);
    ui->deselectAll();
}

// Erzeugt Objekte aus einem GD-Objekt-String ("obj1;obj2;...").
// noUndo = false -> GD legt einen Undo-Eintrag an.
std::vector<int> createWithUndo(LevelEditorLayer* editor, std::string const& objectString) {
    std::vector<int> uids;
    if (objectString.empty()) return uids;
    auto arr = editor->createObjectsFromString(gd::string(objectString), false, true);
    if (!arr) return uids;
    for (auto obj : geode::cocos::CCArrayExt<GameObject*>(arr)) {
        uids.push_back(obj->m_uniqueID);
    }
    return uids;
}

// Wendet "set"/"move"/"extra" aus den Parametern auf einen Objekt-String an.
void applyChanges(Props& props, matjson::Value const& set, double dx, double dy, std::string const& extra) {
    if (auto v = optNum(set, "id")) setProp(props, "1", fmtNum(*v));
    if (auto v = optNum(set, "x")) setProp(props, "2", fmtNum(*v));
    if (auto v = optNum(set, "y")) setProp(props, "3", fmtNum(*v));
    if (auto v = optBool(set, "flipX")) setProp(props, "4", *v ? "1" : "0");
    if (auto v = optBool(set, "flipY")) setProp(props, "5", *v ? "1" : "0");
    if (auto v = optNum(set, "rotation")) {
        setProp(props, "6", fmtNum(*v));
        eraseProp(props, "131"); // getrennte X/Y-Rotation entfernen
        eraseProp(props, "132");
    }
    auto sx = optNum(set, "scaleX");
    auto sy = optNum(set, "scaleY");
    if (auto s = optNum(set, "scale")) {
        sx = s;
        sy = s;
    }
    if (sx || sy) {
        double legacy = toDouble(getProp(props, "32"), 1.0);
        if (!sx) sx = toDouble(getProp(props, "128"), legacy);
        if (!sy) sy = toDouble(getProp(props, "129"), legacy);
        eraseProp(props, "32");
        setProp(props, "128", fmtNum(*sx));
        setProp(props, "129", fmtNum(*sy));
    }
    if (auto v = optNum(set, "color1")) setProp(props, "21", fmtNum(*v));
    if (auto v = optNum(set, "color2")) setProp(props, "22", fmtNum(*v));
    if (auto v = optNum(set, "editorLayer")) setProp(props, "20", fmtNum(*v));
    if (auto v = optNum(set, "zLayer")) setProp(props, "24", fmtNum(*v));
    if (auto v = optNum(set, "zOrder")) setProp(props, "25", fmtNum(*v));
    if (has(set, "groups")) {
        auto groups = optIntList(set, "groups");
        if (groups.empty()) {
            eraseProp(props, "57");
        }
        else {
            std::string g;
            for (int id : groups) {
                if (!g.empty()) g += '.';
                g += std::to_string(id);
            }
            setProp(props, "57", g);
        }
    }

    if (dx != 0.0) setProp(props, "2", fmtNum(toDouble(getProp(props, "2"), 0.0) + dx));
    if (dy != 0.0) setProp(props, "3", fmtNum(toDouble(getProp(props, "3"), 0.0) + dy));

    // "extra": beliebige GD-Schluessel als "schluessel,wert,schluessel,wert"
    if (!extra.empty()) {
        for (auto const& [k, v] : parseObjectString(extra)) setProp(props, k, v);
    }
}

GJEffectManager* settingsEffectManager(LevelEditorLayer* editor) {
    if (editor->m_levelSettings && editor->m_levelSettings->m_effectManager) {
        return editor->m_levelSettings->m_effectManager;
    }
    return editor->m_effectManager;
}

matjson::Value colorToJson(int channel, ColorAction* a) {
    return matjson::makeObject({
        {"channel", channel},
        {"r", static_cast<int>(a->m_fromColor.r)},
        {"g", static_cast<int>(a->m_fromColor.g)},
        {"b", static_cast<int>(a->m_fromColor.b)},
        {"opacity", static_cast<double>(a->m_fromOpacity)},
        {"blending", a->m_blending},
        {"playerColor", a->m_playerColor},
        {"copyChannel", a->m_copyID},
    });
}

// ---------------------------------------------------------------------------
// Die eigentlichen Befehle
// ---------------------------------------------------------------------------
using Handler = Result<matjson::Value> (*)(LevelEditorLayer*, matjson::Value const&);

Result<matjson::Value> cmdGetLevelInfo(LevelEditorLayer* editor, matjson::Value const&) {
    std::string name;
    int lengthCategory = -1;
    if (editor->m_level) {
        name = editor->m_level->m_levelName;
        lengthCategory = editor->m_level->m_levelLength;
    }

    int count = 0;
    double minX = 0, maxX = 0, minY = 0, maxY = 0;
    bool first = true;
    if (editor->m_objects) {
        for (auto obj : geode::cocos::CCArrayExt<GameObject*>(editor->m_objects)) {
            double x = obj->getPositionX();
            double y = obj->getPositionY();
            if (first) {
                minX = maxX = x;
                minY = maxY = y;
                first = false;
            }
            minX = (std::min)(minX, x);
            maxX = (std::max)(maxX, x);
            minY = (std::min)(minY, y);
            maxY = (std::max)(maxY, y);
            count++;
        }
    }

    int selected = 0;
    if (auto sel = editor->m_editorUI->getSelectedObjects()) selected = static_cast<int>(sel->count());

    return Ok(matjson::makeObject({
        {"name", name},
        {"objectCount", count},
        {"selectedCount", selected},
        {"minX", minX},
        {"maxX", maxX},
        {"minY", minY},
        {"maxY", maxY},
        {"lengthInBlocks", maxX / 30.0},
        {"lengthCategory", lengthCategory},
    }));
}

Result<matjson::Value> cmdListObjects(LevelEditorLayer* editor, matjson::Value const& p) {
    double x1 = optNum(p, "x1").value_or(-1e9);
    double x2 = optNum(p, "x2").value_or(1e9);
    double y1 = optNum(p, "y1").value_or(-1e9);
    double y2 = optNum(p, "y2").value_or(1e9);
    auto idFilter = optNum(p, "objectId");
    auto groupFilter = optNum(p, "group");
    int limit = static_cast<int>(optNum(p, "limit").value_or(300));
    int offset = static_cast<int>(optNum(p, "offset").value_or(0));
    limit = (std::max)(1, (std::min)(limit, 5000));

    std::vector<matjson::Value> list;
    int matched = 0;
    if (editor->m_objects) {
        for (auto obj : geode::cocos::CCArrayExt<GameObject*>(editor->m_objects)) {
            double x = obj->getPositionX();
            double y = obj->getPositionY();
            if (x < x1 || x > x2 || y < y1 || y > y2) continue;
            if (idFilter && obj->m_objectID != static_cast<int>(*idFilter)) continue;
            if (groupFilter) {
                auto props = parseObjectString(obj->getSaveString(editor));
                auto groups = parseGroups(getProp(props, "57"));
                if (std::find(groups.begin(), groups.end(), static_cast<int>(*groupFilter)) == groups.end()) continue;
            }
            if (matched >= offset && static_cast<int>(list.size()) < limit) {
                list.push_back(objectToJson(obj, editor));
            }
            matched++;
        }
    }

    return Ok(matjson::makeObject({
        {"totalMatches", matched},
        {"returned", static_cast<int>(list.size())},
        {"offset", offset},
        {"objects", matjson::Value(list)},
    }));
}

Result<matjson::Value> cmdGetSelected(LevelEditorLayer* editor, matjson::Value const&) {
    std::vector<matjson::Value> list;
    if (auto sel = editor->m_editorUI->getSelectedObjects()) {
        for (auto obj : geode::cocos::CCArrayExt<GameObject*>(sel)) {
            list.push_back(objectToJson(obj, editor));
        }
    }
    return Ok(matjson::makeObject({
        {"count", static_cast<int>(list.size())},
        {"objects", matjson::Value(list)},
    }));
}

Result<matjson::Value> cmdAddObjects(LevelEditorLayer* editor, matjson::Value const& p) {
    std::string objectString;

    if (auto raw = optString(p, "objectString")) objectString = *raw;

    auto objects = optArray(p, "objects");
    if (objects.size() > 5000) return Err("Maximal 5000 Objekte pro Aufruf");
    for (auto const& o : objects) {
        auto id = optNum(o, "id");
        if (!id) return Err("Jedes Objekt braucht eine 'id' (Objekt-ID, z. B. 1 = Block)");
        Props props;
        setProp(props, "1", fmtNum(*id));
        setProp(props, "2", fmtNum(optNum(o, "x").value_or(15)));
        setProp(props, "3", fmtNum(optNum(o, "y").value_or(105)));
        applyChanges(props, o, 0, 0, optString(o, "extra").value_or(""));
        if (!objectString.empty() && objectString.back() != ';') objectString += ';';
        objectString += buildObjectString(props);
    }

    if (objectString.empty()) return Err("Keine Objekte angegeben");

    auto uids = createWithUndo(editor, objectString);
    std::vector<matjson::Value> uidValues(uids.begin(), uids.end());
    return Ok(matjson::makeObject({
        {"created", static_cast<int>(uids.size())},
        {"uids", matjson::Value(uidValues)},
    }));
}

Result<matjson::Value> cmdDeleteObjects(LevelEditorLayer* editor, matjson::Value const& p) {
    auto uids = optIntList(p, "uids");
    if (uids.empty()) return Err("'uids' fehlt oder ist leer");

    auto map = objectsByUid(editor);
    std::vector<GameObject*> targets;
    std::vector<matjson::Value> missing;
    for (int uid : uids) {
        auto it = map.find(uid);
        if (it != map.end()) targets.push_back(it->second);
        else missing.push_back(uid);
    }
    deleteWithUndo(editor, targets);

    return Ok(matjson::makeObject({
        {"deleted", static_cast<int>(targets.size())},
        {"notFound", matjson::Value(missing)},
    }));
}

// Aendern = alte Objekte loeschen + geaenderte Kopien erstellen.
// Das ergibt ZWEI Undo-Schritte (Strg+Z zweimal = alles wie vorher).
Result<matjson::Value> cmdModifyObjects(LevelEditorLayer* editor, matjson::Value const& p) {
    auto uids = optIntList(p, "uids");
    if (uids.empty()) return Err("'uids' fehlt oder ist leer");

    matjson::Value set = has(p, "set") ? p["set"] : matjson::Value(nullptr);
    double dx = optNum(p, "dx").value_or(0);
    double dy = optNum(p, "dy").value_or(0);
    std::string extra = optString(p, "extra").value_or("");

    auto map = objectsByUid(editor);
    std::vector<GameObject*> targets;
    std::vector<matjson::Value> oldUids;
    std::vector<matjson::Value> missing;
    std::string newString;

    for (int uid : uids) {
        auto it = map.find(uid);
        if (it == map.end()) {
            missing.push_back(uid);
            continue;
        }
        auto props = parseObjectString(it->second->getSaveString(editor));
        applyChanges(props, set, dx, dy, extra);
        if (!newString.empty()) newString += ';';
        newString += buildObjectString(props);
        targets.push_back(it->second);
        oldUids.push_back(uid);
    }

    if (targets.empty()) return Err("Keines der Objekte wurde gefunden");

    deleteWithUndo(editor, targets);
    auto newUids = createWithUndo(editor, newString);
    std::vector<matjson::Value> newUidValues(newUids.begin(), newUids.end());

    return Ok(matjson::makeObject({
        {"modified", static_cast<int>(targets.size())},
        {"oldUids", matjson::Value(oldUids)},
        {"newUids", matjson::Value(newUidValues)},
        {"notFound", matjson::Value(missing)},
        {"note", "Objekte wurden neu erstellt und haben neue uids. Rueckgaengig: 2x Strg+Z."},
    }));
}

Result<matjson::Value> cmdGetColors(LevelEditorLayer* editor, matjson::Value const& p) {
    auto em = settingsEffectManager(editor);
    if (!em) return Err("Kein Effekt-Manager gefunden");

    auto channels = optIntList(p, "channels");
    if (channels.empty()) {
        // BG, Ground, Line, 3DL, Obj, Ground2, Middleground, Middleground2, dazu 1-20
        channels = {1000, 1001, 1002, 1003, 1004, 1009, 1013, 1014};
        for (int i = 1; i <= 20; i++) channels.push_back(i);
    }

    std::vector<matjson::Value> list;
    for (int ch : channels) {
        if (ch < 1 || ch > 1100) continue;
        if (auto action = em->getColorAction(ch)) list.push_back(colorToJson(ch, action));
    }
    return Ok(matjson::makeObject({{"channels", matjson::Value(list)}}));
}

Result<matjson::Value> cmdSetColor(LevelEditorLayer* editor, matjson::Value const& p) {
    auto channelOpt = optNum(p, "channel");
    if (!channelOpt) return Err("'channel' fehlt");
    int channel = static_cast<int>(*channelOpt);
    if (channel < 1 || channel > 1100) return Err("Ungueltiger Farbkanal");

    auto em = settingsEffectManager(editor);
    if (!em) return Err("Kein Effekt-Manager gefunden");
    auto action = em->getColorAction(channel);
    if (!action) return Err("Farbkanal nicht gefunden");

    auto before = colorToJson(channel, action);

    auto clampByte = [](double v) { return static_cast<GLubyte>((std::max)(0.0, (std::min)(255.0, v))); };
    ccColor3B color = action->m_fromColor;
    if (auto v = optNum(p, "r")) color.r = clampByte(*v);
    if (auto v = optNum(p, "g")) color.g = clampByte(*v);
    if (auto v = optNum(p, "b")) color.b = clampByte(*v);
    float opacity = static_cast<float>(optNum(p, "opacity").value_or(action->m_fromOpacity));
    opacity = (std::max)(0.f, (std::min)(1.f, opacity));
    bool blending = optBool(p, "blending").value_or(action->m_blending);

    auto apply = [&](GJEffectManager* manager) {
        if (!manager) return;
        auto a = manager->getColorAction(channel);
        if (!a) return;
        a->m_color = color;
        a->m_fromColor = color;
        a->m_toColor = color;
        a->m_currentOpacity = opacity;
        a->m_fromOpacity = opacity;
        a->m_toOpacity = opacity;
        a->m_blending = blending;
    };
    apply(em);
    if (editor->m_effectManager != em) apply(editor->m_effectManager);
    editor->updateLevelColors();

    return Ok(matjson::makeObject({
        {"before", before},
        {"after", colorToJson(channel, action)},
        {"note", "Farbkanal-Aenderungen kann GD nicht per Strg+Z rueckgaengig machen. 'before' enthaelt die alten Werte."},
    }));
}

Result<matjson::Value> cmdBotAnalyze(LevelEditorLayer* editor, matjson::Value const&) {
    std::vector<std::string> warns;
    bot::Start start;
    auto objects = bot::collectLevel(editor, start, warns);
    auto r = bot::analyze(objects, start);
    r.warnings.insert(r.warnings.begin(), warns.begin(), warns.end());
    return Ok(bot::resultToJson(r));
}

std::string makeResponse(matjson::Value const& id, bool ok, matjson::Value payload) {
    if (ok) {
        return matjson::makeObject({{"id", id}, {"ok", true}, {"result", payload}}).dump(matjson::NO_INDENTATION);
    }
    return matjson::makeObject({{"id", id}, {"ok", false}, {"error", payload}}).dump(matjson::NO_INDENTATION);
}

} // namespace

std::string claude_bridge::handleRequest(std::string const& text) {
    matjson::Value id = nullptr;

    auto parsed = matjson::parse(text);
    if (parsed.isErr()) return makeResponse(id, false, "Ungueltiges JSON");
    matjson::Value req = parsed.unwrap();

    if (has(req, "id")) id = req["id"];
    std::string cmd = optString(req, "cmd").value_or("");
    matjson::Value params = has(req, "params") ? req["params"] : matjson::Value(nullptr);

    auto editor = LevelEditorLayer::get();
    bool inEditor = editor && editor->m_editorUI;

    if (cmd == "ping") {
        return makeResponse(id, true, matjson::makeObject({
            {"pong", true},
            {"inEditor", inEditor},
            {"modVersion", Mod::get()->getVersion().toVString()},
        }));
    }

    static std::unordered_map<std::string, Handler> const handlers = {
        {"get_level_info", &cmdGetLevelInfo},
        {"list_objects", &cmdListObjects},
        {"get_selected_objects", &cmdGetSelected},
        {"add_objects", &cmdAddObjects},
        {"delete_objects", &cmdDeleteObjects},
        {"modify_objects", &cmdModifyObjects},
        {"get_color_channels", &cmdGetColors},
        {"set_color_channel", &cmdSetColor},
        {"bot_analyze", &cmdBotAnalyze},
    };

    auto it = handlers.find(cmd);
    if (it == handlers.end()) return makeResponse(id, false, "Unbekannter Befehl: " + cmd);
    if (!inEditor) return makeResponse(id, false, "Bitte zuerst ein Level im Level-Editor oeffnen");

    try {
        auto result = it->second(editor, params);
        if (result.isErr()) return makeResponse(id, false, result.unwrapErr());
        return makeResponse(id, true, result.unwrap());
    }
    catch (std::exception const& e) {
        return makeResponse(id, false, std::string("Interner Fehler: ") + e.what());
    }
}
