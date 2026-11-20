// Modified by WhiteCloudOL to queue reference encoding and release slots on exceptions.
#include "voices.h"
#include "queue.h"
#include "json.h"

#include "breeze/audio.h"
#include "breeze/voice.h"

#include <chrono>
#include <cstdio>
#include <algorithm>
#include <filesystem>

namespace breeze {

// keyed off the clip and its transcript, so sending the same voice twice lands on the same entry
std::string voice_key(const std::string & wav, const std::string & text) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : wav) { h ^= c; h *= 1099511628211ull; }
    for (unsigned char c : text) { h ^= c; h *= 1099511628211ull; }
    char buf[32];
    snprintf(buf, sizeof buf, "v_%016llx", (unsigned long long) h);
    return buf;
}

static std::string json_escape(const std::string & s) {
    std::string o;
    for (char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if ((unsigned char) c < 0x20) {
                    char b[8];
                    snprintf(b, sizeof b, "\\u%04x", (unsigned) (unsigned char) c);
                    o += b;
                } else {
                    o += c;
                }
        }
    }
    return o;
}

static std::string voice_json(const std::string & id, const CachedVoice & v, int sr, int spf) {
    char buf[256];
    snprintf(buf, sizeof buf,
             "{\"id\":\"%s\",\"frames\":%d,\"seconds\":%.2f,\"encode_ms\":%.0f,\"saved\":%s,\"ref_text\":\"",
             json_escape(id).c_str(), v.frames, (double) v.frames * spf / sr, v.encode_ms,
             v.saved ? "true" : "false");
    std::string out = std::string(buf) + json_escape(v.text) + "\",\"description\":\"" +
                      json_escape(v.description) + "\",\"metadata\":{";
    bool first = true;
    for (const auto & kv : v.meta) {
        if (!first) out += ",";
        first = false;
        out += "\"" + json_escape(kv.first) + "\":\"" + json_escape(kv.second) + "\"";
    }
    return out + "}}";
}

static void fail(httplib::Response & res, int status, const std::string & msg) {
    res.status = status;
    res.set_content("{\"error\":\"" + json_escape(msg) + "\"}", "application/json");
}

// description and metadata come either as a json body or as form fields, metadata being a json string
static std::string read_meta_request(const httplib::Request & req, JVal & root) {
    root = JVal();
    if (req.get_header_value("Content-Type").find("json") != std::string::npos) {
        if (!JParser(req.body).parse(root) || root.kind != JVal::Obj) return "request body must be a json object";
        return "";
    }
    root.kind = JVal::Obj;
    if (req.has_file("description") || req.has_param("description")) {
        JVal d;
        d.kind = JVal::Str;
        d.str = field(req, "description", "");
        root.obj["description"] = d;
    }
    const std::string meta = field(req, "metadata", "");
    if (!meta.empty()) {
        JVal m;
        if (!JParser(meta).parse(m) || m.kind != JVal::Obj) return "metadata must be a json object";
        root.obj["metadata"] = m;
    }
    return "";
}

// merges onto v, null or an empty string clears a field
static std::string apply_meta(const JVal & root, CachedVoice & v) {
    auto it = root.obj.find("description");
    if (it != root.obj.end()) {
        if (it->second.kind == JVal::Null) v.description.clear();
        else if (it->second.kind == JVal::Str) v.description = it->second.str;
        else return "description must be a string";
        if (v.description.size() > max_voice_description)
            return "description is longer than " + std::to_string(max_voice_description) + " bytes";
    }
    it = root.obj.find("metadata");
    if (it != root.obj.end() && it->second.kind != JVal::Null) {
        if (it->second.kind != JVal::Obj) return "metadata must be a json object";
        for (const auto & kv : it->second.obj) {
            if (!valid_voice_meta_key(kv.first))
                return "metadata keys are 1 to 64 letters, digits, dot, dash or underscore, and not description";
            if (kv.second.kind == JVal::Null || (kv.second.kind == JVal::Str && kv.second.str.empty())) {
                v.meta.erase(kv.first);
                continue;
            }
            if (kv.second.kind != JVal::Str) return "metadata values must be strings";
            if (kv.second.str.size() > max_voice_meta_value)
                return "metadata values are limited to " + std::to_string(max_voice_meta_value) + " bytes";
            v.meta[kv.first] = kv.second.str;
        }
        if (v.meta.size() > max_voice_meta_fields)
            return "a voice can have at most " + std::to_string(max_voice_meta_fields) + " metadata fields";
    }
    return "";
}

void VoiceStore::load_dir(const std::string & dir, int n_codebooks, const std::string & scope) {
    for (Voice & v : load_voice_dir(dir)) {
        if (v.n_codebooks != n_codebooks) {
            fprintf(stderr, "voice %s was made for a different model, skipping\n", v.name.c_str());
            continue;
        }
        CachedVoice cv;
        cv.codes = std::move(v.codes);
        cv.frames = v.frames;
        cv.text = v.text;
        cv.description = v.description;
        cv.meta = v.meta;
        cv.saved = true;
        auto & library = m_libraries[scope];
        library.voices[v.name] = std::move(cv);
        library.order.push_back(v.name);
    }
    if (auto library = m_libraries.find(scope); library != m_libraries.end())
        printf("loaded %zu saved voices%s\n", library->second.order.size(), scope.empty() ? "" : " for a gateway user");
}

void VoiceStore::load_gateway_dir(const std::string & dir, int n_codebooks) {
    std::error_code ec;
    for (const auto & entry : std::filesystem::directory_iterator(dir + "/.new-api", ec)) {
        const auto scope = entry.path().filename().string();
        if (scope.size() == 64 && scope.find_first_not_of("0123456789abcdef") == std::string::npos &&
            entry.is_directory(ec) && !entry.is_symlink(ec))
            load_dir(entry.path().string(), n_codebooks, scope);
    }
}

bool VoiceStore::take(const std::string & id, std::vector<int> & codes, int & frames,
                      std::string & text, const std::string & scope) {
    std::lock_guard<std::mutex> vg(m_mutex);
    const auto library = m_libraries.find(scope);
    if (library == m_libraries.end()) return false;
    const auto it = library->second.voices.find(id);
    if (it == library->second.voices.end()) return false;
    codes = it->second.codes;
    frames = it->second.frames;
    if (text.empty()) text = it->second.text;
    return true;
}

void VoiceStore::put(const std::string & id, CachedVoice v, const std::string & scope) {
    std::lock_guard<std::mutex> vg(m_mutex);
    m_libraries[scope].put(id, std::move(v));
}

void VoiceStore::Library::put(const std::string & id, CachedVoice v) {
    const bool fresh = !voices.count(id);
    if (fresh && order.size() >= max_voices) {
        // saved voices are not evicted, they were asked for by name
        for (size_t i = 0; i < order.size(); i++) {
            if (voices[order[i]].saved) continue;
            voices.erase(order[i]);
            order.erase(order.begin() + i);
            break;
        }
    }
    if (fresh) order.push_back(id);
    voices[id] = std::move(v);
}

void VoiceStore::add_routes(httplib::Server & svr, BreezeModel & model, MimiCodec & codec,
                            GenScheduler & sched, const std::string & dir, bool gateway_enabled) {
    const int sr = model.cfg.sample_rate;
    const int spf = model.cfg.samples_per_frame;
    const int ncb = model.cfg.num_codebooks;

    auto write_file = [dir, sr, ncb](const std::string & scope, const std::string & name, const CachedVoice & v) {
        Voice out;
        out.name = name;
        out.text = v.text;
        out.description = v.description;
        out.meta = v.meta;
        out.codes = v.codes;
        out.frames = v.frames;
        out.n_codebooks = ncb;
        out.sample_rate = sr;
        const std::string library_dir = scope.empty() ? dir : dir + "/.new-api/" + scope;
        return save_voice(library_dir + "/" + name + ".breeze", out);
    };

    const auto create = [&, sr, spf, write_file](const httplib::Request & req, httplib::Response & res) {
        const std::string scope = voice_scope(req);
        const std::string text = field(req, "ref_text", "");
        const std::string name = field(req, "name", "");
        if (!req.has_file("ref_audio") || text.empty())
            return fail(res, 400, "ref_audio and ref_text are required");
        if (!name.empty() && !valid_voice_name(name))
            return fail(res, 400, "name can only use letters, digits, dash and underscore");
        JVal meta;
        const std::string bad = read_meta_request(req, meta);
        if (!bad.empty()) return fail(res, 400, bad);

        const auto & f = req.get_file_value("ref_audio");
        // a named voice is written to disk under that name, an unnamed one just lives in memory
        const std::string id = name.empty() ? voice_key(f.content, text) : name;
        CachedVoice v;
        {
            std::lock_guard<std::mutex> vg(m_mutex);
            auto library = m_libraries.find(scope);
            if (library != m_libraries.end()) {
                auto it = library->second.voices.find(id);
                if (it != library->second.voices.end() && name.empty()) {
                    CachedVoice next = it->second;
                    const std::string err = apply_meta(meta, next);
                    if (!err.empty()) return fail(res, 400, err);
                    it->second = std::move(next);
                    res.set_content(voice_json(id, it->second, sr, spf), "application/json");
                    return;
                }
            }
        }
        const std::string err = apply_meta(meta, v);
        if (!err.empty()) return fail(res, 400, err);

        std::vector<float> pcm;
        read_wav_buffer((const uint8_t *) f.content.data(), f.content.size(), sr, pcm);
        if (pcm.empty()) return fail(res, 400, "could not read ref_audio");

        // encoding runs on the same device generation does, so it waits its turn
        if (!wait_for_slot(sched, req, res)) return;
        SlotHold slot(sched, 1);

        v.text = text;
        const auto t0 = std::chrono::steady_clock::now();
        sched.exec([&] { v.codes = codec.encode(pcm, v.frames); });
        v.encode_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

        std::lock_guard<std::mutex> vg(m_mutex);
        auto & library = m_libraries[scope];
        if (!name.empty()) {
            // Merge with the latest metadata under the same lock as PATCH and save.
            if (auto it = library.voices.find(id); it != library.voices.end()) {
                v.description = it->second.description;
                v.meta = it->second.meta;
                const std::string bad_meta = apply_meta(meta, v);
                if (!bad_meta.empty()) return fail(res, 400, bad_meta);
            }
            if (!write_file(scope, name, v)) return fail(res, 500, "could not write the voice file");
            v.saved = true;
        }
        const std::string body = voice_json(id, v, sr, spf);
        printf("voice %s cached%s, %d frames in %.0f ms\n", id.c_str(),
               v.saved ? " and saved" : "", v.frames, v.encode_ms);
        fflush(stdout);
        library.put(id, std::move(v));
        res.set_content(body, "application/json");
    };

    const auto list = [&, sr, spf](const httplib::Request & req, httplib::Response & res) {
        std::lock_guard<std::mutex> vg(m_mutex);
        const auto library = m_libraries.find(voice_scope(req));
        std::string out = "[";
        if (library != m_libraries.end()) {
            for (const auto & id : library->second.order) {
                if (out.size() > 1) out += ",";
                out += voice_json(id, library->second.voices.at(id), sr, spf);
            }
        }
        res.set_content(out + "]", "application/json");
    };

    const auto get = [&, sr, spf](const httplib::Request & req, httplib::Response & res) {
        const std::string id = req.matches[req.matches.size() - 1];
        std::lock_guard<std::mutex> vg(m_mutex);
        const auto library = m_libraries.find(voice_scope(req));
        if (library == m_libraries.end()) return fail(res, 404, "unknown voice_id");
        const auto it = library->second.voices.find(id);
        if (it == library->second.voices.end()) return fail(res, 404, "unknown voice_id");
        res.set_content(voice_json(id, it->second, sr, spf), "application/json");
    };

    const auto patch = [&, sr, spf, write_file](const httplib::Request & req, httplib::Response & res) {
        const std::string scope = voice_scope(req);
        const std::string id = req.matches[req.matches.size() - 1];
        std::lock_guard<std::mutex> vg(m_mutex);
        const auto library = m_libraries.find(scope);
        if (library == m_libraries.end()) return fail(res, 404, "unknown voice_id");
        const auto it = library->second.voices.find(id);
        if (it == library->second.voices.end()) return fail(res, 404, "unknown voice_id");
        JVal meta;
        std::string err = read_meta_request(req, meta);
        if (!err.empty()) return fail(res, 400, err);
        CachedVoice next = it->second;
        err = apply_meta(meta, next);
        if (!err.empty()) return fail(res, 400, err);
        if (next.saved && !write_file(scope, id, next)) return fail(res, 500, "could not write the voice file");
        it->second = std::move(next);
        printf("voice %s metadata updated%s\n", id.c_str(), it->second.saved ? " and saved" : "");
        fflush(stdout);
        res.set_content(voice_json(id, it->second, sr, spf), "application/json");
    };

    const auto remove = [&](const httplib::Request & req, httplib::Response & res) {
        const std::string id = req.matches[req.matches.size() - 1];
        std::lock_guard<std::mutex> vg(m_mutex);
        const auto library = m_libraries.find(voice_scope(req));
        if (library == m_libraries.end()) return fail(res, 404, "unknown voice_id");
        auto & voices = library->second.voices;
        auto & order = library->second.order;
        const auto it = voices.find(id);
        if (it == voices.end()) return fail(res, 404, "unknown voice_id");
        // the file stays put, deleting one over http would be a nasty way to lose a voice
        const bool saved = it->second.saved;
        voices.erase(it);
        order.erase(std::remove(order.begin(), order.end(), id), order.end());
        if (voices.empty()) m_libraries.erase(library);
        res.set_content(std::string("{\"deleted\":\"") + json_escape(id) + "\",\"file_kept\":" +
                        (saved ? "true" : "false") + "}", "application/json");
    };

    const auto register_routes = [&](const std::string & prefix, const std::string & id_pattern) {
        const auto collection = prefix + "/v1/voices";
        const auto item = collection + "/" + id_pattern;
        svr.Post(collection, create);
        svr.Get(collection, list);
        svr.Get(item, get);
        svr.Patch(item, patch);
        svr.Delete(item, remove);
    };
    register_routes("", "(.+)");
    if (gateway_enabled) register_routes(gateway_voice_route, "([A-Za-z0-9_-]{1,64})");
}

} // namespace breeze
