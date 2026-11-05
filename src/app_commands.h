#pragma once

#include <string>

// Is this prompt one of the app's own slash commands? A leading '/' alone is
// not the test: a pasted absolute path ("/home/me/shot.jpeg\nlook at this") is
// plain text, and reading it as a command dropped Scaffold-mode prompts into
// the Simple agent with no ledger written. Only exact command tokens (or the
// "/skill:<name>" form) count. Keep this list in sync with the commands
// handled in App::sendPrompt().
inline bool isSlashCommand(const std::string& prompt) {
    if (prompt.empty() || prompt.front() != '/')
        return false;
    const std::string tok = prompt.substr(0, prompt.find_first_of(" \t\r\n"));
    return tok == "/new" || tok == "/skills" || tok == "/image" ||
           tok == "/scaffold" || tok.rfind("/skill:", 0) == 0;
}
