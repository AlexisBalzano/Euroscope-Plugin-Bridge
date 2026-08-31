#include "Relay.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

#include "Log.h"
#include "MqttCodec.h"

namespace esb {

namespace {

const uint16_t kKeepAliveSeconds = 30;
const size_t   kMaxOutboundQueue = 4096;
const size_t   kMaxInboundQueue  = 4096;

// esb/v1/<room>/<controller>/<provider>/<field>[/<callsign>]
//
// Every value lives under its publisher's own prefix, which is what makes
// "no arbitration" work: nothing can collide, and a consumer reads the set
// (ARCHITECTURE.md 11.3).
std::string Split(const std::string& topic, std::vector<std::string>& parts)
{
    parts.clear();
    size_t start = 0;
    while (start <= topic.size())
    {
        const size_t slash = topic.find('/', start);
        if (slash == std::string::npos)
        {
            parts.push_back(topic.substr(start));
            break;
        }
        parts.push_back(topic.substr(start, slash - start));
        start = slash + 1;
    }
    return topic;
}

} // namespace

Relay::Relay() = default;

Relay::~Relay()
{
    Stop();
}

std::string Relay::LastError() const
{
    std::lock_guard<std::mutex> lock(m_errorMutex);
    return m_lastError;
}

void Relay::SetError(const std::string& text)
{
    {
        std::lock_guard<std::mutex> lock(m_errorMutex);
        m_lastError = text;
    }
    Log::Get().Write(ESB_LOG_WARN, "relay: %s", text.c_str());
}

void Relay::Start(const RelayConfig& config, std::unique_ptr<Transport> transport)
{
    Stop();

    m_config    = config;
    m_transport = std::move(transport);
    m_stop.store(false);
    m_running.store(true);

    m_thread = std::thread(&Relay::ThreadMain, this);

    Log::Get().Write(ESB_LOG_INFO, "relay started for room '%s' as '%s'",
                     m_config.room.c_str(), m_config.controller.c_str());
}

void Relay::Stop()
{
    if (!m_running.load())
        return;

    m_stop.store(true);
    if (m_thread.joinable())
        m_thread.join();

    m_running.store(false);
    m_connected.store(false);
    m_transport.reset();

    Log::Get().Write(ESB_LOG_INFO, "relay stopped");
}

std::string Relay::TopicFor(const Message& m) const
{
    // provider/field -> separate path segments, so a wildcard subscription can
    // filter on either.
    std::string key = m.qualified;
    const size_t slash = key.find('/');
    const std::string provider = slash == std::string::npos ? key : key.substr(0, slash);
    const std::string field    = slash == std::string::npos ? std::string() : key.substr(slash + 1);

    std::string topic = "esb/v1/" + m_config.room + "/" + m_config.controller +
                        "/" + provider + "/" + field;
    if (!m.callsign.empty())
        topic += "/" + m.callsign;
    return topic;
}

void Relay::PushOutbound(const Message& m)
{
    std::lock_guard<std::mutex> lock(m_outMutex);
    if (m_outbound.size() >= kMaxOutboundQueue)
    {
        // The relay is not keeping up. Dropping the oldest keeps the newest
        // state moving, which is the right trade for current-value data.
        m_outbound.erase(m_outbound.begin());
    }
    m_outbound.push_back(m);
}

bool Relay::PopInbound(Message* out)
{
    std::lock_guard<std::mutex> lock(m_inMutex);
    if (m_inbound.empty())
        return false;
    *out = m_inbound.front();
    m_inbound.erase(m_inbound.begin());
    return true;
}

std::vector<std::string> Relay::Peers() const
{
    std::lock_guard<std::mutex> lock(m_peerMutex);
    return m_peers;
}

void Relay::ThreadMain()
{
    uint32_t backoffMs = 1000;

    while (!m_stop.load())
    {
        if (!m_transport || !m_transport->Connected())
        {
            if (!ConnectAndSubscribe())
            {
                // Back off so a wrong hostname does not spin the CPU or hammer
                // a broker that is refusing us.
                for (uint32_t waited = 0; waited < backoffMs && !m_stop.load(); waited += 100)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                backoffMs = backoffMs < 30000 ? backoffMs * 2 : 30000;
                continue;
            }
            backoffMs = 1000;
        }

        PumpOutbound();

        if (!PumpInbound())
        {
            m_connected.store(false);
            if (m_transport)
                m_transport->Close();
            continue;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    if (m_transport && m_transport->Connected())
    {
        const std::vector<uint8_t> bye = mqtt::EncodeDisconnect();
        m_transport->Send(bye.data(), bye.size());
        m_transport->Close();
    }
    m_connected.store(false);
}

bool Relay::ConnectAndSubscribe()
{
    if (!m_transport)
        return false;

    std::string error;
    if (!m_transport->Connect(m_config.host, m_config.port, m_config.tls, &error))
    {
        SetError(error.empty() ? "connect failed" : error);
        return false;
    }

    const std::string clientId = "esb-" + m_config.room + "-" + m_config.controller;

    // Last Will: if we drop, the broker clears our presence for the room so
    // peers stop showing us as connected.
    mqtt::Will will;
    will.topic   = "esb/v1/" + m_config.room + "/" + m_config.controller + "/@presence";
    will.payload = "";
    will.retain  = true;

    const std::vector<uint8_t> connect = mqtt::EncodeConnect(
        clientId, m_config.username, m_config.password, kKeepAliveSeconds, &will);
    if (m_transport->Send(connect.data(), connect.size()) < 0)
    {
        SetError("could not send CONNECT");
        m_transport->Close();
        return false;
    }

    // Wait for CONNACK before publishing anything.
    m_rx.clear();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline && !m_stop.load())
    {
        uint8_t chunk[2048];
        const int got = m_transport->Recv(chunk, sizeof chunk);
        if (got < 0)
        {
            SetError("connection closed before CONNACK");
            m_transport->Close();
            return false;
        }
        if (got > 0)
            m_rx.insert(m_rx.end(), chunk, chunk + got);

        mqtt::Packet packet;
        size_t consumed = 0;
        const mqtt::DecodeResult r = mqtt::Decode(m_rx.data(), m_rx.size(), &packet, &consumed);
        if (r == mqtt::DecodeResult::Malformed)
        {
            SetError("malformed reply to CONNECT");
            m_transport->Close();
            return false;
        }
        if (r == mqtt::DecodeResult::NeedMore)
            continue;

        m_rx.erase(m_rx.begin(), m_rx.begin() + static_cast<ptrdiff_t>(consumed));

        if (packet.type != mqtt::kConnAck)
            continue;
        if (packet.returnCode != 0)
        {
            char buf[96];
            std::snprintf(buf, sizeof buf, "broker refused the connection (code %u)",
                          static_cast<unsigned>(packet.returnCode));
            SetError(buf);
            m_transport->Close();
            return false;
        }

        // Everyone in the room, ourselves included -- our own messages are
        // filtered on arrival by comparing the controller segment.
        const std::string filter = "esb/v1/" + m_config.room + "/#";
        const std::vector<uint8_t> sub = mqtt::EncodeSubscribe(1, filter);
        if (m_transport->Send(sub.data(), sub.size()) < 0)
        {
            SetError("could not send SUBSCRIBE");
            m_transport->Close();
            return false;
        }

        const std::string presenceTopic =
            "esb/v1/" + m_config.room + "/" + m_config.controller + "/@presence";
        const std::vector<uint8_t> presence =
            mqtt::EncodePublish(presenceTopic, m_config.controller, true);
        m_transport->Send(presence.data(), presence.size());

        m_connected.store(true);
        Log::Get().Write(ESB_LOG_INFO, "relay connected to %s:%u",
                         m_config.host.c_str(), static_cast<unsigned>(m_config.port));
        return true;
    }

    SetError("timed out waiting for CONNACK");
    m_transport->Close();
    return false;
}

void Relay::PumpOutbound()
{
    std::vector<Message> batch;
    {
        std::lock_guard<std::mutex> lock(m_outMutex);
        batch.swap(m_outbound);
    }

    for (const Message& m : batch)
    {
        // Retained, because a registry value is current state: a controller
        // who connects later must receive it without waiting for the next
        // change (ARCHITECTURE.md 11.2).
        char header[32];
        std::snprintf(header, sizeof header, "%u:%llu:",
                      m.type, static_cast<unsigned long long>(m.revision));

        const std::vector<uint8_t> packet =
            mqtt::EncodePublish(TopicFor(m), std::string(header) + m.payload, true);

        if (m_transport->Send(packet.data(), packet.size()) < 0)
        {
            m_connected.store(false);
            return;
        }
        ++m_published;
    }
}

bool Relay::PumpInbound()
{
    static auto lastPing = std::chrono::steady_clock::now();

    uint8_t chunk[4096];
    const int got = m_transport->Recv(chunk, sizeof chunk);
    if (got < 0)
    {
        SetError("connection lost");
        return false;
    }
    if (got > 0)
        m_rx.insert(m_rx.end(), chunk, chunk + got);

    for (;;)
    {
        mqtt::Packet packet;
        size_t consumed = 0;
        const mqtt::DecodeResult r =
            mqtt::Decode(m_rx.data(), m_rx.size(), &packet, &consumed);

        if (r == mqtt::DecodeResult::NeedMore)
            break;
        if (r == mqtt::DecodeResult::Malformed)
        {
            SetError("malformed packet from the broker");
            return false;
        }

        m_rx.erase(m_rx.begin(), m_rx.begin() + static_cast<ptrdiff_t>(consumed));

        if (packet.type == mqtt::kPublish)
            HandlePublish(packet.topic, packet.payload);
    }

    const auto now = std::chrono::steady_clock::now();
    if (now - lastPing > std::chrono::seconds(kKeepAliveSeconds / 2))
    {
        lastPing = now;
        const std::vector<uint8_t> ping = mqtt::EncodePingReq();
        if (m_transport->Send(ping.data(), ping.size()) < 0)
            return false;
    }

    return true;
}

void Relay::HandlePublish(const std::string& topic, const std::string& payload)
{
    std::vector<std::string> parts;
    Split(topic, parts);

    // esb / v1 / room / controller / provider / field [/ callsign]
    if (parts.size() < 5 || parts[0] != "esb" || parts[1] != "v1")
        return;
    if (parts[2] != m_config.room)
        return;

    const std::string& controller = parts[3];

    if (parts[4] == "@presence")
    {
        std::lock_guard<std::mutex> lock(m_peerMutex);
        auto it = std::find(m_peers.begin(), m_peers.end(), controller);
        if (payload.empty())
        {
            if (it != m_peers.end())
                m_peers.erase(it);      // Last Will fired: they dropped
        }
        else if (it == m_peers.end() && controller != m_config.controller)
        {
            m_peers.push_back(controller);
        }
        return;
    }

    // Our own retained messages come straight back on subscribe. Ignoring
    // them here is what stops a controller reading their own data as remote.
    if (controller == m_config.controller)
        return;

    if (parts.size() < 6)
        return;

    Message m;
    m.peer      = controller;
    m.qualified = parts[4] + "/" + parts[5];
    if (parts.size() >= 7)
        m.callsign = parts[6];

    // payload is "<type>:<revision>:<data>"
    const size_t firstColon = payload.find(':');
    if (firstColon == std::string::npos)
        return;
    const size_t secondColon = payload.find(':', firstColon + 1);
    if (secondColon == std::string::npos)
        return;

    m.type     = static_cast<uint32_t>(std::strtoul(payload.substr(0, firstColon).c_str(), nullptr, 10));
    m.revision = std::strtoull(
        payload.substr(firstColon + 1, secondColon - firstColon - 1).c_str(), nullptr, 10);
    m.payload  = payload.substr(secondColon + 1);

    {
        std::lock_guard<std::mutex> lock(m_inMutex);
        if (m_inbound.size() >= kMaxInboundQueue)
        {
            ++m_rejected;
            return;                    // shed rather than grow without bound
        }
        m_inbound.push_back(m);
    }
    ++m_received;
}

} // namespace esb
