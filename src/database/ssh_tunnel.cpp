#include "database/ssh_tunnel.hpp"

#include "database/db_interface.hpp"
#include "database/ssh_config_parser.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <libssh2.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

namespace {
#ifdef _WIN32
    using Socket = SOCKET;
    constexpr Socket kNoSocket = INVALID_SOCKET;
    void closeSocket(Socket s) {
        closesocket(s);
    }
    int pollSockets(pollfd* fds, size_t n, int ms) {
        return WSAPoll(fds, static_cast<ULONG>(n), ms);
    }
    void setBlocking(Socket s, bool blocking) {
        u_long nb = blocking ? 0 : 1;
        ioctlsocket(s, FIONBIO, &nb);
    }
    bool wouldBlock() {
        return WSAGetLastError() == WSAEWOULDBLOCK;
    }
#else
    using Socket = int;
    constexpr Socket kNoSocket = -1;
    void closeSocket(Socket s) {
        close(s);
    }
    int pollSockets(pollfd* fds, size_t n, int ms) {
        return poll(fds, static_cast<nfds_t>(n), ms);
    }
    void setBlocking(Socket s, bool blocking) {
        const int flags = fcntl(s, F_GETFL, 0);
        fcntl(s, F_SETFL, blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK));
    }
    bool wouldBlock() {
        return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS;
    }
#endif
#ifdef MSG_NOSIGNAL
    constexpr int kSendFlags = MSG_NOSIGNAL;
#else
    constexpr int kSendFlags = 0;
#endif

    // a write to a socket the other side closed must not kill the app (SIGPIPE)
    void noSigPipe([[maybe_unused]] Socket s) {
#ifdef SO_NOSIGPIPE
        int on = 1;
        setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
    }

    void initOnce() {
        static std::once_flag once;
        std::call_once(once, [] {
#ifdef _WIN32
            WSADATA d;
            WSAStartup(MAKEWORD(2, 2), &d);
#endif
            libssh2_init(0); // never libssh2_exit: tunnels may still close during shutdown
        });
    }

    std::string homeDir() {
        if (const char* h = std::getenv("HOME"); h && *h)
            return h;
        if (const char* h = std::getenv("USERPROFILE"); h && *h)
            return h;
        return {};
    }

    std::string expandHome(const std::string& path) {
        return path.starts_with("~/") ? homeDir() + path.substr(1) : path;
    }

    // tcp connect with a timeout, trying every address of the host
    Socket connectTo(const std::string& host, int port, int timeoutMs, std::string& error) {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        if (int rc = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res);
            rc != 0) {
            error = "Could not resolve SSH host " + host + ": " + gai_strerror(rc);
            return kNoSocket;
        }
        Socket result = kNoSocket;
        for (addrinfo* a = res; a && result == kNoSocket; a = a->ai_next) {
            Socket s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
            if (s == kNoSocket)
                continue;
            setBlocking(s, false);
            if (connect(s, a->ai_addr, static_cast<int>(a->ai_addrlen)) != 0 && !wouldBlock()) {
                closeSocket(s);
                continue;
            }
            pollfd p{};
            p.fd = s;
            p.events = POLLOUT;
            int err = 0;
            socklen_t len = sizeof(err);
            if (pollSockets(&p, 1, timeoutMs) == 1 &&
                getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len) == 0 &&
                err == 0) {
                setBlocking(s, true);
                noSigPipe(s);
                result = s;
            } else {
                closeSocket(s);
            }
        }
        freeaddrinfo(res);
        if (result == kNoSocket)
            error = "Could not reach SSH server " + host + ":" + std::to_string(port);
        return result;
    }

    std::string lastError(LIBSSH2_SESSION* session) {
        char* msg = nullptr;
        libssh2_session_last_error(session, &msg, nullptr, 0);
        return msg ? msg : "unknown error";
    }

    int knownHostKeyMask(int hostKeyType) {
        switch (hostKeyType) {
        case LIBSSH2_HOSTKEY_TYPE_RSA:
            return LIBSSH2_KNOWNHOST_KEY_SSHRSA;
        case LIBSSH2_HOSTKEY_TYPE_DSS:
            return LIBSSH2_KNOWNHOST_KEY_SSHDSS;
        case LIBSSH2_HOSTKEY_TYPE_ECDSA_256:
            return LIBSSH2_KNOWNHOST_KEY_ECDSA_256;
        case LIBSSH2_HOSTKEY_TYPE_ECDSA_384:
            return LIBSSH2_KNOWNHOST_KEY_ECDSA_384;
        case LIBSSH2_HOSTKEY_TYPE_ECDSA_521:
            return LIBSSH2_KNOWNHOST_KEY_ECDSA_521;
        case LIBSSH2_HOSTKEY_TYPE_ED25519:
            return LIBSSH2_KNOWNHOST_KEY_ED25519;
        default:
            return 0;
        }
    }

    // ssh's StrictHostKeyChecking=accept-new: a known key must match, a new host is
    // remembered. the file is read line by line (libssh2_knownhost_readfile stops at
    // the first line it can't parse) and only ever appended to
    std::string checkHostKey(LIBSSH2_SESSION* session, const std::string& host, int port) {
        std::string path = homeDir() + "/.ssh/known_hosts";
        if (const char* f = std::getenv("DEARSQL_SSH_KNOWN_HOSTS_FILE"); f && *f)
            path = f; // tests keep the real known_hosts untouched

        size_t keyLen = 0;
        int keyType = 0;
        const char* key = libssh2_session_hostkey(session, &keyLen, &keyType);
        const int mask = knownHostKeyMask(keyType);
        if (!key || !mask)
            return "The SSH server offered a host key type DearSQL does not support";

        LIBSSH2_KNOWNHOSTS* known = libssh2_knownhost_init(session);
        if (!known)
            return "Could not check the SSH host key";
        std::ifstream in(path);
        for (std::string line; std::getline(in, line);) {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (!line.empty() && line[0] != '#')
                libssh2_knownhost_readline(known, line.c_str(), line.size(),
                                           LIBSSH2_KNOWNHOST_FILE_OPENSSH);
        }
        in.close();

        const int typeMask = LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW | mask;
        const int check =
            libssh2_knownhost_checkp(known, host.c_str(), port, key, keyLen, typeMask, nullptr);
        std::string error;
        if (check == LIBSSH2_KNOWNHOST_CHECK_MISMATCH) {
            error = "The SSH host key for " + host + " has changed. If the server was " +
                    "reinstalled, remove its old entry from " + path + "; otherwise someone " +
                    "may be intercepting the connection.";
        } else if (check != LIBSSH2_KNOWNHOST_CHECK_MATCH) {
            const std::string name = port == 22 ? host : "[" + host + "]:" + std::to_string(port);
            libssh2_knownhost* entry = nullptr;
            char line[4096];
            size_t lineLen = 0;
            if (libssh2_knownhost_addc(known, name.c_str(), nullptr, key, keyLen, nullptr, 0,
                                       typeMask, &entry) == 0 &&
                libssh2_knownhost_writeline(known, entry, line, sizeof(line), &lineLen,
                                            LIBSSH2_KNOWNHOST_FILE_OPENSSH) == 0) {
                std::error_code ec;
                std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
                std::ofstream out(path, std::ios::app | std::ios::binary);
                out.write(line, static_cast<std::streamsize>(lineLen));
                if (lineLen == 0 || line[lineLen - 1] != '\n')
                    out << '\n';
                spdlog::info("ssh: added {} to {}", name, path);
            }
        }
        libssh2_knownhost_free(known);
        return error;
    }

    // keyboard-interactive servers ask for the password as a prompt
    void answerWithPassword(const char*, int, const char*, int, int numPrompts,
                            const LIBSSH2_USERAUTH_KBDINT_PROMPT*,
                            LIBSSH2_USERAUTH_KBDINT_RESPONSE* responses, void** abstract) {
        const auto* password = static_cast<const std::string*>(*abstract);
        for (int i = 0; i < numPrompts; ++i) {
            responses[i].text = strdup(password->c_str()); // libssh2 frees it
            responses[i].length = static_cast<unsigned int>(password->size());
        }
    }
} // namespace

struct SSHTunnel::Impl {
    Socket sshSocket = kNoSocket;
    Socket listener = kNoSocket;
    LIBSSH2_SESSION* session = nullptr;
    std::string password; // read by answerWithPassword through the session's abstract
    std::string remoteHost;
    int remotePort = 0;
    int localPort = 0;
    std::atomic<bool> running{false};
    std::thread thread;

    struct Channel {
        Socket client;
        LIBSSH2_CHANNEL* channel;
        std::string toRemote; // read from the client, not yet taken by the channel
        std::string toClient; // read from the channel, not yet taken by the client
        bool done = false;
    };

    ~Impl() {
        running = false;
        if (thread.joinable())
            thread.join();
        if (session) {
            libssh2_session_set_blocking(session, 1);
            libssh2_session_disconnect(session, "bye");
            libssh2_session_free(session);
        }
        if (listener != kNoSocket)
            closeSocket(listener);
        if (sshSocket != kNoSocket)
            closeSocket(sshSocket);
    }

    void closeChannel(Channel& c) {
        closeSocket(c.client);
        libssh2_session_set_blocking(session, 1);
        libssh2_channel_free(c.channel);
        libssh2_session_set_blocking(session, 0);
    }

    LIBSSH2_CHANNEL* openChannel() {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (running && std::chrono::steady_clock::now() < deadline) {
            if (LIBSSH2_CHANNEL* ch = libssh2_channel_direct_tcpip_ex(
                    session, remoteHost.c_str(), remotePort, "127.0.0.1", localPort))
                return ch;
            if (libssh2_session_last_errno(session) != LIBSSH2_ERROR_EAGAIN)
                break;
            pollfd p{};
            p.fd = sshSocket;
            p.events = POLLIN | POLLOUT;
            pollSockets(&p, 1, 50);
        }
        spdlog::warn("ssh: could not open a channel to {}:{}: {}", remoteHost, remotePort,
                     lastError(session));
        return nullptr;
    }

    // one thread owns the session (libssh2 sessions are not thread-safe) and pumps
    // every channel; pools open several connections through one tunnel
    void run() {
        libssh2_session_set_blocking(session, 0);
        libssh2_keepalive_config(session, 1, 30);
        std::vector<Channel> channels;
        std::vector<char> buf(64 * 1024);
        bool busy = false;
        while (running) {
            std::vector<pollfd> fds(2 + channels.size());
            fds[0].fd = listener;
            fds[0].events = POLLIN;
            fds[1].fd = sshSocket;
            fds[1].events = POLLIN;
            if (libssh2_session_block_directions(session) & LIBSSH2_SESSION_BLOCK_OUTBOUND)
                fds[1].events |= POLLOUT;
            for (size_t i = 0; i < channels.size(); ++i) {
                fds[2 + i].fd = channels[i].client;
                fds[2 + i].events =
                    static_cast<short>((channels[i].toRemote.empty() ? POLLIN : 0) |
                                       (channels[i].toClient.empty() ? 0 : POLLOUT));
            }
            // data libssh2 already buffered for a channel shows on no socket: go round
            // again at once while anything moved
            pollSockets(fds.data(), fds.size(), busy ? 0 : 50);
            busy = false;
            if (fds[1].revents & (POLLERR | POLLHUP)) {
                spdlog::warn("ssh: connection to the server closed");
                break;
            }

            if (fds[0].revents & POLLIN) {
                if (Socket client = accept(listener, nullptr, nullptr); client != kNoSocket) {
                    setBlocking(client, false); // a slow client must not stall the others
                    noSigPipe(client);
                    if (LIBSSH2_CHANNEL* ch = openChannel())
                        channels.push_back({client, ch});
                    else
                        closeSocket(client);
                }
            }

            for (size_t i = 0; i < channels.size(); ++i) {
                Channel& c = channels[i];
                if (fds.size() > 2 + i && (fds[2 + i].revents & (POLLIN | POLLHUP | POLLERR))) {
                    const auto n = recv(c.client, buf.data(), static_cast<int>(buf.size()), 0);
                    if (n > 0)
                        c.toRemote.assign(buf.data(), static_cast<size_t>(n));
                    else if (!(n < 0 && wouldBlock()))
                        c.done = true; // the database client hung up
                }
                while (!c.done && !c.toRemote.empty()) {
                    const auto w =
                        libssh2_channel_write(c.channel, c.toRemote.data(), c.toRemote.size());
                    if (w == LIBSSH2_ERROR_EAGAIN)
                        break;
                    if (w < 0) {
                        c.done = true;
                        break;
                    }
                    c.toRemote.erase(0, static_cast<size_t>(w));
                    busy = true;
                }
                // the channel is read only once the client took the last chunk, so a
                // client that stops reading backs up its own channel's window only
                for (;;) {
                    while (!c.done && !c.toClient.empty()) {
                        const auto s = send(c.client, c.toClient.data(),
                                            static_cast<int>(c.toClient.size()), kSendFlags);
                        if (s < 0 && wouldBlock())
                            break;
                        if (s <= 0) {
                            c.done = true;
                            break;
                        }
                        c.toClient.erase(0, static_cast<size_t>(s));
                        busy = true;
                    }
                    if (c.done || !c.toClient.empty())
                        break;
                    const auto r = libssh2_channel_read(c.channel, buf.data(), buf.size());
                    if (r == LIBSSH2_ERROR_EAGAIN)
                        break;
                    if (r <= 0) {
                        c.done = r < 0 || libssh2_channel_eof(c.channel);
                        break;
                    }
                    c.toClient.assign(buf.data(), static_cast<size_t>(r));
                    busy = true;
                }
                if (!c.done && c.toClient.empty() && libssh2_channel_eof(c.channel))
                    c.done = true;
            }
            for (auto it = channels.begin(); it != channels.end();) {
                if (it->done) {
                    closeChannel(*it);
                    it = channels.erase(it);
                } else {
                    ++it;
                }
            }

            int next = 0;
            if (int rc = libssh2_keepalive_send(session, &next);
                rc != 0 && rc != LIBSSH2_ERROR_EAGAIN) {
                spdlog::warn("ssh: keepalive failed: {}", lastError(session));
                break;
            }
        }
        for (auto& c : channels)
            closeChannel(c);
        running = false;
    }
};

SSHTunnel::SSHTunnel() = default;

SSHTunnel::~SSHTunnel() {
    stop();
}

std::pair<bool, std::string> SSHTunnel::start(const SSHConfig& sshIn, const std::string& remoteHost,
                                              int remotePort) {
    if (isRunning())
        return {true, ""};
    stop();
    initOnce();
    remoteHost_ = remoteHost;
    remotePort_ = remotePort;

    // ~/.ssh/config aliases keep working as they did with the ssh command
    SSHConfig ssh = sshIn;
    for (const auto& entry : parseSSHConfig()) {
        if (entry.alias != ssh.host)
            continue;
        if (!entry.hostname.empty())
            ssh.host = entry.hostname;
        if (ssh.port == 22)
            ssh.port = entry.port;
        if (ssh.username.empty())
            ssh.username = entry.user;
        if (ssh.authMethod == SSHAuthMethod::PrivateKey && ssh.privateKeyPath.empty())
            ssh.privateKeyPath = entry.identityFile;
        break;
    }

    auto impl = std::make_unique<Impl>();
    impl->remoteHost = remoteHost;
    impl->remotePort = remotePort;
    impl->password = ssh.password;

    std::string error;
    impl->sshSocket = connectTo(ssh.host, ssh.port, 10000, error);
    if (impl->sshSocket == kNoSocket)
        return {false, error};

    impl->session = libssh2_session_init_ex(nullptr, nullptr, nullptr, &impl->password);
    if (!impl->session)
        return {false, "Could not start an SSH session"};
    libssh2_session_set_blocking(impl->session, 1);
    libssh2_session_set_timeout(impl->session, 15000);
    if (libssh2_session_handshake(impl->session, impl->sshSocket) != 0)
        return {false, "SSH handshake with " + ssh.host + " failed: " + lastError(impl->session)};

    if (std::string hostKeyError = checkHostKey(impl->session, ssh.host, ssh.port);
        !hostKeyError.empty())
        return {false, hostKeyError};

    const auto userLen = static_cast<unsigned int>(ssh.username.size());
    const char* methods = libssh2_userauth_list(impl->session, ssh.username.c_str(), userLen);
    const std::string offered = methods ? methods : "";
    if (!libssh2_userauth_authenticated(impl->session)) {
        int rc = -1;
        if (ssh.authMethod == SSHAuthMethod::PrivateKey) {
            if (ssh.privateKeyPath.empty())
                return {false, "Private key path is required"};
            const std::string keyPath = expandHome(ssh.privateKeyPath);
            if (!std::filesystem::exists(keyPath))
                return {false, "Private key not found: " + keyPath};
            rc = libssh2_userauth_publickey_fromfile_ex(
                impl->session, ssh.username.c_str(), userLen, nullptr, keyPath.c_str(),
                ssh.password.empty() ? nullptr : ssh.password.c_str());
        } else {
            if (offered.find("password") != std::string::npos)
                rc = libssh2_userauth_password_ex(
                    impl->session, ssh.username.c_str(), userLen, ssh.password.c_str(),
                    static_cast<unsigned int>(ssh.password.size()), nullptr);
            if (rc != 0 && offered.find("keyboard-interactive") != std::string::npos)
                rc = libssh2_userauth_keyboard_interactive_ex(impl->session, ssh.username.c_str(),
                                                              userLen, answerWithPassword);
        }
        if (rc != 0)
            return {false, "SSH authentication failed for " + ssh.username + "@" + ssh.host +
                               " (server accepts: " + (offered.empty() ? "none" : offered) +
                               "): " + lastError(impl->session)};
    }

    // loopback only, port picked by the os
    impl->listener = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof(addr);
    if (impl->listener == kNoSocket ||
        bind(impl->listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        listen(impl->listener, 16) != 0 ||
        getsockname(impl->listener, reinterpret_cast<sockaddr*>(&addr), &len) != 0)
        return {false, "No available local port for SSH tunnel"};
    setBlocking(impl->listener, false);
    impl->localPort = ntohs(addr.sin_port);

    impl->running = true;
    impl->thread = std::thread([p = impl.get()] { p->run(); });
    localPort_ = impl->localPort;
    impl_ = std::move(impl);
    spdlog::info("SSH tunnel 127.0.0.1:{} -> {}:{} via {}", localPort_, remoteHost, remotePort,
                 ssh.host);
    return {true, ""};
}

void SSHTunnel::stop() {
    if (impl_)
        spdlog::info("SSH tunnel stopped (127.0.0.1:{})", localPort_);
    impl_.reset(); // joins the forwarding thread (it wakes every 50ms)
    localPort_ = 0;
}

bool SSHTunnel::isRunning() const {
    return impl_ && impl_->running;
}
