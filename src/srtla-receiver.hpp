#pragma once

#include <cstdint>
#include <memory>

namespace srtla_embed {

// In-process SRTLA receiver built from the vendored irlserver/srtla sources.
// It accepts bonded SRTLA links on a UDP port and forwards the reassembled SRT
// traffic to an SRT listener in the same process.
class EmbeddedSrtlaReceiver {
   public:
    EmbeddedSrtlaReceiver();
    ~EmbeddedSrtlaReceiver();

    EmbeddedSrtlaReceiver(const EmbeddedSrtlaReceiver&) = delete;
    EmbeddedSrtlaReceiver& operator=(const EmbeddedSrtlaReceiver&) = delete;

    bool start(uint16_t srtla_port, uint16_t srt_port);
    void stop();
    bool running() const;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace srtla_embed
