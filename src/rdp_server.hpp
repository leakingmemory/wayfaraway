#pragma once

#include <cstddef>
#include <memory>
#include <vector>

#include "server.hpp"

// FreeRDP types, kept out of the header.
struct rdp_freerdp_listener;
struct rdp_freerdp_peer;

namespace wfa {

class RdpPeer;

// Accepts RDP connections and streams the virtual output to the active peer
// using RemoteFX surface commands. Only one peer is active at a time: a new
// connection takes over the session, and every connect or disconnect locks it.
class RdpServer : public FrameSink {
public:
    explicit RdpServer(Server& server);
    ~RdpServer() override;

    RdpServer(const RdpServer&) = delete;
    RdpServer& operator=(const RdpServer&) = delete;

    bool listen();

    void on_frame(const std::byte* pixels, int width, int height, std::size_t stride,
        const pixman_region32_t* damage) override;

    Server& server() const { return m_server; }

    // Shadow copy of the output, always up to date, so a (re)activated peer
    // can be sent a full picture without re-rendering.
    const std::byte* shadow() const { return m_shadow.data(); }
    int width() const { return m_width; }
    int height() const { return m_height; }
    std::size_t stride() const { return m_stride; }

    // Called by RdpPeer.
    void peer_closed(RdpPeer* peer);

private:
    static int on_listener_event(int fd, uint32_t mask, void* data);
    static void on_reap(void* data);
    bool accept(rdp_freerdp_peer* client);

    Server& m_server;
    rdp_freerdp_listener* m_listener = nullptr;
    std::vector<wl_event_source*> m_listener_sources;

    std::unique_ptr<RdpPeer> m_peer;
    // Peers are torn down from the event loop, never from inside their own
    // FreeRDP callbacks.
    std::vector<std::unique_ptr<RdpPeer>> m_dead_peers;
    wl_event_source* m_reaper = nullptr;

    std::vector<std::byte> m_shadow;
    int m_width = 0;
    int m_height = 0;
    std::size_t m_stride = 0;
};

} // namespace wfa
