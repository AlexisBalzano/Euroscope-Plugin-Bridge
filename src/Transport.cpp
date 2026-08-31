#include "Transport.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#define SECURITY_WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <security.h>
#include <schannel.h>
#include <sspi.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "secur32.lib")

namespace esb {

namespace {

const uintptr_t kInvalid = ~static_cast<uintptr_t>(0);
const int       kIoTimeoutMs = 200;

// WSAStartup once per process, undone at exit. The relay is the only user.
struct WinsockScope
{
    WinsockScope()
    {
        WSADATA data;
        ok = (WSAStartup(MAKEWORD(2, 2), &data) == 0);
    }
    ~WinsockScope()
    {
        if (ok)
            WSACleanup();
    }
    bool ok = false;
};

bool EnsureWinsock()
{
    static WinsockScope scope;
    return scope.ok;
}

void SetError(std::string* error, const char* what, int code)
{
    if (!error)
        return;
    char buf[192];
    std::snprintf(buf, sizeof buf, "%s (error %d)", what, code);
    *error = buf;
}

} // namespace

// --------------------------------------------------------------------- TCP

TcpTransport::~TcpTransport()
{
    Close();
}

bool TcpTransport::Connect(const std::string& host, uint16_t port, bool tls,
                           std::string* error)
{
    Close();

    if (!EnsureWinsock())
    {
        SetError(error, "WSAStartup failed", WSAGetLastError());
        return false;
    }

    char service[16];
    std::snprintf(service, sizeof service, "%u", static_cast<unsigned>(port));

    addrinfo hints = { 0 };
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* results = nullptr;
    const int rc = getaddrinfo(host.c_str(), service, &hints, &results);
    if (rc != 0 || !results)
    {
        SetError(error, "cannot resolve the broker hostname", rc);
        return false;
    }

    SOCKET sock = INVALID_SOCKET;
    for (addrinfo* a = results; a; a = a->ai_next)
    {
        sock = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (sock == INVALID_SOCKET)
            continue;
        if (::connect(sock, a->ai_addr, static_cast<int>(a->ai_addrlen)) == 0)
            break;
        closesocket(sock);
        sock = INVALID_SOCKET;
    }
    freeaddrinfo(results);

    if (sock == INVALID_SOCKET)
    {
        SetError(error, "cannot connect to the broker", WSAGetLastError());
        return false;
    }

    // Short timeouts: the relay thread must stay responsive to a shutdown
    // request rather than blocking in recv until the socket dies.
    DWORD timeout = kIoTimeoutMs;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof timeout);
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof timeout);

    BOOL nodelay = TRUE;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY,
               reinterpret_cast<const char*>(&nodelay), sizeof nodelay);

    m_socket = static_cast<uintptr_t>(sock);
    m_tls    = tls;

    if (tls && !TlsHandshake(host, error))
    {
        Close();
        return false;
    }

    return true;
}

void TcpTransport::Close()
{
    if (m_context)
    {
        DeleteSecurityContext(static_cast<CtxtHandle*>(m_context));
        delete static_cast<CtxtHandle*>(m_context);
        m_context = nullptr;
    }
    if (m_credentials)
    {
        FreeCredentialsHandle(static_cast<CredHandle*>(m_credentials));
        delete static_cast<CredHandle*>(m_credentials);
        m_credentials = nullptr;
    }
    if (m_socket != kInvalid)
    {
        closesocket(static_cast<SOCKET>(m_socket));
        m_socket = kInvalid;
    }

    m_tlsIncoming.clear();
    m_tlsPlain.clear();
    m_tls = false;
}

// SChannel client handshake.
//
// NOT VERIFIED AGAINST A LIVE BROKER. It follows the standard
// InitializeSecurityContext loop and validates the server certificate by
// leaving SCH_CRED_MANUAL_CRED_VALIDATION off, so a bad chain fails the
// handshake rather than silently trusting it -- but "compiles and looks
// right" is not the same as "tested", and this must be exercised against a
// real TLS broker before the relay ships. See the M5 notes in ARCHITECTURE.md.
bool TcpTransport::TlsHandshake(const std::string& host, std::string* error)
{
    SCHANNEL_CRED cred = { 0 };
    cred.dwVersion = SCHANNEL_CRED_VERSION;
    // No SCH_CRED_MANUAL_CRED_VALIDATION and no SCH_CRED_NO_SERVERNAME_CHECK:
    // the chain and the hostname are both checked by SChannel.
    cred.dwFlags   = SCH_CRED_NO_DEFAULT_CREDS | SCH_USE_STRONG_CRYPTO;

    CredHandle* credentials = new CredHandle();
    SECURITY_STATUS status = AcquireCredentialsHandleA(
        nullptr, const_cast<char*>(UNISP_NAME_A), SECPKG_CRED_OUTBOUND,
        nullptr, &cred, nullptr, nullptr, credentials, nullptr);
    if (status != SEC_E_OK)
    {
        delete credentials;
        SetError(error, "AcquireCredentialsHandle failed", static_cast<int>(status));
        return false;
    }
    m_credentials = credentials;

    CtxtHandle* context = new CtxtHandle();
    bool haveContext = false;

    const DWORD requested = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT |
                            ISC_REQ_CONFIDENTIALITY | ISC_REQ_ALLOCATE_MEMORY |
                            ISC_REQ_STREAM;

    std::vector<uint8_t> incoming;
    std::string hostname = host;

    for (int iteration = 0; iteration < 32; ++iteration)
    {
        SecBuffer inBuffers[2] = { 0 };
        inBuffers[0].BufferType = SECBUFFER_TOKEN;
        inBuffers[0].pvBuffer   = incoming.empty() ? nullptr : incoming.data();
        inBuffers[0].cbBuffer   = static_cast<unsigned long>(incoming.size());
        inBuffers[1].BufferType = SECBUFFER_EMPTY;

        SecBufferDesc inDesc = { SECBUFFER_VERSION, 2, inBuffers };

        SecBuffer outBuffers[1] = { 0 };
        outBuffers[0].BufferType = SECBUFFER_TOKEN;
        SecBufferDesc outDesc = { SECBUFFER_VERSION, 1, outBuffers };

        DWORD attributes = 0;
        status = InitializeSecurityContextA(
            credentials,
            haveContext ? context : nullptr,
            haveContext ? nullptr : const_cast<char*>(hostname.c_str()),
            requested, 0, 0,
            haveContext ? &inDesc : nullptr,
            0, haveContext ? nullptr : context,
            &outDesc, &attributes, nullptr);

        if (status == SEC_E_OK || status == SEC_I_CONTINUE_NEEDED ||
            status == SEC_E_INCOMPLETE_MESSAGE)
        {
            haveContext = true;
        }

        // Anything SChannel produced must go to the server before we read.
        if (outBuffers[0].cbBuffer && outBuffers[0].pvBuffer)
        {
            const uint8_t* p = static_cast<const uint8_t*>(outBuffers[0].pvBuffer);
            size_t left = outBuffers[0].cbBuffer;
            while (left)
            {
                const int sent = ::send(static_cast<SOCKET>(m_socket),
                                        reinterpret_cast<const char*>(p),
                                        static_cast<int>(left), 0);
                if (sent <= 0)
                {
                    FreeContextBuffer(outBuffers[0].pvBuffer);
                    delete context;
                    SetError(error, "TLS handshake send failed", WSAGetLastError());
                    return false;
                }
                p    += sent;
                left -= static_cast<size_t>(sent);
            }
            FreeContextBuffer(outBuffers[0].pvBuffer);
        }

        if (status == SEC_E_OK)
        {
            // Handshake complete. Any bytes SChannel did not consume are the
            // start of the application stream.
            if (inBuffers[1].BufferType == SECBUFFER_EXTRA && inBuffers[1].cbBuffer)
            {
                const size_t extra = inBuffers[1].cbBuffer;
                m_tlsIncoming.assign(incoming.end() - static_cast<ptrdiff_t>(extra),
                                     incoming.end());
            }

            SecPkgContext_StreamSizes sizes = { 0 };
            if (QueryContextAttributes(context, SECPKG_ATTR_STREAM_SIZES, &sizes) != SEC_E_OK)
            {
                delete context;
                SetError(error, "QueryContextAttributes failed", 0);
                return false;
            }
            m_tlsHeader  = sizes.cbHeader;
            m_tlsTrailer = sizes.cbTrailer;
            m_tlsMaxMsg  = sizes.cbMaximumMessage;

            m_context = context;
            return true;
        }

        if (status != SEC_I_CONTINUE_NEEDED && status != SEC_E_INCOMPLETE_MESSAGE)
        {
            delete context;
            SetError(error, "TLS handshake rejected", static_cast<int>(status));
            return false;
        }

        // Keep whatever SChannel could not use yet, then read more.
        if (status != SEC_E_INCOMPLETE_MESSAGE &&
            inBuffers[1].BufferType == SECBUFFER_EXTRA && inBuffers[1].cbBuffer)
        {
            const size_t extra = inBuffers[1].cbBuffer;
            std::vector<uint8_t> keep(incoming.end() - static_cast<ptrdiff_t>(extra),
                                      incoming.end());
            incoming.swap(keep);
        }
        else if (status != SEC_E_INCOMPLETE_MESSAGE)
        {
            incoming.clear();
        }

        uint8_t chunk[8192];
        const int got = ::recv(static_cast<SOCKET>(m_socket),
                               reinterpret_cast<char*>(chunk), sizeof chunk, 0);
        if (got == 0)
        {
            delete context;
            SetError(error, "broker closed the connection during the TLS handshake", 0);
            return false;
        }
        if (got < 0)
        {
            const int err = WSAGetLastError();
            if (err == WSAETIMEDOUT)
                continue;              // nothing yet; ask SChannel again
            delete context;
            SetError(error, "TLS handshake receive failed", err);
            return false;
        }
        incoming.insert(incoming.end(), chunk, chunk + got);
    }

    delete context;
    SetError(error, "TLS handshake did not complete", 0);
    return false;
}

int TcpTransport::Send(const uint8_t* data, size_t size)
{
    if (m_socket == kInvalid)
        return -1;
    if (m_tls)
        return TlsSend(data, size);

    size_t left = size;
    const uint8_t* p = data;
    while (left)
    {
        const int sent = ::send(static_cast<SOCKET>(m_socket),
                                reinterpret_cast<const char*>(p),
                                static_cast<int>(left), 0);
        if (sent <= 0)
        {
            const int err = WSAGetLastError();
            if (sent < 0 && err == WSAETIMEDOUT)
                continue;
            return -1;
        }
        p    += sent;
        left -= static_cast<size_t>(sent);
    }
    return static_cast<int>(size);
}

int TcpTransport::TlsSend(const uint8_t* data, size_t size)
{
    CtxtHandle* context = static_cast<CtxtHandle*>(m_context);
    if (!context)
        return -1;

    size_t offset = 0;
    while (offset < size)
    {
        const size_t chunk = std::min<size_t>(size - offset, m_tlsMaxMsg);

        std::vector<uint8_t> packet(m_tlsHeader + chunk + m_tlsTrailer);
        std::memcpy(packet.data() + m_tlsHeader, data + offset, chunk);

        SecBuffer buffers[4] = { 0 };
        buffers[0].BufferType = SECBUFFER_STREAM_HEADER;
        buffers[0].pvBuffer   = packet.data();
        buffers[0].cbBuffer   = m_tlsHeader;
        buffers[1].BufferType = SECBUFFER_DATA;
        buffers[1].pvBuffer   = packet.data() + m_tlsHeader;
        buffers[1].cbBuffer   = static_cast<unsigned long>(chunk);
        buffers[2].BufferType = SECBUFFER_STREAM_TRAILER;
        buffers[2].pvBuffer   = packet.data() + m_tlsHeader + chunk;
        buffers[2].cbBuffer   = m_tlsTrailer;
        buffers[3].BufferType = SECBUFFER_EMPTY;

        SecBufferDesc desc = { SECBUFFER_VERSION, 4, buffers };
        if (EncryptMessage(context, 0, &desc, 0) != SEC_E_OK)
            return -1;

        const size_t total = buffers[0].cbBuffer + buffers[1].cbBuffer + buffers[2].cbBuffer;
        size_t left = total;
        const uint8_t* p = packet.data();
        while (left)
        {
            const int sent = ::send(static_cast<SOCKET>(m_socket),
                                    reinterpret_cast<const char*>(p),
                                    static_cast<int>(left), 0);
            if (sent <= 0)
            {
                if (sent < 0 && WSAGetLastError() == WSAETIMEDOUT)
                    continue;
                return -1;
            }
            p    += sent;
            left -= static_cast<size_t>(sent);
        }

        offset += chunk;
    }

    return static_cast<int>(size);
}

int TcpTransport::Recv(uint8_t* buffer, size_t size)
{
    if (m_socket == kInvalid)
        return -1;
    if (m_tls)
        return TlsRecv(buffer, size);

    const int got = ::recv(static_cast<SOCKET>(m_socket),
                           reinterpret_cast<char*>(buffer),
                           static_cast<int>(size), 0);
    if (got > 0)
        return got;
    if (got == 0)
        return -1;                     // orderly shutdown by the broker

    return WSAGetLastError() == WSAETIMEDOUT ? 0 : -1;
}

int TcpTransport::TlsRecv(uint8_t* buffer, size_t size)
{
    CtxtHandle* context = static_cast<CtxtHandle*>(m_context);
    if (!context)
        return -1;

    for (;;)
    {
        // Hand back anything already decrypted first.
        if (!m_tlsPlain.empty())
        {
            const size_t n = std::min(size, m_tlsPlain.size());
            std::memcpy(buffer, m_tlsPlain.data(), n);
            m_tlsPlain.erase(m_tlsPlain.begin(), m_tlsPlain.begin() + static_cast<ptrdiff_t>(n));
            return static_cast<int>(n);
        }

        if (!m_tlsIncoming.empty())
        {
            SecBuffer buffers[4] = { 0 };
            buffers[0].BufferType = SECBUFFER_DATA;
            buffers[0].pvBuffer   = m_tlsIncoming.data();
            buffers[0].cbBuffer   = static_cast<unsigned long>(m_tlsIncoming.size());
            buffers[1].BufferType = SECBUFFER_EMPTY;
            buffers[2].BufferType = SECBUFFER_EMPTY;
            buffers[3].BufferType = SECBUFFER_EMPTY;

            SecBufferDesc desc = { SECBUFFER_VERSION, 4, buffers };
            const SECURITY_STATUS status = DecryptMessage(context, &desc, 0, nullptr);

            if (status == SEC_E_OK)
            {
                std::vector<uint8_t> extra;
                for (int i = 1; i < 4; ++i)
                {
                    if (buffers[i].BufferType == SECBUFFER_DATA && buffers[i].cbBuffer)
                    {
                        const uint8_t* p = static_cast<const uint8_t*>(buffers[i].pvBuffer);
                        m_tlsPlain.insert(m_tlsPlain.end(), p, p + buffers[i].cbBuffer);
                    }
                    else if (buffers[i].BufferType == SECBUFFER_EXTRA && buffers[i].cbBuffer)
                    {
                        const uint8_t* p = static_cast<const uint8_t*>(buffers[i].pvBuffer);
                        extra.assign(p, p + buffers[i].cbBuffer);
                    }
                }
                m_tlsIncoming.swap(extra);
                continue;
            }

            if (status == SEC_I_CONTEXT_EXPIRED)
                return -1;             // the peer closed the TLS session
            if (status != SEC_E_INCOMPLETE_MESSAGE)
                return -1;
            // else: fall through and read more ciphertext
        }

        uint8_t chunk[8192];
        const int got = ::recv(static_cast<SOCKET>(m_socket),
                               reinterpret_cast<char*>(chunk), sizeof chunk, 0);
        if (got == 0)
            return -1;
        if (got < 0)
            return WSAGetLastError() == WSAETIMEDOUT ? 0 : -1;

        m_tlsIncoming.insert(m_tlsIncoming.end(), chunk, chunk + got);
    }
}

} // namespace esb
