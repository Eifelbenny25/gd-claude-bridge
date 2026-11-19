#pragma once
#include <string>

namespace claude_bridge {
    // Nimmt eine JSON-Anfrage {"id":..,"cmd":"..","params":{..}} entgegen
    // und gibt die JSON-Antwort zurueck. Muss im Haupt-Thread laufen.
    std::string handleRequest(std::string const& text);
}
