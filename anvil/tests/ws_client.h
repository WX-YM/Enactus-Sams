#pragma once

// A blocking WebSocket client over a raw socket, for the cases that hold a chat
// socket open and read what the server pushes down it.
//
// Not drogon::WebSocketClient, for two reasons. It runs on an event loop of
// its own, so a case that wants to assert "this frame arrived within five
// seconds, and that one never did" is a case written in callbacks against a
// loop it does not own. And the load case holds a thousand of these: a
// blocking socket per connection, read with poll(), is the cheapest thing that
// can, and the cheapest thing is what a load case should cost on the client
// side so that what it measures is the server.
//
// Only what the chat socket needs: one handshake, masked binary frames up,
// unmasked frames down, and the close code when the server closes.

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace anvil::testfixture {

struct WsFrame final {
    std::vector<std::uint8_t> payload;
    std::uint8_t              opcode;   // 0x2 binary, 0x8 close, 0x9 ping, 0xA pong
    // The status code of a close frame, 0 otherwise.
    std::uint16_t             close_code;
};

class WsClient final {
public:
    WsClient() = default;
    WsClient(const WsClient&) = delete;
    WsClient& operator=(const WsClient&) = delete;
    WsClient(WsClient&& other) noexcept
        : buffer_{std::move(other.buffer_)}, fd_{other.fd_}, status_{other.status_} {
        other.fd_ = -1;
    }
    WsClient& operator=(WsClient&& other) noexcept {
        if (this != &other) {
            close_fd();
            buffer_ = std::move(other.buffer_);
            fd_ = other.fd_;
            status_ = other.status_;
            other.fd_ = -1;
        }
        return *this;
    }
    ~WsClient() { close_fd(); }

    // Connects and handshakes. `headers` is a block of CRLF-terminated lines
    // (a Cookie, an Origin). Returns whether the server answered 101.
    bool open(std::uint16_t port, std::string_view path, std::string_view headers) {
        close_fd();
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) { return false; }
        const int one = 1;
        ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = ::htons(port);
        address.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        if (::connect(fd_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
            close_fd();
            return false;
        }
        std::string request;
        request.append("GET ").append(path).append(" HTTP/1.1\r\n");
        request.append("Host: 127.0.0.1\r\n");
        request.append("Upgrade: websocket\r\nConnection: Upgrade\r\n");
        request.append("Sec-WebSocket-Version: 13\r\n");
        request.append("Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n");
        request.append(headers);
        request.append("\r\n");
        if (!write_all(request)) { return false; }

        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds{10};
        std::size_t end = std::string::npos;
        while ((end = find_header_end()) == std::string::npos) {
            if (!fill(until)) { return false; }
        }
        const std::string head{buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(end)};
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(end + 4));
        status_ = head.size() >= 12 ? std::stoi(head.substr(9, 3)) : 0;
        return status_ == 101;
    }

    [[nodiscard]] int status() const noexcept { return status_; }
    [[nodiscard]] bool is_open() const noexcept { return fd_ >= 0; }

    // One masked binary frame.
    bool send_binary(std::span<const std::uint8_t> payload) { return send_frame(0x2, payload); }
    bool send_text(std::string_view text) {
        return send_frame(
            0x1, std::span<const std::uint8_t>{
                     reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
    }

    // The next frame, or nullopt at the deadline or when the peer has gone
    // without a close frame. Pings from the framework are answered and skipped.
    [[nodiscard]] std::optional<WsFrame> next(std::chrono::milliseconds timeout) {
        const auto until = std::chrono::steady_clock::now() + timeout;
        while (true) {
            if (std::optional<WsFrame> frame = parse()) {
                if (frame->opcode == 0x9) {
                    (void)send_frame(0xA, frame->payload);
                    continue;
                }
                return frame;
            }
            if (!fill(until)) { return std::nullopt; }
        }
    }

    // Whether a frame (or a close) is already readable, without blocking.
    [[nodiscard]] bool readable() const {
        if (fd_ < 0) { return false; }
        pollfd p{fd_, POLLIN, 0};
        return !buffer_.empty() || ::poll(&p, 1, 0) > 0;
    }

    [[nodiscard]] int fd() const noexcept { return fd_; }

private:
    void close_fd() noexcept {
        if (fd_ >= 0) { ::close(fd_); }
        fd_ = -1;
    }

    bool write_all(std::string_view bytes) {
        std::size_t written = 0;
        while (written < bytes.size()) {
            const ssize_t n = ::send(fd_, bytes.data() + written, bytes.size() - written,
                                     MSG_NOSIGNAL);
            if (n <= 0) { return false; }
            written += static_cast<std::size_t>(n);
        }
        return true;
    }

    bool send_frame(std::uint8_t opcode, std::span<const std::uint8_t> payload) {
        if (fd_ < 0) { return false; }
        std::string frame;
        frame.push_back(static_cast<char>(0x80 | opcode));
        // A client MUST mask. The key is fixed: masking is not a secret here,
        // it is a property the server checks.
        constexpr std::array<std::uint8_t, 4> kMask{0x11, 0x22, 0x33, 0x44};
        if (payload.size() < 126) {
            frame.push_back(static_cast<char>(0x80 | payload.size()));
        } else {
            frame.push_back(static_cast<char>(0x80 | 126));
            frame.push_back(static_cast<char>((payload.size() >> 8) & 0xFF));
            frame.push_back(static_cast<char>(payload.size() & 0xFF));
        }
        for (const std::uint8_t b : kMask) { frame.push_back(static_cast<char>(b)); }
        for (std::size_t i = 0; i < payload.size(); ++i) {
            frame.push_back(static_cast<char>(payload[i] ^ kMask[i % 4]));
        }
        return write_all(frame);
    }

    [[nodiscard]] std::size_t find_header_end() const {
        const std::string_view view{reinterpret_cast<const char*>(buffer_.data()),
                                    buffer_.size()};
        return view.find("\r\n\r\n");
    }

    // Reads whatever is there, waiting until `until`. False at the deadline or
    // when the peer closed.
    bool fill(std::chrono::steady_clock::time_point until) {
        if (fd_ < 0) { return false; }
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            until - std::chrono::steady_clock::now());
        if (left.count() <= 0) { return false; }
        pollfd p{fd_, POLLIN, 0};
        if (::poll(&p, 1, static_cast<int>(left.count())) <= 0) { return false; }
        std::array<std::uint8_t, 8192> chunk{};
        const ssize_t n = ::recv(fd_, chunk.data(), chunk.size(), 0);
        if (n <= 0) {
            close_fd();
            return false;
        }
        buffer_.insert(buffer_.end(), chunk.begin(), chunk.begin() + n);
        return true;
    }

    [[nodiscard]] std::optional<WsFrame> parse() {
        if (buffer_.size() < 2) { return std::nullopt; }
        const std::uint8_t opcode = buffer_[0] & 0x0F;
        std::size_t length = buffer_[1] & 0x7F;
        std::size_t at = 2;
        if (length == 126) {
            if (buffer_.size() < 4) { return std::nullopt; }
            length = (std::size_t{buffer_[2]} << 8) | buffer_[3];
            at = 4;
        } else if (length == 127) {
            if (buffer_.size() < 10) { return std::nullopt; }
            length = 0;
            for (std::size_t i = 0; i < 8; ++i) { length = (length << 8) | buffer_[2 + i]; }
            at = 10;
        }
        if (buffer_.size() < at + length) { return std::nullopt; }
        WsFrame frame{};
        frame.opcode = opcode;
        frame.payload.assign(buffer_.begin() + static_cast<std::ptrdiff_t>(at),
                             buffer_.begin() + static_cast<std::ptrdiff_t>(at + length));
        if (opcode == 0x8 && frame.payload.size() >= 2) {
            frame.close_code =
                static_cast<std::uint16_t>((frame.payload[0] << 8) | frame.payload[1]);
        }
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(at + length));
        return frame;
    }

    std::vector<std::uint8_t> buffer_;
    int                       fd_{-1};
    int                       status_{0};
};

}  // namespace anvil::testfixture
