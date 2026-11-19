#include <Geode/Geode.hpp>
#include <Geode/modify/CCScheduler.hpp>

#include <deque>
#include <mutex>

#include "Commands.hpp"
#include "Server.hpp"

using namespace geode::prelude;

namespace {
    // Nachrichten kommen im WebSocket-Thread an. GD darf aber nur im
    // Haupt-Thread veraendert werden. Darum: erst in eine Warteschlange legen ...
    std::mutex g_queueMutex;
    std::deque<std::pair<bridge::ClientId, std::string>> g_queue;
}

$on_mod(Loaded) {
    auto port = Mod::get()->getSettingValue<int64_t>("port");
    auto error = bridge::startServer(
        static_cast<std::uint16_t>(port),
        [](bridge::ClientId client, std::string message) {
            std::lock_guard lock(g_queueMutex);
            g_queue.emplace_back(client, std::move(message));
        }
    );
    if (error.empty()) {
        log::info("Claude Bridge laeuft auf ws://127.0.0.1:{}", port);
    }
    else {
        log::error("Claude Bridge konnte nicht starten: {}", error);
    }
}

// ... und dann jeden Frame im Haupt-Thread abarbeiten.
class $modify(ClaudeBridgeScheduler, CCScheduler) {
    void update(float dt) {
        CCScheduler::update(dt);

        std::deque<std::pair<bridge::ClientId, std::string>> batch;
        {
            std::lock_guard lock(g_queueMutex);
            batch.swap(g_queue);
        }
        for (auto& [client, message] : batch) {
            bridge::sendToClient(client, claude_bridge::handleRequest(message));
        }
    }
};
