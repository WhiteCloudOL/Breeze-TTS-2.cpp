#pragma once

#include "breeze/codec.h"
#include "breeze/model.h"
#include "breeze/scheduler.h"

#include "httplib.h"

#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace breeze {

// form fields arrive as either a file part or a plain param depending on the client
inline std::string field(const httplib::Request & req, const char * name, const std::string & def) {
    if (req.has_file(name)) return req.get_file_value(name).content;
    if (req.has_param(name)) return req.get_param_value(name);
    return def;
}

// hash of the clip bytes and its transcript; the id unnamed reference clips are cached under
std::string voice_key(const std::string & wav, const std::string & text);

inline constexpr char gateway_voice_prefix[] = "/new-api/";
inline constexpr char gateway_voice_route[] = "/new-api/([a-f0-9]{64})";

// Private route patterns validate the scope segment before reaching a handler.
inline std::string voice_scope(const httplib::Request & req) {
    return req.path.compare(0, sizeof(gateway_voice_prefix) - 1, gateway_voice_prefix) == 0
        ? req.path.substr(sizeof(gateway_voice_prefix) - 1, 64) : std::string();
}

struct CachedVoice {
    std::vector<int> codes;
    int frames = 0;
    std::string text;
    std::string description;
    std::map<std::string, std::string> meta;
    double encode_ms = 0;
    bool saved = false; // has a .breeze file behind it, so it survives a restart
};

// reference voices the server already encoded, both the ones posted at runtime and the files on disk
class VoiceStore {
public:
    explicit VoiceStore(bool direct_can_manage_all = false) : m_direct_can_manage_all(direct_can_manage_all) {}
    void load_dir(const std::string & dir, int n_codebooks, const std::string & scope = "");
    void load_gateway_dir(const std::string & dir, int n_codebooks);

    // fills in a cached reference, false only when the id is unknown
    bool take(const std::string & id, std::vector<int> & codes, int & frames, std::string & text,
              const std::string & scope = "");

    // in memory insert, unnamed entries get evicted oldest first when the table is full
    void put(const std::string & id, CachedVoice v, const std::string & scope = "");

    void add_routes(httplib::Server & svr, BreezeModel & model, MimiCodec & codec, GenScheduler & sched,
                    const std::string & dir, bool gateway_enabled);

private:
    struct VoiceLocation {
        std::string scope;
        std::string id;
    };
    VoiceLocation locate_voice(const std::string & request_scope, const std::string & id) const;

    struct Library {
        std::map<std::string, CachedVoice> voices;
        std::vector<std::string> order;
        void put(const std::string & id, CachedVoice v);
    };
    std::map<std::string, Library> m_libraries;
    std::mutex m_mutex;
    const bool m_direct_can_manage_all;
    static const size_t max_voices = 64;
};

} // namespace breeze
