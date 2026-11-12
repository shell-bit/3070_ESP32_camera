#pragma once

#include "viewer_config.h"

namespace lcd {

enum class SetupField { Ssid, Password, Host, Token, ControlPort, StreamPort };
enum class SetupSection { None, Wifi, Camera };

class SetupController {
public:
    void begin(const ViewerConfig& config, uint32_t now);
    void discard();
    bool active() const { return active_; }
    SetupSection section() const { return section_; }
    bool selectSection(SetupSection section, uint32_t now);
    void discardSection(uint32_t now);
    bool dirty() const;
    bool networkDirty() const;
    const ViewerConfig& draft() const { return draft_; }
    bool setText(SetupField field, const char* text, uint32_t now);
    void keepSecret(SetupField field, uint32_t now);
    void setOpenNetwork(uint32_t now);
    void touch(uint32_t now);
    bool timedOut(uint32_t now) const;
    bool validate(ConfigError& error) const;
    bool save(ConfigError& error);

private:
    bool prepareCandidate(ViewerConfig& candidate, ConfigError& error) const;

    ViewerConfig original_{};
    ViewerConfig draft_{};
    uint32_t lastInput_ = 0;
    bool active_ = false;
    SetupSection section_ = SetupSection::None;
};

}
