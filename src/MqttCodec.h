#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace esb {
namespace mqtt {

// MQTT 3.1.1, the subset the relay actually uses. See ARCHITECTURE.md 11.2 for
// why MQTT: retained messages give current-value-per-key to late joiners for
// free, wildcards give subscription filtering with no server code, and Last
// Will cleans up a controller's data when they drop.
//
// QoS 0 only. A registry value is a current-state fact, not an event: if a
// publish is lost, the next one supersedes it anyway, and coalescing already
// means we send only the latest value per key.

enum Type : uint8_t
{
    kConnect     = 1,
    kConnAck     = 2,
    kPublish     = 3,
    kSubscribe   = 8,
    kSubAck      = 9,
    kPingReq     = 12,
    kPingResp    = 13,
    kDisconnect  = 14,
};

struct Will
{
    std::string topic;
    std::string payload;
    bool        retain = true;
};

// --- encoding ------------------------------------------------------------

std::vector<uint8_t> EncodeConnect(const std::string& clientId,
                                   const std::string& username,
                                   const std::string& password,
                                   uint16_t keepAliveSeconds,
                                   const Will* will);

std::vector<uint8_t> EncodePublish(const std::string& topic,
                                   const std::string& payload,
                                   bool retain);

std::vector<uint8_t> EncodeSubscribe(uint16_t packetId,
                                     const std::string& topicFilter);

std::vector<uint8_t> EncodePingReq();
std::vector<uint8_t> EncodeDisconnect();

// --- decoding ------------------------------------------------------------

struct Packet
{
    Type        type  = kConnect;
    uint8_t     flags = 0;

    // kConnAck
    uint8_t     returnCode = 0;

    // kPublish
    std::string topic;
    std::string payload;
    bool        retain = false;

    // kSubAck
    uint16_t    packetId = 0;
};

enum class DecodeResult
{
    Ok,          // one packet decoded; `consumed` bytes taken from the buffer
    NeedMore,    // a complete packet is not in the buffer yet
    Malformed,   // protocol violation -- the caller must drop the connection
};

// Decodes at most one packet from the front of `buffer`.
DecodeResult Decode(const uint8_t* buffer, size_t size,
                    Packet* out, size_t* consumed);

// Exposed for testing: MQTT's 1-4 byte variable-length integer.
void   EncodeVarInt(uint32_t value, std::vector<uint8_t>& out);
bool   DecodeVarInt(const uint8_t* buffer, size_t size,
                    uint32_t* value, size_t* bytesUsed);

} // namespace mqtt
} // namespace esb
