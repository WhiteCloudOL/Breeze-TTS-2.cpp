// Added by WhiteCloudOL for multi-key HTTP and WebSocket authentication.
#include "auth.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>

namespace breeze {
namespace {

std::string trim(const std::string & s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    return s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
}

std::vector<std::string> split_keys(const std::string & text, char separator) {
    std::vector<std::string> keys;
    size_t start = 0;
    do {
        const size_t end = text.find(separator, start);
        std::string key = trim(text.substr(start, end - start));
        if (separator == ',' || !key.empty()) keys.push_back(std::move(key));
        if (end == std::string::npos) break;
        start = end + 1;
    } while (start <= text.size());
    return keys;
}

bool equal_token(const std::string & a, const std::string & b) {
    if (a.size() != b.size()) return false;
    volatile unsigned char difference = 0;
    for (size_t i = 0; i < a.size(); ++i)
        difference = static_cast<unsigned char>(difference | (a[i] ^ b[i]));
    return difference == 0;
}

std::string websocket_token(const std::string & key) {
    const char * alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string token = "breeze-api-key.";
    unsigned bits = 0;
    int count = 0;
    for (unsigned char c : key) {
        bits = (bits << 8) | c;
        count += 8;
        while (count >= 6) {
            count -= 6;
            token += alphabet[(bits >> count) & 63];
        }
    }
    if (count) token += alphabet[(bits << (6 - count)) & 63];
    return token;
}

std::string read_keys(const std::string & path) {
    std::ifstream input(std::filesystem::u8path(path), std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read the API key file.");
    std::string data((std::istreambuf_iterator<char>(input)), {});
    // Windows PowerShell can save key files as UTF-16 or UTF-32 with a BOM.
    size_t width = 1, offset = 0, low_byte = 0;
    if (data.compare(0, 4, std::string("\xff\xfe\0\0", 4)) == 0) {
        width = 4; offset = 4;
    } else if (data.compare(0, 4, std::string("\0\0\xfe\xff", 4)) == 0) {
        width = 4; offset = 4; low_byte = 3;
    } else if (data.compare(0, 2, "\xff\xfe") == 0) {
        width = 2; offset = 2;
    } else if (data.compare(0, 2, "\xfe\xff") == 0) {
        width = 2; offset = 2; low_byte = 1;
    } else if (data.compare(0, 3, "\xef\xbb\xbf") == 0) {
        offset = 3;
    }
    if ((data.size() - offset) % width)
        throw std::runtime_error("Invalid API key file encoding.");
    std::string ascii;
    for (size_t at = offset; at < data.size(); at += width) {
        for (size_t i = 0; i < width; ++i) {
            if (i != low_byte && data[at + i] != 0)
                throw std::runtime_error("API keys must contain printable ASCII without whitespace.");
        }
        ascii += data[at + low_byte];
    }
    return ascii;
}

} // namespace

BearerAuth::BearerAuth(std::vector<std::string> keys) : m_keys(std::move(keys)) {
    for (const auto & key : m_keys) {
        if (key.empty() || std::any_of(key.begin(), key.end(), [](unsigned char c) {
                return c < 33 || c > 126;
            }))
            throw std::runtime_error("API keys must contain printable ASCII without whitespace.");
        m_protocols.push_back(websocket_token(key));
    }
}

BearerAuth BearerAuth::load(bool enabled, const std::string & key_file) {
    if (!enabled) return BearerAuth();
    std::vector<std::string> keys;
    if (const char * values = std::getenv("BREEZE_API_KEYS")) {
        keys = split_keys(values, ',');
    } else if (const char * value = std::getenv("BREEZE_API_KEY")) {
        keys = {value};
    } else {
        const char * path = std::getenv("BREEZE_API_KEY_FILE");
        keys = split_keys(read_keys(path ? path : key_file), '\n');
    }
    if (keys.empty()) throw std::runtime_error("Auth mode requires at least one API key.");
    return BearerAuth(std::move(keys));
}

bool BearerAuth::accepts(const std::vector<std::string> & authorization) const {
    if (!enabled()) return true;
    if (authorization.size() != 1) return false;
    std::istringstream header(authorization.front());
    std::string scheme, token, extra;
    if (!(header >> scheme >> token) || (header >> extra)) return false;
    for (char & c : scheme) {
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    }
    if (scheme != "bearer") return false;
    bool valid = false;
    for (const auto & key : m_keys) valid |= equal_token(token, key);
    return valid;
}

bool BearerAuth::accepts_websocket(const std::vector<std::string> & authorization,
                                   const std::vector<std::string> & protocols,
                                   std::string & selected_protocol) const {
    selected_protocol.clear();
    if (!enabled() || !authorization.empty()) return accepts(authorization);
    std::vector<std::string> credentials;
    for (const auto & header : protocols) {
        for (const auto & protocol : split_keys(header, ',')) {
            if (protocol.compare(0, 15, "breeze-api-key.") == 0)
                credentials.push_back(protocol);
        }
    }
    if (credentials.size() != 1) return false;
    bool valid = false;
    for (const auto & protocol : m_protocols)
        valid |= equal_token(credentials.front(), protocol);
    if (valid) selected_protocol = credentials.front();
    return valid;
}

} // namespace breeze
