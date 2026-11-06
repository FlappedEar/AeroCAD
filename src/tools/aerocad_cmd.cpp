// SPDX-License-Identifier: GPL-3.0-or-later
// aerocad-cmd: executes a JSON command script without the UI.
//
//   aerocad-cmd <script.json | ->
//
// The script is a JSON array of {"name": "...", "params": {...}} objects, the same calls an
// MCP client makes. Each result is printed as one JSON line on stdout. Execution stops at
// the first failing command (exit code 1); malformed input exits with code 2.
#include "aerocad/commands/Session.hpp"

#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

using aerocad::commands::Json;

int main(int argc, char** argv)
{
    if (argc != 2 || std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h") {
        std::cerr << "usage: aerocad-cmd <script.json | ->\n"
                     "Runs a JSON array of {\"name\", \"params\"} commands; prints one JSON result per line.\n";
        return argc == 2 ? 0 : 2;
    }

    std::string text;
    const std::string source = argv[1];
    if (source == "-") {
        text.assign(std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>());
    } else {
        std::ifstream in(source, std::ios::binary);
        if (!in) {
            std::cerr << "aerocad-cmd: cannot open " << source << "\n";
            return 2;
        }
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    const Json script = Json::parse(text, nullptr, false);
    if (!script.is_array()) {
        std::cerr << "aerocad-cmd: the script must be a JSON array of commands\n";
        return 2;
    }

    aerocad::commands::Session session;
    for (const auto& step : script) {
        if (!step.is_object() || !step.contains("name") || !step["name"].is_string()) {
            std::cerr << "aerocad-cmd: step is not an object with a string \"name\": " << step.dump() << "\n";
            return 2;
        }
        const std::string name = step["name"].get<std::string>();
        const auto result = session.execute(name, step.value("params", Json::object()));
        Json line = result.toJson();
        line["command"] = name;
        std::cout << line.dump() << "\n";
        if (!result.ok) {
            std::cerr << "aerocad-cmd: " << name << " failed: " << result.error << "\n";
            return 1;
        }
    }
    return 0;
}
