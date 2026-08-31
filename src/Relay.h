#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Config.h"
#include "Transport.h"

namespace esb {

class Registry;

// The online relay. See ARCHITECTURE.md 11.
//
// The rule this class exists to enforce: it runs on its own thread and NEVER
// touches the registry. Everything crosses through the two queues below, and
// the registry side of them is drained on the EuroScope main thread. That is
// what keeps the bridge single-threaded from a plugin author's point of view.
class Relay
{
public:
    // One value moving in either direction. Deliberately flat strings: this
    // struct crosses a thread boundary, so it owns everything it references.
    struct Message
    {
        std::string peer;        // inbound only: who published it
        std::string qualified;   // "provider/field"
        std::string callsign;    // empty for global scope
        uint32_t    type = 0;
        std::string payload;     // scalars rendered as text, blobs as hex
        uint64_t    revision = 0;
    };

    Relay();
    ~Relay();

    // `transport` is injected so tests can drive the whole path with a
    // loopback and no broker.
    void Start(const RelayConfig& config, std::unique_ptr<Transport> transport);
    void Stop();

    bool Running() const { return m_running.load(); }
    bool Connected() const { return m_connected.load(); }
    std::string LastError() const;

    // --- main-thread side -------------------------------------------------
    void PushOutbound(const Message& m);        // called from the registry
    bool PopInbound(Message* out);              // drained by Housekeeping

    uint64_t Published() const { return m_published.load(); }
    uint64_t Received() const  { return m_received.load(); }
    uint64_t Rejected() const  { return m_rejected.load(); }
    void     NoteRejected() { ++m_rejected; }

    std::vector<std::string> Peers() const;

private:
    void ThreadMain();
    bool ConnectAndSubscribe();
    void PumpOutbound();
    bool PumpInbound();
    void HandlePublish(const std::string& topic, const std::string& payload);
    void SetError(const std::string& text);

    std::string TopicFor(const Message& m) const;

    RelayConfig                m_config;
    std::unique_ptr<Transport> m_transport;

    std::thread       m_thread;
    std::atomic<bool> m_running{ false };
    std::atomic<bool> m_stop{ false };
    std::atomic<bool> m_connected{ false };

    std::atomic<uint64_t> m_published{ 0 };
    std::atomic<uint64_t> m_received{ 0 };
    std::atomic<uint64_t> m_rejected{ 0 };

    mutable std::mutex   m_outMutex;
    std::vector<Message> m_outbound;

    mutable std::mutex   m_inMutex;
    std::vector<Message> m_inbound;

    mutable std::mutex       m_peerMutex;
    std::vector<std::string> m_peers;

    mutable std::mutex m_errorMutex;
    std::string        m_lastError;

    std::vector<uint8_t> m_rx;         // partial packet buffer, thread-local
};

} // namespace esb
