#pragma once

#include <cstdint>


#include <string>
#include <vector>

namespace esb {

// A byte pipe for the relay thread. Abstracted for one concrete reason: it
// lets the entire relay path -- coalescing, queues, codec, remote view,
// inbound validation -- be exercised in tests without a broker, and those are
// the parts that can corrupt the registry or leak data.
class Transport
{
public:
    virtual ~Transport() = default;

    virtual bool Connect(const std::string& host, uint16_t port, bool tls,
                         std::string* error) = 0;

    // Blocking-ish with a short timeout. Returns bytes moved, 0 on timeout,
    // -1 on a connection that is now dead.
    virtual int  Send(const uint8_t* data, size_t size) = 0;
    virtual int  Recv(uint8_t* buffer, size_t size) = 0;

    virtual void Close() = 0;
    virtual bool Connected() const = 0;
};

// Real sockets. TLS is SChannel; see the note in Transport.cpp about what is
// and is not verified.
class TcpTransport : public Transport
{
public:
    TcpTransport() = default;
    ~TcpTransport() override;

    bool Connect(const std::string& host, uint16_t port, bool tls,
                 std::string* error) override;
    int  Send(const uint8_t* data, size_t size) override;
    int  Recv(uint8_t* buffer, size_t size) override;
    void Close() override;
    bool Connected() const override { return m_socket != ~static_cast<uintptr_t>(0); }

private:
    bool     TlsHandshake(const std::string& host, std::string* error);
    int      TlsSend(const uint8_t* data, size_t size);
    int      TlsRecv(uint8_t* buffer, size_t size);

    uintptr_t m_socket = ~static_cast<uintptr_t>(0);   // INVALID_SOCKET
    bool      m_tls    = false;

    // SChannel state, kept as void* so this header pulls in no Windows
    // security headers for every translation unit that touches a Transport.
    void*                m_credentials = nullptr;   // CredHandle*
    void*                m_context     = nullptr;   // CtxtHandle*
    std::vector<uint8_t> m_tlsIncoming;             // ciphertext not yet decrypted
    std::vector<uint8_t> m_tlsPlain;                // plaintext not yet consumed
    uint32_t             m_tlsHeader   = 0;
    uint32_t             m_tlsTrailer  = 0;
    uint32_t             m_tlsMaxMsg   = 0;
};

} // namespace esb
