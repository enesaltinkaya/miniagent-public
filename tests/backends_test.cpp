// Tests for the backends.json config (src/llm/backends.cpp): first run
// creates the file with the default local backend (name/url/apiKey/model),
// object entries are parsed, save() round-trips, corrupt / empty / old
// (bare-URL) files fall back to the default (with an error out) without
// touching the file.
#include "llm/backends.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

static int failures = 0;

static void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

namespace fs = std::filesystem;

static bool defaultLoaded(const std::vector<LlmBackend>& v) {
    return v.size() == 1 && v[0].name == "local" &&
           v[0].url == "http://localhost:1234/v1" && v[0].apiKey == "1234" &&
           v[0].model == "local";
}

int main() {
    const std::string base = "/tmp/backends_test";
    fs::remove_all(base);
    fs::create_directories(base + "/.config/miniagent");
    setenv("HOME", base.c_str(), 1);
    const std::string path = base + "/.config/miniagent/backends.json";

    // First run: no file -> the default local backend, and the file is
    // created.
    {
        std::string err;
        auto v = loadBackends(&err);
        check(defaultLoaded(v), "first run: default backend");
        check(fs::exists(path), "first run: file created");
        check(err.empty(), "first run: no error");
    }

    // The user adds a second backend.
    {
        std::ofstream f(path, std::ios::trunc);
        f << "[\n"
          << "  {\"name\": \"local\", \"url\": \"http://localhost:1234/v1\", "
          << "\"apiKey\": \"1234\", \"model\": \"local\"},\n"
          << "  {\"name\": \"cloud\", \"url\": \"https://api.example.com/v1\", "
          << "\"apiKey\": \"sk-abc\", \"model\": \"gpt-4o\"}\n"
          << "]\n";
    }
    {
        auto v = loadBackends();
        check(v.size() == 2 && v[0].name == "local" &&
                  v[0].url == "http://localhost:1234/v1" && v[0].apiKey == "1234" &&
                  v[0].model == "local" && v[1].name == "cloud" &&
                  v[1].url == "https://api.example.com/v1" && v[1].apiKey == "sk-abc" &&
                  v[1].model == "gpt-4o",
              "entries parsed");
    }

    // save() rewrites the file; a missing name becomes "default"; an entry
    // without a URL is dropped.
    {
        check(saveBackends({{"a", "http://a/v1", "ka", "ma"},
                            {"", "http://b/v1", "", "mb"}}),
              "save");
        auto v = loadBackends();
        check(v.size() == 2 && v[0].name == "a" && v[0].url == "http://a/v1" &&
                  v[0].apiKey == "ka" && v[0].model == "ma" && v[1].name == "default" &&
                  v[1].url == "http://b/v1" && v[1].apiKey == "" && v[1].model == "mb",
              "save round-trip");
    }

    // Old format (bare URL strings): treated as malformed -> error out,
    // default backend, file untouched.
    {
        {
            std::ofstream f(path, std::ios::trunc);
            f << "[\"http://10.0.0.5:8080/v1\"]\n";
        }
        std::ifstream in(path);
        std::string before((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::string err;
        auto v = loadBackends(&err);
        check(defaultLoaded(v), "old format: default fallback");
        check(!err.empty(), "old format: error reported");
        std::ifstream in2(path);
        std::string after((std::istreambuf_iterator<char>(in2)), std::istreambuf_iterator<char>());
        check(before == after, "old format: file not rewritten");
    }

    // Corrupt file: in-memory default with an error out, file untouched.
    {
        {
            std::ofstream f(path, std::ios::trunc);
            f << "{ this is not an array";
        }
        std::ifstream in(path);
        std::string before((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::string err;
        auto v = loadBackends(&err);
        check(defaultLoaded(v), "corrupt file: default fallback");
        check(!err.empty(), "corrupt file: error reported");
        std::ifstream in2(path);
        std::string after((std::istreambuf_iterator<char>(in2)), std::istreambuf_iterator<char>());
        check(before == after, "corrupt file: not rewritten");
    }

    // Empty (zero-byte) file: same as corrupt.
    {
        {
            std::ofstream f(path, std::ios::trunc);
        }
        std::string err;
        auto v = loadBackends(&err);
        check(defaultLoaded(v), "zero-byte file: default fallback");
        check(!err.empty(), "zero-byte file: error reported");
    }

    // Array without usable entries: fallback with an error out.
    {
        {
            std::ofstream f(path, std::ios::trunc);
            f << "[\"\", {\"name\": \"nourl\"}]\n";
        }
        std::string err;
        auto v = loadBackends(&err);
        check(defaultLoaded(v), "no usable entries: default fallback");
        check(!err.empty(), "no usable entries: error reported");
    }

    // Empty array: same fallback as corrupt.
    {
        {
            std::ofstream f(path, std::ios::trunc);
            f << "[]\n";
        }
        auto v = loadBackends();
        check(defaultLoaded(v), "empty array: default fallback");
    }

    fs::remove_all(base);
    if (failures == 0)
        std::printf("ok\n");
    return failures == 0 ? 0 : 1;
}
