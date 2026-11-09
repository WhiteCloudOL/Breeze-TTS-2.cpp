// Added by WhiteCloudOL for authentication regression coverage.
#include "auth.h"

#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>

static void require(bool ok) {
    if (!ok) throw std::runtime_error("authentication regression");
}

static void environment(const char * name, const char * value) {
#ifdef _WIN32
    _putenv_s(name, value ? value : "");
#else
    if (value) setenv(name, value, 1);
    else unsetenv(name);
#endif
}

int main() {
    try {
        using breeze::BearerAuth;
        const BearerAuth auth({"foo", "bar"});
        require(auth.accepts({"Bearer foo"}));
        require(auth.accepts({"bEaReR bar"}));
        for (const auto & header : {"", "Bearer", "Basic foo", "Bearer wrong", "Bearer foo extra", "Bearer FOO"})
            require(!auth.accepts({header}));
        require(!auth.accepts({}));
        require(!auth.accepts({"Bearer foo", "Bearer foo"}));
        require(BearerAuth().accepts({}));
        std::string selected;
        require(!auth.accepts_websocket({}, {}, selected));
        require(auth.accepts_websocket({"Bearer bar"}, {}, selected) && selected.empty());
        require(auth.accepts_websocket({}, {"breeze-api-key.Zm9v"}, selected));
        require(selected == "breeze-api-key.Zm9v");
        require(!auth.accepts_websocket({}, {"breeze-api-key.Zm9v,breeze-api-key.YmFy"}, selected));
        require(!auth.accepts_websocket({"Bearer wrong"}, {"breeze-api-key.Zm9v"}, selected));

        environment("BREEZE_API_KEYS", "foo, bar");
        environment("BREEZE_API_KEY", "superseded");
        const auto multiple = BearerAuth::load(true, "missing-file");
        require(multiple.accepts({"Bearer foo"}) && multiple.accepts({"Bearer bar"}));
        require(!multiple.accepts({"Bearer superseded"}));
        environment("BREEZE_API_KEYS", "foo,");
        bool failed = false;
        try { BearerAuth::load(true, "missing-file"); } catch (const std::exception &) { failed = true; }
        require(failed);
        environment("BREEZE_API_KEYS", nullptr);
        require(BearerAuth::load(true, "missing-file").accepts({"Bearer superseded"}));
        environment("BREEZE_API_KEY", nullptr);
        environment("BREEZE_API_KEY_FILE", nullptr);
        const auto path = std::filesystem::temp_directory_path() /
            ("breeze-auth-test-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        {
            std::ofstream file(path, std::ios::binary);
            file << "\xef\xbb\xbf" "foo\r\n\r\nlegacy,token\r\n";
        }
        const auto file_auth = BearerAuth::load(true, path.u8string());
        require(file_auth.accepts({"Bearer foo"}));
        require(file_auth.accepts({"Bearer legacy,token"}));
        {
            std::ofstream file(path, std::ios::binary);
            file << "\xff\xfe";
            for (char c : std::string("foo\r\nbar\r\n")) {
                file.put(c);
                file.put(0);
            }
        }
        require(BearerAuth::load(true, path.u8string()).accepts({"Bearer bar"}));
        std::filesystem::remove(path);
        failed = false;
        try { BearerAuth::load(true, "missing-file"); } catch (const std::exception &) { failed = true; }
        require(failed);
        require(!BearerAuth::load(false, "missing-file").enabled());
        printf("Bearer authentication tests passed\n");
        return 0;
    } catch (const std::exception & e) {
        fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}
