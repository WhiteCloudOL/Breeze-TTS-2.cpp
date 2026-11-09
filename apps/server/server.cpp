// Modified by WhiteCloudOL to authenticate HTTP and WebSocket requests.
#include "server.h"
#include "auth.h"
#include "queue.h"
#include "encode.h"
#include "openai.h"
#include "voices.h"
#include "ws.h"
#include "llm_proxy.h"
#include "ws_api.h"

#include "breeze/audio.h"
#include "breeze/generation.h"
#include "breeze/model.h"
#include "breeze/scheduler.h"

#include "httplib.h"
#include "breeze_webui_assets.h"

#ifdef _WIN32
#include <windows.h> // after httplib, it pulls in winsock2 first
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace breeze {

static std::string mmss(double s) {
    if (!(s >= 0) || s > 86399) s = 0;
    char buf[16];
    snprintf(buf, sizeof buf, "%02d:%02d", (int) s / 60, (int) s % 60);
    return buf;
}

static std::string bar(double frac, int width) {
    static const char * part[] = { " ", "\u258f", "\u258e", "\u258d", "\u258c", "\u258b", "\u258a", "\u2589" };
    if (!(frac > 0)) frac = 0;
    if (frac > 1) frac = 1;
    const double filled = frac * width;
    const int full = (int) filled;
    std::string s;
    for (int i = 0; i < full; i++) s += "\u2588";
    if (full < width) {
        s += part[(int) ((filled - full) * 8)];
        for (int i = full + 1; i < width; i++) s += " ";
    }
    return s;
}

static std::string json_quote(const std::string & s) {
    std::string o = "\"";
    for (const char c : s) {
        if (c == '"' || c == '\\') o += '\\';
        if ((unsigned char) c >= 0x20) o += c;
    }
    return o + "\"";
}

// an origin thats not on the allowlist gets an empty string, so no header goes out
static std::string cors_origin(const std::string & allowed, const httplib::Request & req) {
    if (allowed == "*") return "*";
    const std::string origin = req.get_header_value("Origin");
    if (origin.empty()) return "";
    size_t start = 0;
    while (start <= allowed.size()) {
        size_t end = allowed.find(',', start);
        if (end == std::string::npos) end = allowed.size();
        if (allowed.compare(start, end - start, origin) == 0) return origin;
        start = end + 1;
    }
    return "";
}

// one generation with the console progress bar, emit gets each decoded chunk and false stops it
static void run_generation(BreezeModel & model, MimiCodec & codec, GenScheduler & sched, const GenRequest & g, int sr,
                           bool verbose, const AudioCallback & emit) {
    GenTimings tm;
    const auto t0 = std::chrono::steady_clock::now();
    const auto elapsed = [&] {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    };
    // the model decides when to stop, so the total is only ever an estimate
    const double est = estimate_seconds(g.text);
    size_t total = 0;
    try {
        generate(model, codec, g, [&](const float * s, int n) {
            if (!emit(s, n)) return false;
            total += (size_t) n;
            const double secs = (double) total / sr, wall = elapsed();
            const double rate = wall > 0 ? secs / wall : 0;
            const double frac = est > 0 ? secs / est : 0;
            const double eta = rate > 0 ? (est > secs ? (est - secs) / rate : 0) : 0;
            // bars from several streams would just write over each other
            if (sched.parallel() > 1) return true;
            printf("\r%3.0f%%|%s| %.1f/%.1fs [%s<%s, %.1f fps, %.2fx]  ",
                   (frac < 1 ? frac : 1) * 100, bar(frac, 24).c_str(), secs, est,
                   mmss(wall).c_str(), mmss(eta).c_str(), secs * 12.5 / wall, rate);
            fflush(stdout);
            return true;
        }, &tm, &sched);
    } catch (const std::exception & e) {
        fprintf(stderr, "\ngeneration error: %s\n", e.what());
    }
    const double secs = (double) total / sr, wall = elapsed();
    printf("\r%3.0f%%|%s| %.1f/%.1fs [%s, %.1f fps, %.2fx]        \n",
           100.0, bar(1, 24).c_str(), secs, secs, mmss(wall).c_str(),
           wall > 0 ? secs * 12.5 / wall : 0.0, wall > 0 ? secs / wall : 0.0);
    printf("     %d frames in %d flushes, first audio %.0f ms\n",
           tm.frames, tm.flushes, tm.first_audio);
    if (verbose && tm.frames > 0) {
        printf("     ref %.0f  prompt %.0f  prefill %.0f ms | per frame: backbone %.2f"
               "  depth %.2f  vocoder %.2f ms\n",
               tm.encode_ref, tm.prompt, tm.prefill, tm.backbone / tm.frames,
               tm.depth / tm.frames, tm.vocoder / tm.frames);
    }
    fflush(stdout);
}

int run_server(const ServerOptions & opts) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8); // otherwise the bar glyphs and any chinese text come out as mojibake
#endif
    BearerAuth auth;
    try {
        auth = BearerAuth::load(opts.auth, opts.api_key_file);
    } catch (const std::exception & e) {
        fprintf(stderr, "authentication configuration error: %s\n", e.what());
        return 1;
    }
    BreezeModel model;
    printf("loading %s ...\n", opts.model.c_str());
    if (!model.load(opts.model, opts.use_gpu, opts.te_cpu)) {
        fprintf(stderr, "failed to load model\n");
        return 1;
    }
    printf("backend: %s, sample rate: %d\n", model.backend.name(), model.cfg.sample_rate);
    if (opts.require_cuda && std::string(model.backend.name()).find("CUDA") != 0) {
        fprintf(stderr, "A CUDA backend is required. Check the CUDA build and NVIDIA driver.\n");
        return 1;
    }
    MimiCodec codec;
    codec.init(model);
    if (opts.warmup) {
        printf("warming up ...\n");
        fflush(stdout);
        const auto tw = std::chrono::steady_clock::now();
        GenRequest base;
        base.chunk_first = opts.chunk_first;
        base.chunk_max = opts.chunk_max;
        try {
            warm_up(model, codec, base);
            printf("warmed up in %.1f s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - tw).count());
        } catch (const std::exception & e) {
            fprintf(stderr, "warm up failed: %s\n", e.what());
        }
    }

    GenScheduler sched;
    sched.start(model, opts.parallel);
    if (opts.warmup) {
        GenRequest base;
        try {
            sched.warm_up(base);
        } catch (const std::exception & e) {
            fprintf(stderr, "warm up failed: %s\n", e.what());
        }
    }
    if (opts.parallel > 1) printf("up to %d streams at once\n", opts.parallel);

    httplib::Server svr;
    svr.set_pre_routing_handler([&](const httplib::Request & req, httplib::Response & res) {
        static const std::vector<std::string> public_paths = {
            "/", "/style.css", "/app.js", "/docs", "/redoc", "/openapi.json",
            "/talk", "/talk.css", "/talk.js"
        };
        const bool public_page = (req.method == "GET" || req.method == "HEAD") &&
            std::find(public_paths.begin(), public_paths.end(), req.path) != public_paths.end();
        // CORS preflight carries no credentials and cannot run a business route.
        if (public_page || (req.method == "OPTIONS" && !opts.cors.empty()))
            return httplib::Server::HandlerResponse::Unhandled;
        std::vector<std::string> credentials;
        const auto range = req.headers.equal_range("Authorization");
        for (auto it = range.first; it != range.second; ++it) credentials.push_back(it->second);
        if (auth.accepts(credentials)) return httplib::Server::HandlerResponse::Unhandled;
        res.status = 401;
        res.set_header("WWW-Authenticate", "Bearer");
        res.set_header("Connection", "close");
        res.set_content("{\"detail\":\"Invalid or missing API key.\"}", "application/json");
        return httplib::Server::HandlerResponse::Handled;
    });
    const int sr = model.cfg.sample_rate;

    VoiceStore store;
    store.load_dir(opts.voices_dir, model.cfg.num_codebooks);
    store.add_routes(svr, model, codec, sched, opts.voices_dir);

    WsServer ws;
    const int ws_port = opts.ws_port == 0 ? opts.port + 1 : opts.ws_port;
    if (ws_port > 0) {
        const bool up = ws.start(opts.host, ws_port, [&](WsConn & c) {
            ws_connection(c, model, codec, store, sched, opts.chunk_first, opts.chunk_max, opts.split_chars);
        }, auth);
        if (up) printf("websocket on ws://%s:%d\n", opts.host.c_str(), ws_port);
        else fprintf(stderr, "could not open the websocket port %d\n", ws_port);
    }

    svr.Get("/health", [&](const httplib::Request &, httplib::Response & res) {
        res.set_content("{\"status\":\"ok\",\"sample_rate\":" + std::to_string(sr) +
                        ",\"ws_port\":" + std::to_string(ws_port > 0 ? ws_port : 0) + "}",
                        "application/json");
    });

    svr.Get("/v1/model", [&](const httplib::Request &, httplib::Response & res) {
        res.set_content("{\"model\":\"breeze-tts-2\",\"backend\":" +
                        json_quote(model.backend.name()) + ",\"sample_rate\":" +
                        std::to_string(sr) + "}", "application/json");
    });
    svr.Get("/v1/queue", [&](const httplib::Request &, httplib::Response & res) {
        const auto status = sched.queue_status();
        res.set_content("{\"active\":" + std::to_string(status.active) +
                        ",\"waiting\":" + std::to_string(status.waiting) +
                        ",\"capacity\":" + std::to_string(status.capacity) + "}", "application/json");
    });

    svr.Get("/docs", [](const httplib::Request &, httplib::Response & res) {
        res.set_content(R"HTML(<!doctype html><html><head><meta charset="utf-8">
<title>Breeze API</title>
<link rel="stylesheet" href="https://cdn.jsdelivr.net/npm/swagger-ui-dist@5/swagger-ui.css">
</head><body><div id="swagger-ui"></div>
<script src="https://cdn.jsdelivr.net/npm/swagger-ui-dist@5/swagger-ui-bundle.js"></script>
<script>SwaggerUIBundle({url:"/openapi.json",dom_id:"#swagger-ui"});</script></body></html>)HTML",
                        "text/html; charset=utf-8");
    });
    svr.Get("/redoc", [](const httplib::Request &, httplib::Response & res) {
        res.set_redirect("/docs");
    });
    svr.Get("/openapi.json", [&](const httplib::Request &, httplib::Response & res) {
        std::string schema = R"JSON({
  "openapi":"3.0.3","info":{"title":"Breeze TTS C++ API","version":"1.0"},
  "components":{"securitySchemes":{"BreezeApiKey":{"type":"http","scheme":"bearer"}}},
  "paths":{
    "/health":{"get":{"responses":{"200":{"description":"Healthy"}}}},
    "/v1/model":{"get":{"responses":{"200":{"description":"Loaded model and backend"}}}},
    "/v1/queue":{"get":{"responses":{"200":{"description":"Active slots, waiting requests and capacity"}}}},
    "/v1/voices":{"get":{"responses":{"200":{"description":"Saved voices"}}}},
    "/v1/audio/speech":{"post":{
      "requestBody":{"required":true,"content":{
        "multipart/form-data":{"schema":{"type":"object","required":["text"],"properties":{
          "text":{"type":"string"},"instruction":{"type":"string"},
          "ref_audio":{"type":"string","format":"binary"},"ref_text":{"type":"string"},
          "voice_id":{"type":"string"},"cfg_scale":{"type":"number","default":1},
          "seed":{"type":"integer","default":42}}}},
        "application/json":{"schema":{"type":"object","required":["input"],"properties":{
          "input":{"type":"string"},"instructions":{"type":"string"},
          "voice":{"type":"string"},"response_format":{"type":"string","enum":["pcm","wav","mp3"]}}}}
      }},"responses":{"200":{"description":"Streaming audio"},"401":{"description":"Invalid or missing API key"}}}}
  }
})JSON";
        if (auth.enabled()) schema.insert(schema.rfind('}'), ",\"security\":[{\"BreezeApiKey\":[]}]");
        res.set_content(schema, "application/json");
    });

    // openai clients post json to the same path, the form fields keep the native behaviour
    const auto speak_openai = [&](const httplib::Request & req, httplib::Response & res) {
        OpenAiSpeech o;
        int status = 200;
        const std::string err = parse_openai_speech(req.body, store, opts.split_chars, o, status);
        if (!err.empty()) {
            res.status = status;
            res.set_content(openai_error(err), "application/json");
            return;
        }
        const int cost = sched.cost(o.g.cfg_scale);
        if (!wait_for_slot(sched, req, res, cost)) return;
        auto slot = std::make_shared<SlotHold>(sched, cost);
        o.g.chunk_first = opts.chunk_first;
        o.g.chunk_max = opts.chunk_max;
        std::string sent, ctype;
        std::shared_ptr<AudioEncoder> enc = make_encoder(o.format, sr, sent, ctype);

        res.set_header("X-Sample-Rate", std::to_string(enc->sample_rate(sr)));
        res.set_header("Cache-Control", "no-store");
        if (o.sse) ctype = "text/event-stream";

        const char * mode = o.g.ref_frames > 0 ? (o.g.instruction != GenRequest().instruction ? "direction" : "clone")
                                               : "design";
        printf("gen  %s, %d chars, cfg %.1f, seed %d, openai %s%s%s\n", mode, (int) o.g.text.size(),
               o.g.cfg_scale, o.g.seed, sent.c_str(), sent != o.format ? " for " : "",
               sent != o.format ? o.format.c_str() : "");
        fflush(stdout);

        res.set_chunked_content_provider(
            ctype, [&model, &codec, &sched, g = o.g, sse = o.sse, enc, slot, sr, verbose = opts.verbose](
                       size_t, httplib::DataSink & sink) {
                std::string buf;
                const auto flush = [&] {
                    if (buf.empty()) return true;
                    const std::string out = sse ? sse_audio_delta(buf) : buf;
                    buf.clear();
                    return sink.write(out.data(), out.size());
                };
                enc->begin(buf);
                if (flush()) {
                    bool alive = true;
                    run_generation(model, codec, sched, g, sr, verbose, [&](const float * s, int n) {
                        enc->write(s, n, buf);
                        return alive = flush();
                    });
                    if (alive) {
                        enc->finish(buf);
                        if (flush() && sse) {
                            const std::string done = sse_audio_done();
                            sink.write(done.data(), done.size());
                        }
                    }
                }
                sink.done();
                return true;
            });
    };

    svr.Post("/v1/audio/speech", [&](const httplib::Request & req, httplib::Response & res) {
        if (req.get_header_value("Content-Type").find("json") != std::string::npos) {
            speak_openai(req, res);
            return;
        }
        const int cost = sched.cost((float) atof(field(req, "cfg_scale", "1.0").c_str()));
        if (!wait_for_slot(sched, req, res, cost)) return;
        auto slot = std::make_shared<SlotHold>(sched, cost);
        GenRequest g;
        g.text = field(req, "text", "");
        g.instruction = field(req, "instruction", "Speak clearly and naturally.");
        g.ref_text = field(req, "ref_text", "");
        g.cfg_scale = (float) atof(field(req, "cfg_scale", "1.0").c_str());
        g.seed = atoi(field(req, "seed", "42").c_str());
        g.temperature = (float) atof(field(req, "temperature", "0").c_str());
        g.top_k = atoi(field(req, "top_k", "0").c_str());
        g.top_p = (float) atof(field(req, "top_p", "0").c_str());
        g.repetition_penalty = (float) atof(field(req, "repetition_penalty", "0").c_str());
        g.max_new_tokens = atoi(field(req, "max_new_tokens", "0").c_str());
        g.split_chars = atoi(field(req, "split_chars", std::to_string(opts.split_chars)).c_str());
        g.chunk_first = opts.chunk_first;
        g.chunk_max = opts.chunk_max;
        if (req.has_file("ref_audio")) {
            const auto & f = req.get_file_value("ref_audio");
            if (!f.content.empty() && !g.ref_text.empty()) {
                // a raw upload joins the same hash cache an unnamed POST /v1/voices entry
                // does: identical clip and transcript reuse the codes instead of re-running
                // the codec (which is most of the pre-audio wait), and identical seeds give
                // byte identical audio either way
                const std::string key = voice_key(f.content, g.ref_text);
                if (!store.take(key, g.ref_codes, g.ref_frames, g.ref_text)) {
                    std::vector<float> pcm;
                    if (read_wav_buffer((const uint8_t *) f.content.data(), f.content.size(), sr, pcm) && !pcm.empty()) {
                        CachedVoice v;
                        v.text = g.ref_text;
                        const auto t0 = std::chrono::steady_clock::now();
                        sched.exec([&] { v.codes = codec.encode(pcm, v.frames); });
                        v.encode_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                        printf("ref %s: encoded %d frames in %.0f ms\n", key.c_str(), v.frames, v.encode_ms);
                        fflush(stdout);
                        g.ref_codes = v.codes;
                        g.ref_frames = v.frames;
                        store.put(key, std::move(v));
                    }
                } else {
                    printf("ref %s: cache hit, %d frames, encode skipped\n", key.c_str(), g.ref_frames);
                    fflush(stdout);
                }
            } else if (!f.content.empty()) {
                read_wav_buffer((const uint8_t *) f.content.data(), f.content.size(), sr, g.ref_audio);
            }
            // without this a bad clip quietly turns a clone into voice design
            if (!f.content.empty() && g.ref_codes.empty() && g.ref_audio.empty()) {
                res.status = 400;
                res.set_content("{\"error\":\"could not read ref_audio\"}", "application/json");
                return;
            }
        }
        const std::string vid = field(req, "voice_id", "");
        if (!vid.empty() && !store.take(vid, g.ref_codes, g.ref_frames, g.ref_text)) {
            res.status = 404;
            res.set_content("{\"error\":\"unknown voice_id\"}", "application/json");
            return;
        }
        if (g.text.empty()) {
            res.status = 400;
            res.set_content("{\"error\":\"text is required\"}", "application/json");
            return;
        }

        res.set_header("X-Sample-Rate", std::to_string(sr));
        res.set_header("X-Sample-Format", "s16le");
        res.set_header("Cache-Control", "no-store");

        const bool sent_ins = req.has_file("instruction") || req.has_param("instruction");
        const bool has_ref = !g.ref_audio.empty() || !g.ref_codes.empty();
        const char * mode = has_ref ? (sent_ins ? "direction" : "clone") : "design";
        printf("gen  %s, %d chars, cfg %.1f, seed %d\n", mode, (int) g.text.size(), g.cfg_scale, g.seed);
        fflush(stdout);

        res.set_chunked_content_provider(
            "audio/pcm",
            [&model, &codec, &sched, g, slot, sr, verbose = opts.verbose](size_t, httplib::DataSink & sink) {
                run_generation(model, codec, sched, g, sr, verbose, [&](const float * s, int n) {
                    const std::vector<uint8_t> pcm = to_pcm16(s, n);
                    return sink.write((const char *) pcm.data(), pcm.size());
                });
                sink.done();
                return true;
            });
    });

    svr.Post("/v1/audio/convert", [&](const httplib::Request & req, httplib::Response & res) {
        // this one runs for a while and doesnt share a batch, so it needs the whole gpu
        if (!wait_for_slot(sched, req, res, sched.parallel())) return;
        SlotHold all_slots(sched, sched.parallel());
        std::vector<float> src, ref;
        const auto load = [&](const char * name, std::vector<float> & out) {
            if (!req.has_file(name)) return false;
            const auto & f = req.get_file_value(name);
            return !f.content.empty() &&
                   read_wav_buffer((const uint8_t *) f.content.data(), f.content.size(), sr, out);
        };
        if (!load("source", src)) {
            res.status = 400;
            res.set_content("{\"error\":\"a source wav file is required\"}", "application/json");
            return;
        }
        std::string ref_text = field(req, "ref_text", "");
        std::vector<int> vcodes;
        int vframes = 0;
        const std::string vid = field(req, "voice_id", "");
        if (!vid.empty()) {
            if (!store.take(vid, vcodes, vframes, ref_text)) {
                res.status = 404;
                res.set_content("{\"error\":\"unknown voice_id\"}", "application/json");
                return;
            }
        } else if (!load("ref_audio", ref)) {
            res.status = 400;
            res.set_content("{\"error\":\"ref_audio or voice_id is required\"}", "application/json");
            return;
        }
        if (ref_text.empty()) {
            res.status = 400;
            res.set_content("{\"error\":\"ref_text is required\"}", "application/json");
            return;
        }

        printf("conv %.2f s source, %s\n", (double) src.size() / sr,
               vframes > 0 ? "cached reference" : "uploaded reference");
        fflush(stdout);
        try {
            ConvertOptions copt;
            copt.src_text = field(req, "text", "");
            copt.temperature = (float) atof(field(req, "temperature", "0.3").c_str());
            copt.top_k = atoi(field(req, "top_k", "1").c_str());
            copt.cfg_scale = (float) atof(field(req, "cfg_scale", "1.0").c_str());
            copt.keep_acoustic = atoi(field(req, "keep_acoustic", "0").c_str());
            copt.seed = atoi(field(req, "seed", "42").c_str());
            copt.ref_codes = vcodes;
            copt.ref_frames = vframes;
            const auto t0 = std::chrono::steady_clock::now();
            std::vector<float> audio;
            sched.exec([&] {
                int T = 0;
                const std::vector<int> codes = codec.encode(src, T);
                audio = convert_voice(model, codec, codes, T, ref, ref_text, copt);
            });
            const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            const double secs = (double) audio.size() / sr;
            printf("     %.2f s in %.2f s, %.2fx rt\n", secs, wall, wall > 0 ? secs / wall : 0.0);
            fflush(stdout);
            std::vector<uint8_t> pcm = to_pcm16(audio.data(), (int) audio.size());
            res.set_header("X-Sample-Rate", std::to_string(sr));
            res.set_header("X-Sample-Format", "s16le");
            res.set_content((const char *) pcm.data(), pcm.size(), "audio/pcm");
        } catch (const std::exception & e) {
            fprintf(stderr, "conversion error: %s\n", e.what());
            res.status = 500;
            res.set_content("{\"error\":\"conversion failed\"}", "application/json");
        }
    });

    if (opts.webui) {
        svr.Get("/", [](const httplib::Request &, httplib::Response & res) {
            res.set_content(breeze_webui::index_html, "text/html");
        });
        svr.Get("/style.css", [](const httplib::Request &, httplib::Response & res) {
            res.set_content(breeze_webui::style_css, "text/css");
        });
        svr.Get("/app.js", [](const httplib::Request &, httplib::Response & res) {
            res.set_content(breeze_webui::app_js, "application/javascript");
        });
        printf("web ui: http://%s:%d/\n", opts.host.c_str(), opts.port);
    }

    if (opts.talk) {
        svr.Get("/talk", [](const httplib::Request &, httplib::Response & res) {
            res.set_content(breeze_webui::talk_html, "text/html");
        });
        svr.Get("/talk.css", [](const httplib::Request &, httplib::Response & res) {
            res.set_content(breeze_webui::talk_css, "text/css");
        });
        svr.Get("/talk.js", [](const httplib::Request &, httplib::Response & res) {
            res.set_content(breeze_webui::talk_js, "application/javascript");
        });
        svr.Get("/talk/config", [&, ws_port, sr](const httplib::Request &, httplib::Response & res) {
            res.set_content("{\"ws_port\":" + std::to_string(ws_port) + ",\"tyger\":" + json_quote(opts.tyger) +
                                ",\"sample_rate\":" + std::to_string(sr) + "}",
                            "application/json");
        });
        if (ws_port > 0) printf("voice call demo: http://%s:%d/talk\n", opts.host.c_str(), opts.port);
        else fprintf(stderr, "the voice call demo needs the websocket, drop --ws-port -1\n");
    }
    if (!opts.llm.empty()) add_llm_proxy(svr, opts.llm);

    if (!opts.cors.empty()) {
        // post routing hits every response, errors and the chunked stream included
        svr.set_post_routing_handler([&](const httplib::Request & req, httplib::Response & res) {
            const std::string origin = cors_origin(opts.cors, req);
            if (origin.empty()) return;
            res.set_header("Access-Control-Allow-Origin", origin);
            res.set_header("Access-Control-Expose-Headers", "X-Sample-Rate, X-Sample-Format");
            if (origin != "*") res.set_header("Vary", "Origin");
        });
        // an origin off the list still gets a 204, just without the header, so the browser refuses it
        svr.Options(".*", [](const httplib::Request & req, httplib::Response & res) {
            res.set_header("Access-Control-Allow-Methods", "GET, POST, PATCH, DELETE, OPTIONS");
            res.set_header("Access-Control-Max-Age", "86400");
            const std::string want = req.get_header_value("Access-Control-Request-Headers");
            if (!want.empty()) res.set_header("Access-Control-Allow-Headers", want);
            res.status = 204;
        });
        printf("cors: %s\n", opts.cors == "*" ? "any origin" : opts.cors.c_str());
    }

    printf("listening on http://%s:%d\n", opts.host.c_str(), opts.port);
    if (!svr.listen(opts.host, opts.port)) {
        fprintf(stderr, "failed to bind %s:%d\n", opts.host.c_str(), opts.port);
        sched.stop();
        return 1;
    }
    sched.stop();
    model.free();
    return 0;
}

}
