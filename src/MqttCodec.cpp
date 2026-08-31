#include "MqttCodec.h"

namespace esb {
namespace mqtt {

namespace {

void PutU16(uint16_t v, std::vector<uint8_t>& out)
{
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v & 0xFF));
}

void PutString(const std::string& s, std::vector<uint8_t>& out)
{
    PutU16(static_cast<uint16_t>(s.size()), out);
    out.insert(out.end(), s.begin(), s.end());
}

// Fixed header, then the body. Remaining Length is a varint over the body,
// so the body has to be built first.
std::vector<uint8_t> Frame(Type type, uint8_t flags, const std::vector<uint8_t>& body)
{
    std::vector<uint8_t> packet;
    packet.push_back(static_cast<uint8_t>((static_cast<uint8_t>(type) << 4) | (flags & 0x0F)));
    EncodeVarInt(static_cast<uint32_t>(body.size()), packet);
    packet.insert(packet.end(), body.begin(), body.end());
    return packet;
}

} // namespace

void EncodeVarInt(uint32_t value, std::vector<uint8_t>& out)
{
    do
    {
        uint8_t byte = static_cast<uint8_t>(value % 128);
        value /= 128;
        if (value > 0)
            byte |= 0x80;
        out.push_back(byte);
    } while (value > 0);
}

bool DecodeVarInt(const uint8_t* buffer, size_t size, uint32_t* value, size_t* bytesUsed)
{
    uint32_t multiplier = 1;
    uint32_t result     = 0;
    size_t   i          = 0;

    for (;;)
    {
        if (i >= size || i >= 4)
            return false;              // incomplete, or longer than MQTT allows

        const uint8_t byte = buffer[i++];
        result += static_cast<uint32_t>(byte & 0x7F) * multiplier;

        if ((byte & 0x80) == 0)
            break;

        multiplier *= 128;
    }

    *value     = result;
    *bytesUsed = i;
    return true;
}

std::vector<uint8_t> EncodeConnect(const std::string& clientId,
                                   const std::string& username,
                                   const std::string& password,
                                   uint16_t keepAliveSeconds,
                                   const Will* will)
{
    std::vector<uint8_t> body;

    PutString("MQTT", body);
    body.push_back(0x04);              // protocol level 4 == MQTT 3.1.1

    uint8_t flags = 0x02;              // clean session
    if (will)
    {
        flags |= 0x04;                 // will flag
        if (will->retain)
            flags |= 0x20;
        // Will QoS stays 0, matching everything else we publish.
    }
    if (!username.empty())
        flags |= 0x80;
    if (!password.empty())
        flags |= 0x40;
    body.push_back(flags);

    PutU16(keepAliveSeconds, body);

    PutString(clientId, body);
    if (will)
    {
        PutString(will->topic, body);
        PutString(will->payload, body);
    }
    if (!username.empty())
        PutString(username, body);
    if (!password.empty())
        PutString(password, body);

    return Frame(kConnect, 0, body);
}

std::vector<uint8_t> EncodePublish(const std::string& topic,
                                   const std::string& payload,
                                   bool retain)
{
    std::vector<uint8_t> body;
    PutString(topic, body);
    // No packet identifier: that field exists only for QoS 1 and 2.
    body.insert(body.end(), payload.begin(), payload.end());

    return Frame(kPublish, retain ? 0x01 : 0x00, body);
}

std::vector<uint8_t> EncodeSubscribe(uint16_t packetId, const std::string& topicFilter)
{
    std::vector<uint8_t> body;
    PutU16(packetId, body);
    PutString(topicFilter, body);
    body.push_back(0x00);              // requested QoS 0

    return Frame(kSubscribe, 0x02, body);   // SUBSCRIBE requires flags 0010
}

std::vector<uint8_t> EncodePingReq()
{
    return Frame(kPingReq, 0, std::vector<uint8_t>());
}

std::vector<uint8_t> EncodeDisconnect()
{
    return Frame(kDisconnect, 0, std::vector<uint8_t>());
}

DecodeResult Decode(const uint8_t* buffer, size_t size, Packet* out, size_t* consumed)
{
    if (size < 2)
        return DecodeResult::NeedMore;

    const uint8_t  header = buffer[0];
    const uint8_t  type   = static_cast<uint8_t>(header >> 4);
    const uint8_t  flags  = static_cast<uint8_t>(header & 0x0F);

    uint32_t remaining = 0;
    size_t   varIntLen = 0;
    if (!DecodeVarInt(buffer + 1, size - 1, &remaining, &varIntLen))
    {
        // Four continuation bytes is the protocol maximum; more is malformed,
        // less just means we have not received it yet.
        return (size - 1) >= 4 ? DecodeResult::Malformed : DecodeResult::NeedMore;
    }

    const size_t headerLen = 1 + varIntLen;
    if (size < headerLen + remaining)
        return DecodeResult::NeedMore;

    const uint8_t* body    = buffer + headerLen;
    const size_t   bodyLen = remaining;

    *consumed  = headerLen + remaining;
    out->type  = static_cast<Type>(type);
    out->flags = flags;

    switch (type)
    {
    case kConnAck:
        if (bodyLen < 2)
            return DecodeResult::Malformed;
        out->returnCode = body[1];
        return DecodeResult::Ok;

    case kPublish:
    {
        if (bodyLen < 2)
            return DecodeResult::Malformed;

        const size_t topicLen = (static_cast<size_t>(body[0]) << 8) | body[1];
        if (bodyLen < 2 + topicLen)
            return DecodeResult::Malformed;

        out->topic.assign(reinterpret_cast<const char*>(body + 2), topicLen);

        size_t offset = 2 + topicLen;

        // QoS > 0 carries a packet identifier we must skip. We never subscribe
        // above QoS 0, but a broker is not obliged to be well behaved.
        const uint8_t qos = static_cast<uint8_t>((flags >> 1) & 0x03);
        if (qos > 0)
        {
            if (bodyLen < offset + 2)
                return DecodeResult::Malformed;
            offset += 2;
        }

        out->payload.assign(reinterpret_cast<const char*>(body + offset),
                            bodyLen - offset);
        out->retain = (flags & 0x01) != 0;
        return DecodeResult::Ok;
    }

    case kSubAck:
        if (bodyLen < 3)
            return DecodeResult::Malformed;
        out->packetId   = static_cast<uint16_t>((body[0] << 8) | body[1]);
        out->returnCode = body[2];
        return DecodeResult::Ok;

    case kPingResp:
        return DecodeResult::Ok;

    default:
        // Anything else is something we never asked for. Skipping it keeps the
        // stream in sync rather than desynchronising on an unexpected packet.
        return DecodeResult::Ok;
    }
}

} // namespace mqtt
} // namespace esb
