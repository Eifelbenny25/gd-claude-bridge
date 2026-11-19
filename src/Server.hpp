#pragma once
// Bewusst ohne Geode- und ohne websocketpp-Header, damit sich
// Windows-/asio-Header nicht mit Geode in die Quere kommen.
#include <cstdint>
#include <functional>
#include <string>

namespace bridge {
    using ClientId = std::uint64_t;
    using MessageCallback = std::function<void(ClientId, std::string)>;

    // Startet den Server auf 127.0.0.1:<port> in einem eigenen Thread.
    // Gibt einen leeren String zurueck, wenn alles geklappt hat, sonst die Fehlermeldung.
    std::string startServer(std::uint16_t port, MessageCallback onMessage);

    // Schickt eine Text-Nachricht an einen verbundenen Client (thread-sicher).
    void sendToClient(ClientId client, std::string const& text);
}
