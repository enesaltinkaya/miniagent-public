#include "llm/backends.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <utility>

#include <nlohmann/json.hpp>

#include "util/settings.h"

namespace {

const LlmBackend kDefault{"local", "http://localhost:1234/v1", "1234", "local"};

nlohmann::json toJson(const LlmBackend& b) {
    return {
        {"name", b.name},
        {"url", b.url},
        {"apiKey", b.apiKey},
        {"model", b.model},
    };
}

}

std::string backendsPath() { return miniagentConfigDir() + "/backends.json"; }

std::vector<LlmBackend> loadBackends(std::string* outError) {
    const std::string path = backendsPath();
    if (!std::ifstream(path)) {
        // First run: create the file with the default local backend.
        if (EnsureConfigDir())
            saveBackends({kDefault});
        return {kDefault};
    }
    std::ifstream f(path);
    const std::string content{
        std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
    nlohmann::json j = nlohmann::json::parse(content, nullptr, false);
    std::vector<LlmBackend> out;
    bool legacy = false;
    if (j.is_array()) {
        for (const auto& e : j) {
            if (e.is_string()) {
                // Old malformed format: bare URL strings instead of objects.
                legacy = true;
                continue;
            }
            if (!e.is_object())
                continue;
            LlmBackend b;
            b.name = e.value("name", std::string());
            b.url  = e.value("url", std::string());
            b.apiKey = e.value("apiKey", std::string());
            b.model  = e.value("model", std::string());
            if (b.url.empty())
                continue;
            if (b.name.empty())
                b.name = "default";
            out.push_back(std::move(b));
        }
        if (legacy && outError) {
            *outError = path +
                        " uses the old bare-URL format; expected objects with "
                        "name/url/apiKey/model; using the default backend";
            return {kDefault};
        }
    } else if (outError) {
        *outError = path + " is not valid JSON; using the default backend";
        return {kDefault};
    }
    if (out.empty()) {
        if (outError)
            *outError = path + " lists no usable backends; using the default";
        return {kDefault};
    }
    return out;
}

bool saveBackends(const std::vector<LlmBackend>& backends) {
    const std::string path = backendsPath();
    const std::string tmp  = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "w");
    if (!f)
        return false;
    fprintf(f, "[\n");
    for (size_t i = 0; i < backends.size(); ++i) {
        fprintf(f, "  %s", toJson(backends[i]).dump().c_str());
        if (i + 1 < backends.size())
            fputc(',', f);
        fputc('\n', f);
    }
    fprintf(f, "]\n");
    fclose(f);
    if (rename(tmp.c_str(), path.c_str()) != 0) {
        remove(tmp.c_str());
        return false;
    }
    return true;
}
