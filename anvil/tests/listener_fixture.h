#pragma once

// The one Drogon listener a binary owns, and the way a second translation unit
// gets its handlers into it.
//
// `drogon::app()` is a process singleton whose `run()` does not return until
// `quit()`, so a process holds exactly one listener for its whole life. That is
// why listener cases have a binary of their own rather than a fixture inside
// another one (docs/16-test-plan.md phase 10) — and it is also why each FILE
// cannot boot its own: two files each constructing a listener is two `run()`
// calls, and the second one is a hard failure in whichever case happens to run
// after the first.
//
// So the boot is here, once, and every file installs its routes through a
// namespace-scope `RouteRegistrar`. Dynamic initialisation of every translation
// unit completes before `main`, and the listener boots lazily inside the first
// case that asks for the port, so by then every file's routes are in the router.
// That is the only ordering Drogon requires: a handler registered after `run()`
// is the one that 404s.
//
// The registrar list and the listener are `inline` at namespace scope rather
// than `static` in an anonymous namespace, and that distinction is load-bearing
// for the same reason tests/db_fixture.h spells out: a per-translation-unit copy
// would give the first file to ask for a port a listener holding only its own
// routes, and every other file's cases would 404 against a server that was
// running perfectly well.

#include <chrono>
#include <cstdint>
#include <future>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <array>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpClient.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/HttpTypes.h>

#include "anvil/accesscontrol/stealth.h"
#include "anvil/http/request_scope.h"
#include "anvil/http/trace_context.h"

namespace anvil::testfixture {

// A plain function pointer rather than std::function: an installer is a file's
// own free function, known at compile time, and this list is built during static
// initialisation where an allocation per entry buys nothing.
using RouteInstaller = void (*)();

[[nodiscard]] inline std::vector<RouteInstaller>& route_installers() {
    static std::vector<RouteInstaller> installers;
    return installers;
}

// Declared at namespace scope in each test file, beside the handlers it installs.
struct RouteRegistrar final {
    explicit RouteRegistrar(RouteInstaller install) { route_installers().push_back(install); }
};

// Whether the listener is ACCEPTING, which is not the same question as whether
// the port is known.
//
// Drogon fires beginning advices before `startListening()`, and the two are
// different events: `createListeners()` has already bound the socket by the time
// an advice runs — which is the only reason the kernel's ephemeral port is
// readable there at all — but nothing is calling `accept` until every advice has
// returned. A client that connects in that window is REFUSED, and the case fails
// with a transport error that looks nothing like the property it was asserting.
//
// The window is small and it was survivable while this binary held eight cases.
// It is not survivable under `ctest -j`, which gives every case its own process
// and its own boot: the loser is a different case on every run, and a suite whose
// red moves around is a suite people stop reading.
[[nodiscard]] inline bool accepts_connections(std::uint16_t port) noexcept {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { return false; }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = ::htons(port);
    address.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);

    const bool connected =
        ::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;
    ::close(fd);
    return connected;
}

class Listener final {
public:
    Listener() {
        // Process-wide, and shared by every file in the binary: an unmatched
        // route must answer the byte-identical stealth 404, or a case comparing
        // a denial against a nonexistent route compares against Drogon's own
        // page instead.
        accesscontrol::install_as_framework_404();
        // Both halves of the request-id wiring, and it is installed HERE rather
        // than in each file's installer for the reason the 404 is: it is
        // process-wide, and a binary where one file's cases carried an id and
        // another's did not would make every assertion about the header depend
        // on which file ran first.
        //
        // `TrustedPeer` rather than the default `Off`, and it changes nothing
        // for the rest of the binary: with no proxy list installed this process
        // is the edge, an edge trusts no hop, and every request here ingests
        // exactly what `Off` would — nothing. What it buys is that ONE case can
        // install loopback as a trusted proxy and observe the other side of the
        // decision, which is the only way to tell a working ingest from an
        // ingest that was never wired.
        http::install_request_scope(http::TraceIngest::TrustedPeer);
        for (const RouteInstaller install : route_installers()) { install(); }

        // A shared_ptr rather than a capture by reference: the advice outlives
        // this constructor's frame even though it fires inside it, and a
        // dangling reference across a thread boundary is the crash CLAUDE.md
        // §2.2 names as the most likely one in code built on this library.
        auto ready = std::make_shared<std::promise<std::uint16_t>>();
        std::future<std::uint16_t> port = ready->get_future();
        drogon::app().registerBeginningAdvice([ready] {
            ready->set_value(drogon::app().getListeners().at(0).toPort());
        });

        loop_ = std::thread{[] {
            // Port 0: the kernel picks a free one and getListeners reports it
            // back. Choosing a fixed port would make the suite fail on a
            // machine that happens to be using it, intermittently and for a
            // reason nobody would look for here.
            drogon::app().setThreadNum(1).addListener("127.0.0.1", 0).run();
        }};
        port_ = port.get();

        // A second within which a loopback listener either accepts or is broken.
        // Throwing rather than giving up quietly: a fixture that handed out a
        // port nothing answers on would report that as a failure of whichever
        // case happened to run first.
        constexpr int kAttempts = 500;
        for (int attempt = 0; !accepts_connections(port_); ++attempt) {
            if (attempt == kAttempts) {
                throw std::runtime_error{
                    "the test listener never began accepting connections"};
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
        }
    }

    ~Listener() {
        drogon::app().quit();
        if (loop_.joinable()) { loop_.join(); }
    }

    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

private:
    std::thread   loop_;
    std::uint16_t port_ = 0;
};

[[nodiscard]] inline std::uint16_t listener_port() {
    static const Listener listener{};
    return listener.port();
}

struct Exchange final {
    drogon::ReqResult       result;
    drogon::HttpResponsePtr response;
};

// Over a real socket, which is the whole point of this binary: the defects these
// cases exist for live between the socket and the handler, where a test that
// drives a repository, a service or a filter directly cannot see them.
[[nodiscard]] inline Exchange send(const drogon::HttpRequestPtr& req) {
    const drogon::HttpClientPtr client =
        drogon::HttpClient::newHttpClient("127.0.0.1", listener_port(), false, nullptr, false);
    auto [result, response] = client->sendRequest(req, 10.0);
    return Exchange{result, response};
}

// A WebSocket handshake, written byte for byte onto a raw socket, with
// everything the server wrote back returned raw.
//
// Not through `drogon::HttpClient`, and the reason is worth keeping: the client
// sets `Connection` itself — `Keep-Alive` or `close` — so a `Connection:
// Upgrade` added to a request is overwritten before it reaches the wire and
// `HttpServer::isWebSocket` then answers false. The request routes as an
// ordinary GET, the WebSocket route is never consulted, and a case comes back
// with the framework's 404 while appearing to have tested a handshake. A test
// that cannot express the input cannot assert anything about it.
//
// `extra_headers` is a pre-formatted block, each line CRLF-terminated, so a
// caller adds an `Origin` or a `Cookie` without this helper growing a parameter
// per header somebody might want.
//
// The whole response is returned rather than a parsed one: a refusal carries a
// body and a status, an accepted upgrade carries neither, and the cases below
// assert on the presence of headers as much as on the status line.
[[nodiscard]] inline std::string raw_handshake(std::string_view path,
                                               std::string_view extra_headers = {}) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { return {}; }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = ::htons(listener_port());
    address.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(fd);
        return {};
    }

    // An accepted upgrade leaves the connection open with nothing more to say,
    // so a read with no deadline would hang the suite on the one outcome that
    // means success.
    timeval timeout{};
    timeout.tv_sec = 3;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    std::string request;
    request.append("GET ").append(path).append(" HTTP/1.1\r\n");
    request.append("Host: 127.0.0.1\r\n");
    request.append("Upgrade: websocket\r\n");
    request.append("Connection: Upgrade\r\n");
    request.append("Sec-WebSocket-Version: 13\r\n");
    // Sixteen bytes of base64, which is what the standard fixes the key at.
    request.append("Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n");
    request.append(extra_headers);
    request.append("\r\n");

    std::size_t written = 0;
    while (written < request.size()) {
        const ssize_t n = ::send(fd, request.data() + written, request.size() - written, 0);
        if (n <= 0) {
            ::close(fd);
            return {};
        }
        written += static_cast<std::size_t>(n);
    }

    std::string response;
    std::array<char, 1024> buffer{};
    // One read is enough for a status line and a short body, and a second would
    // block for the timeout on every accepted upgrade.
    const ssize_t read_bytes = ::recv(fd, buffer.data(), buffer.size(), 0);
    if (read_bytes > 0) { response.assign(buffer.data(), static_cast<std::size_t>(read_bytes)); }
    ::close(fd);
    return response;
}

// The same handshake, but EVERYTHING the server sent, not just the first
// segment the kernel happened to deliver.
//
// `raw_handshake` reads once, and that is right for a case asserting a status
// line — but it cannot see what this binary now has to be able to assert. A
// refused upgrade used to be followed by a four-byte WebSocket close frame
// written by `WebSocketConnectionImpl`'s destructor, and whether one `recv` saw
// it was a question about TCP segmentation rather than about the server: the
// same server behaviour read 0 of 40 times or 12 of 40 times depending on
// coalescing. A property measured that way is a property nobody can assert.
//
// So this one reads until the peer goes quiet. The first read carries the
// server's deadline, because a server that is going to answer at all answers
// within it; every read after that has a short one, because anything the server
// still owes arrives in the same event-loop turn as the response and a longer
// wait would only be spent on a connection that is being kept alive.
[[nodiscard]] inline std::string raw_handshake_all(std::string_view path,
                                                   std::string_view extra_headers = {}) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { return {}; }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = ::htons(listener_port());
    address.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(fd);
        return {};
    }

    timeval timeout{};
    timeout.tv_sec = 3;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    std::string request;
    request.append("GET ").append(path).append(" HTTP/1.1\r\n");
    request.append("Host: 127.0.0.1\r\n");
    request.append("Upgrade: websocket\r\n");
    request.append("Connection: Upgrade\r\n");
    request.append("Sec-WebSocket-Version: 13\r\n");
    request.append("Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n");
    request.append(extra_headers);
    request.append("\r\n");

    std::size_t written = 0;
    while (written < request.size()) {
        const ssize_t n = ::send(fd, request.data() + written, request.size() - written, 0);
        if (n <= 0) {
            ::close(fd);
            return {};
        }
        written += static_cast<std::size_t>(n);
    }

    std::string response;
    std::array<char, 1024> buffer{};
    while (true) {
        const ssize_t read_bytes = ::recv(fd, buffer.data(), buffer.size(), 0);
        if (read_bytes <= 0) { break; }
        response.append(buffer.data(), static_cast<std::size_t>(read_bytes));
        // 250 ms of silence is the end of what the server has to say. The write
        // this exists to catch happens in the same loop iteration as the
        // response, so it is already in the socket buffer by the time the first
        // read returns — the wait is for segmentation, not for the server.
        timeout.tv_sec = 0;
        timeout.tv_usec = 250 * 1000;
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    }
    ::close(fd);
    return response;
}

// The status line alone, for a case that only cares which answer came back.
[[nodiscard]] inline std::string_view status_line(std::string_view response) noexcept {
    const std::size_t end = response.find("\r\n");
    return response.substr(0, end == std::string_view::npos ? response.size() : end);
}

[[nodiscard]] inline Exchange get(std::string_view path, std::string_view cookie_header) {
    drogon::HttpRequestPtr req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Get);
    req->setPath(std::string{path});
    if (!cookie_header.empty()) { req->addHeader("Cookie", std::string{cookie_header}); }
    return send(req);
}

}  // namespace anvil::testfixture
