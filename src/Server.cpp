// websocketpp/asio zuerst einbinden (wegen winsock2.h vor windows.h)
#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>

#include "Server.hpp"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

namespace {
    using WsServer = websocketpp::server<websocketpp::config::asio>;
    using Hdl = websocketpp::connection_hdl;

    WsServer* g_server = nullptr;
    std::mutex g_clientsMutex;
    std::map<bridge::ClientId, Hdl> g_clients;
    std::map<Hdl, bridge::ClientId, std::owner_less<Hdl>> g_ids;
    std::atomic<bridge::ClientId> g_nextId{1};
}

std::string bridge::startServer(std::uint16_t port, MessageCallback onMessage) {
    if (g_server) return "Server laeuft bereits";

    try {
        g_server = new WsServer();
        g_server->clear_access_channels(websocketpp::log::alevel::all);
        g_server->clear_error_channels(websocketpp::log::elevel::all);
        g_server->init_asio();
        g_server->set_reuse_addr(true);
        g_server->set_max_message_size(64 * 1024 * 1024);

        // Sicherheit: Webseiten im Browser schicken immer einen "Origin"-Header.
        // Unser Node-Programm schickt keinen. Damit kann keine fremde Webseite
        // heimlich deinen Editor steuern.
        g_server->set_validate_handler([](Hdl hdl) {
            auto con = g_server->get_con_from_hdl(hdl);
            return con->get_origin().empty();
        });

        g_server->set_open_handler([](Hdl hdl) {
            std::lock_guard lock(g_clientsMutex);
            auto id = g_nextId++;
            g_clients[id] = hdl;
            g_ids[hdl] = id;
        });

        g_server->set_close_handler([](Hdl hdl) {
            std::lock_guard lock(g_clientsMutex);
            auto it = g_ids.find(hdl);
            if (it != g_ids.end()) {
                g_clients.erase(it->second);
                g_ids.erase(it);
            }
        });

        g_server->set_message_handler([onMessage](Hdl hdl, WsServer::message_ptr msg) {
            bridge::ClientId id = 0;
            {
                std::lock_guard lock(g_clientsMutex);
                auto it = g_ids.find(hdl);
                if (it != g_ids.end()) id = it->second;
            }
            if (id != 0) onMessage(id, msg->get_payload());
        });

        // NUR 127.0.0.1 (loopback) - von aussen/anderen PCs nicht erreichbar.
        websocketpp::lib::asio::ip::tcp::endpoint endpoint(
            websocketpp::lib::asio::ip::address_v4::loopback(), port
        );
        g_server->listen(endpoint);
        g_server->start_accept();
    }
    catch (std::exception const& e) {
        return e.what();
    }

    std::thread([] {
        try {
            g_server->run();
        }
        catch (...) {}
    }).detach();

    return "";
}

void bridge::sendToClient(ClientId client, std::string const& text) {
    if (!g_server) return;
    Hdl hdl;
    {
        std::lock_guard lock(g_clientsMutex);
        auto it = g_clients.find(client);
        if (it == g_clients.end()) return;
        hdl = it->second;
    }
    websocketpp::lib::error_code ec;
    g_server->send(hdl, text, websocketpp::frame::opcode::text, ec);
}
