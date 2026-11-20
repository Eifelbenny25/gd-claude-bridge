#include "EditorBot.hpp"

#include <Geode/modify/EditorUI.hpp>

#include <cmath>
#include <memory>
#include <optional>
#include <thread>

using namespace geode::prelude;

namespace bot {

namespace {

std::optional<Kind> classify(GameObject* obj) {
    switch (obj->m_objectID) {
        case 200: return Kind::Speed05;
        case 201: return Kind::Speed1;
        case 202: return Kind::Speed2;
        case 203: return Kind::Speed3;
        case 1334: return Kind::Speed4;
        default: break;
    }
    using T = GameObjectType;
    switch (obj->m_objectType) {
        case T::Solid:
        case T::Breakable:
            return Kind::Solid;
        case T::Slope:
            if (obj->m_slopeIsHazard) return Kind::Hazard;
            return Kind::Slope; // Richtung wird in collectLevel bestimmt
        case T::Hazard:
        case T::AnimatedHazard:
            return Kind::Hazard;
        case T::YellowJumpRing: return Kind::OrbYellow;
        case T::PinkJumpRing: return Kind::OrbPink;
        case T::RedJumpRing: return Kind::OrbRed;
        case T::GravityRing: return Kind::OrbBlue;
        case T::GreenRing: return Kind::OrbGreen;
        case T::DropRing: return Kind::OrbBlack;
        case T::DashRing: return Kind::OrbDash;
        case T::GravityDashRing: return Kind::OrbDashGravity;
        case T::YellowJumpPad: return Kind::PadYellow;
        case T::PinkJumpPad: return Kind::PadPink;
        case T::RedJumpPad: return Kind::PadRed;
        case T::GravityPad: return Kind::PadBlue;
        case T::NormalGravityPortal: return Kind::GravityNormal;
        case T::InverseGravityPortal: return Kind::GravityFlip;
        case T::GravityTogglePortal: return Kind::GravityToggle;
        case T::CubePortal: return Kind::ModeCube;
        case T::ShipPortal: return Kind::ModeShip;
        case T::BallPortal: return Kind::ModeBall;
        case T::UfoPortal: return Kind::ModeUfo;
        case T::WavePortal: return Kind::ModeWave;
        case T::RobotPortal: return Kind::ModeRobot;
        case T::SpiderPortal: return Kind::ModeSpider;
        case T::SwingPortal: return Kind::ModeSwing;
        case T::MiniSizePortal: return Kind::SizeMini;
        case T::RegularSizePortal: return Kind::SizeNormal;
        case T::InverseMirrorPortal:
        case T::NormalMirrorPortal:
            return std::nullopt; // Spiegel-Portale aendern nur die Ansicht
        case T::DualPortal:
        case T::TeleportPortal:
            return Kind::Stop;
        case T::SoloPortal:
            return std::nullopt;
        case T::CustomRing:
        case T::SpiderOrb:
        case T::SpiderPad:
        case T::TeleportOrb:
            return Kind::Unsupported;
        default:
            return std::nullopt; // Deko, Trigger, Muenzen ...
    }
}

bool isMovingTrigger(int id) {
    switch (id) {
        case 901:   // Move
        case 1346:  // Rotate
        case 1347:  // Follow
        case 1814:  // Follow Player Y
        case 1049:  // Toggle
        case 3006: case 3007: case 3008: case 3009: case 3010: // Area-Trigger
            return true;
        default:
            return false;
    }
}

} // namespace

std::vector<LevelObj> collectLevel(LevelEditorLayer* editor, Start& start,
                                   std::vector<std::string>& warnings) {
    std::vector<LevelObj> out;
    bool slope = false, triggers = false;
    if (editor->m_objects) {
        for (auto obj : CCArrayExt<GameObject*>(editor->m_objects)) {
            if (isMovingTrigger(obj->m_objectID)) triggers = true;
            auto kind = classify(obj);
            if (!kind) continue;
            if (obj->m_isNoTouch) continue;
            if (*kind == Kind::Solid && obj->m_isPassable) continue;

            CCRect r = obj->getObjectRect();
            if (r.size.width <= 0.f || r.size.height <= 0.f) continue;
            LevelObj o;
            o.kind = *kind;
            o.rect = {r.getMinX(), r.getMinY(), r.getMaxX(), r.getMaxY()};
            o.uid = obj->m_uniqueID;
            o.objectId = obj->m_objectID;
            o.x = obj->getPositionX();
            o.y = obj->getPositionY();
            o.rotation = obj->getRotation();
            o.flipX = obj->isFlipX();
            {
                float rr = std::fmod(std::fmod(obj->getRotation(), 360.f) + 360.f, 360.f);
                o.upside = (std::abs(rr - 180.f) < 1.f) != obj->isFlipY();
            }
            if (o.kind == Kind::Slope) {
                // Im Editor setzt GD m_slopeDirection erst beim Playtest. Darum
                // selbst bestimmen: ungespiegelt/0 Grad = steigt nach rechts, Boden oben.
                float rot = std::fmod(std::fmod(obj->getRotation(), 360.f) + 360.f, 360.f);
                bool fx = obj->isFlipX(), fy = obj->isFlipY();
                bool ok = true;
                if (std::abs(rot - 180.f) < 1.f) { fx = !fx; fy = !fy; }
                else if (rot > 1.f && rot < 359.f) ok = false; // 90/270 Grad: als Block
                if (!ok || fy) {
                    o.kind = Kind::Solid;
                    slope = true;
                }
                else {
                    bool uphillRight = !fx;
                    o.sy0 = uphillRight ? o.rect.y0 : o.rect.y1;
                    o.sy1 = uphillRight ? o.rect.y1 : o.rect.y0;
                }
            }
            out.push_back(o);
        }
    }
    if (slope) warnings.push_back("Gedrehte Schraegen oder Decken-Schraegen werden wie Bloecke behandelt.");
    if (triggers) warnings.push_back("Move-/Toggle-Trigger werden nicht simuliert (Objekte bleiben an ihrer Editor-Position).");

    start = Start{};
    if (auto ls = editor->m_levelSettings) {
        switch (ls->m_startSpeed) {
            case Speed::Slow: start.speed = 0; break;
            case Speed::Normal: start.speed = 1; break;
            case Speed::Fast: start.speed = 2; break;
            case Speed::Faster: start.speed = 3; break;
            case Speed::Fastest: start.speed = 4; break;
        }
        start.gravityFlipped = ls->m_isFlipped;
        if (ls->m_startMode >= 0 && ls->m_startMode <= 7) start.mode = static_cast<Mode>(ls->m_startMode);
        start.mini = ls->m_startMini;
        if (ls->m_startDual) warnings.push_back("Dual-Start wird nicht simuliert (nur ein Spieler).");
    }
    if (start.gravityFlipped) start.y = 105.f;
    return out;
}

matjson::Value resultToJson(Result const& r) {
    std::vector<matjson::Value> clicks;
    for (auto const& c : r.clicks) {
        clicks.push_back(matjson::makeObject({
            {"x", c.x},
            {"y", c.y - 90.f},
            {"what", c.what},
            {"tick", c.tick},
            {"windowTicks", c.windowTicks()},
            {"windowStartTick", c.windowStart},
            {"windowEndTick", c.windowEnd},
            {"label", clickLabel(c)},
        }));
    }
    std::vector<matjson::Value> nerfs;
    for (auto const& n : r.nerfs) {
        nerfs.push_back(matjson::makeObject({
            {"uid", n.uid},
            {"objectId", n.objectId},
            {"change", n.change},
            {"makesPossible", n.makesPossible},
            {"reachedX", n.reachedX},
        }));
    }
    std::vector<matjson::Value> warns(r.warnings.begin(), r.warnings.end());
    std::vector<matjson::Value> unused(r.unusedObjects.begin(), r.unusedObjects.end());
    return matjson::makeObject({
        {"possible", r.possible},
        {"stoppedAtUnsupported", r.stoppedAtUnsupported},
        {"reachedX", r.reachedX},
        {"deathX", r.deathX},
        {"deathY", r.deathY - 90.f},
        {"killerUid", r.killerUid},
        {"killerObjectId", r.killerObjectId},
        {"clicks", matjson::Value(clicks)},
        {"nerfs", matjson::Value(nerfs)},
        {"unusedObjects", matjson::Value(unused)},
        {"warnings", matjson::Value(warns)},
        {"summary", summary(r)},
        {"note", "y-Werte in Level-Koordinaten (Boden = 0). 240 Ticks/s, 4 Ticks = 1 Frame bei 60 FPS."},
    });
}

} // namespace bot

// ---------------------------------------------------------------------------
// Editor-Knoepfe "Bot" und "Frames" + Anzeige im Level
// ---------------------------------------------------------------------------
class $modify(BotEditorUI, EditorUI) {
    struct Fields {
        CCDrawNode* draw = nullptr;
        CCNode* labels = nullptr;
        std::shared_ptr<bool> alive = std::make_shared<bool>(true);
        std::optional<bot::Result> result;
        bool running = false;
        bool showFrames = false;
        bool framesAfterRun = false;
        ~Fields() { *alive = false; }
    };

    bool init(LevelEditorLayer* layer) {
        if (!EditorUI::init(layer)) return false;

        auto win = CCDirector::get()->getWinSize();
        auto menu = CCMenu::create();
        menu->setID("claude-bot-menu"_spr);

        auto botSpr = ButtonSprite::create("Bot", "goldFont.fnt", "GJ_button_01.png", .7f);
        auto botBtn = CCMenuItemSpriteExtra::create(botSpr, this, menu_selector(BotEditorUI::onBotRun));
        botBtn->setID("bot-button"_spr);
        auto frSpr = ButtonSprite::create("Frames", "goldFont.fnt", "GJ_button_02.png", .7f);
        auto frBtn = CCMenuItemSpriteExtra::create(frSpr, this, menu_selector(BotEditorUI::onBotFrames));
        frBtn->setID("frames-button"_spr);

        botBtn->setPosition({0.f, 0.f});
        frBtn->setPosition({0.f, -32.f});
        menu->addChild(botBtn);
        menu->addChild(frBtn);
        menu->setPosition({win.width - 45.f, win.height - 125.f});
        this->addChild(menu, 50);

        auto draw = CCDrawNode::create();
        auto labels = CCNode::create();
        layer->m_objectLayer->addChild(draw, 9998);
        layer->m_objectLayer->addChild(labels, 9999);
        m_fields->draw = draw;
        m_fields->labels = labels;
        return true;
    }

    void onBotRun(CCObject*) {
        m_fields->framesAfterRun = false;
        startAnalysis();
    }

    void onBotFrames(CCObject*) {
        if (!m_fields->result) {
            m_fields->framesAfterRun = true;
            startAnalysis();
            return;
        }
        m_fields->showFrames = !m_fields->showFrames;
        m_fields->labels->setVisible(m_fields->showFrames);
        if (m_fields->showFrames) showFramesPopup();
    }

    void startAnalysis() {
        if (m_fields->running) {
            Notification::create("Bot rechnet noch ...", NotificationIcon::Loading)->show();
            return;
        }
        std::vector<std::string> warns;
        bot::Start start;
        auto objects = bot::collectLevel(m_editorLayer, start, warns);
        m_fields->running = true;
        Notification::create("Bot spielt das Level ...", NotificationIcon::Loading, 1.5f)->show();

        auto alive = m_fields->alive;
        auto self = this;
        std::thread([objects = std::move(objects), start, warns, alive, self]() {
            bot::Result r = bot::analyze(objects, start);
            r.warnings.insert(r.warnings.begin(), warns.begin(), warns.end());
            geode::queueInMainThread([r = std::move(r), alive, self]() mutable {
                if (!*alive) return;
                self->onAnalysisDone(std::move(r));
            });
        }).detach();
    }

    void onAnalysisDone(bot::Result r) {
        m_fields->running = false;
        m_fields->result = std::move(r);
        drawResult();
        if (m_fields->framesAfterRun && m_fields->result->possible) {
            m_fields->showFrames = true;
            m_fields->labels->setVisible(true);
            showFramesPopup();
        }
        else {
            m_fields->labels->setVisible(m_fields->showFrames);
            auto const& res = *m_fields->result;
            FLAlertLayer::create(nullptr, res.possible ? "Bot: moeglich" : "Bot: unmoeglich",
                                 bot::summary(res), "OK", nullptr, 420.f, true, 260.f, 1.f)
                ->show();
        }
    }

    static ccColor4F colorFor(int ticks) {
        if (ticks <= 1) return {1.f, 0.15f, 0.15f, 1.f};   // 240fps frame perfect
        if (ticks <= 4) return {1.f, 0.55f, 0.1f, 1.f};    // 60fps frame perfect
        if (ticks <= 8) return {1.f, 0.9f, 0.2f, 1.f};     // eng
        return {0.3f, 1.f, 0.4f, 1.f};                     // locker
    }

    void drawResult() {
        auto draw = m_fields->draw;
        auto labels = m_fields->labels;
        draw->clear();
        labels->removeAllChildren();
        auto const& r = *m_fields->result;

        ccColor4F pathColor = r.possible ? ccColor4F{0.2f, 0.9f, 1.f, 0.9f}
                                         : ccColor4F{1.f, 0.3f, 0.3f, 0.9f};
        for (size_t i = 1; i < r.path.size(); ++i) {
            draw->drawSegment({r.path[i - 1].x, r.path[i - 1].y},
                              {r.path[i].x, r.path[i].y}, 1.5f, pathColor);
        }

        int n = 0;
        for (auto const& c : r.clicks) {
            auto col = colorFor(c.windowTicks());
            draw->drawDot({c.x, c.y}, 5.f, col);
            auto text = c.what + "\n" + bot::clickLabel(c);
            auto label = CCLabelBMFont::create(text.c_str(), "chatFont.fnt");
            label->setScale(0.45f);
            label->setAlignment(kCCTextAlignmentCenter);
            label->setColor({static_cast<GLubyte>(col.r * 255), static_cast<GLubyte>(col.g * 255),
                             static_cast<GLubyte>(col.b * 255)});
            label->setPosition({c.x, c.y + ((n++ % 2) ? 34.f : 22.f)});
            labels->addChild(label);
        }

        if (!r.possible && r.deathX > -1e8f) {
            float x = r.deathX, y = r.deathY, s = 8.f;
            ccColor4F red{1.f, 0.f, 0.f, 1.f};
            draw->drawSegment({x - s, y - s}, {x + s, y + s}, 2.f, red);
            draw->drawSegment({x - s, y + s}, {x + s, y - s}, 2.f, red);
        }
    }

    void showFramesPopup() {
        auto const& r = *m_fields->result;
        if (!r.possible) {
            FLAlertLayer::create(nullptr, "Frames", "Erst muss der Teil schaffbar sein.\n\n" + bot::summary(r),
                                 "OK", nullptr, 420.f, true, 260.f, 1.f)->show();
            return;
        }
        std::string text;
        int n = 0;
        for (auto const& c : r.clicks) {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "x=%.0f  %s: %s\n", c.x, c.what.c_str(),
                          bot::clickLabel(c).c_str());
            text += buf;
            ++n;
        }
        if (n == 0) text = "Keine Klicks noetig.\n";
        text += "\n(240 Ticks/s, 4 Ticks = 1 Frame bei 60 FPS. Werte sind Schaetzungen +-1 Tick.)";
        FLAlertLayer::create(nullptr, "Klick-Fenster", text, "OK", nullptr, 420.f, true, 260.f, 0.9f)->show();
    }
};
