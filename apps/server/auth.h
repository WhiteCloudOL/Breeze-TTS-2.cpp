// Added by WhiteCloudOL for multi-key HTTP and WebSocket authentication.
#pragma once

#include <string>
#include <vector>

namespace breeze {

class BearerAuth {
public:
    static BearerAuth load(bool enabled, const std::string & key_file);
    explicit BearerAuth(std::vector<std::string> keys = {});
    bool enabled() const { return !m_keys.empty(); }
    bool accepts(const std::vector<std::string> & authorization) const;
    bool accepts_websocket(const std::vector<std::string> & authorization,
                           const std::vector<std::string> & protocols,
                           std::string & selected_protocol) const;

private:
    std::vector<std::string> m_keys;
    std::vector<std::string> m_protocols;
};

} // namespace breeze
