#include "anvil/notifications/email.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>

#include <netdb.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "anvil/crypto/base64url.h"

namespace anvil::notifications {
namespace {

constexpr std::string_view kMailField = "mail";
constexpr std::string_view kCrlf = "\r\n";

// RFC 2045 base64: the standard alphabet with padding, not base64url. The two
// differ in three characters and mail decoders accept only the first.
constexpr std::string_view kBase64Alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

[[nodiscard]] std::string base64_mime(std::span<const std::uint8_t> input) {
    std::string out;
    out.reserve(((input.size() + 2) / 3) * 4);
    std::size_t i = 0;
    while (i + 2 < input.size()) {
        const std::uint32_t block = (static_cast<std::uint32_t>(input[i]) << 16U) |
                                    (static_cast<std::uint32_t>(input[i + 1]) << 8U) |
                                    static_cast<std::uint32_t>(input[i + 2]);
        out.push_back(kBase64Alphabet[(block >> 18U) & 0x3FU]);
        out.push_back(kBase64Alphabet[(block >> 12U) & 0x3FU]);
        out.push_back(kBase64Alphabet[(block >> 6U) & 0x3FU]);
        out.push_back(kBase64Alphabet[block & 0x3FU]);
        i += 3;
    }
    if (i < input.size()) {
        const std::size_t remaining = input.size() - i;
        std::uint32_t block = static_cast<std::uint32_t>(input[i]) << 16U;
        if (remaining == 2) { block |= static_cast<std::uint32_t>(input[i + 1]) << 8U; }
        out.push_back(kBase64Alphabet[(block >> 18U) & 0x3FU]);
        out.push_back(kBase64Alphabet[(block >> 12U) & 0x3FU]);
        out.push_back(remaining == 2 ? kBase64Alphabet[(block >> 6U) & 0x3FU] : '=');
        out.push_back('=');
    }
    return out;
}

[[nodiscard]] std::string base64_of(std::string_view text) {
    return base64_mime({reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
}

[[nodiscard]] bool is_printable_ascii(std::string_view value) noexcept {
    return std::all_of(value.begin(), value.end(), [](char c) {
        const auto byte = static_cast<unsigned char>(c);
        return byte >= 0x20U && byte < 0x7FU;
    });
}

// RFC 5322 date: "Tue, 14 Nov 2023 22:13:20 +0000".
//
// Built by hand from UTC rather than through strftime, because strftime reads the
// process locale: a host whose locale is not C produces month and day names no
// mail client parses, and it does it only on that host.
[[nodiscard]] std::string rfc5322_date(db::TimeMs now) {
    static constexpr std::array<std::string_view, 7> kDays{"Sun", "Mon", "Tue", "Wed",
                                                           "Thu", "Fri", "Sat"};
    static constexpr std::array<std::string_view, 12> kMonths{
        "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

    const std::int64_t seconds = now.time_since_epoch().count() / 1000;
    const std::int64_t days = seconds >= 0 ? seconds / 86400 : ((seconds - 86399) / 86400);
    const std::int64_t rest = seconds - (days * 86400);

    // Civil-from-days: pure integer arithmetic on the Unix epoch, no timezone, no
    // calendar library. The same era arithmetic input/fields.h uses.
    std::int64_t z = days + 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const std::int64_t doe = z - (era * 146097);
    const std::int64_t yoe = (doe - (doe / 1460) + (doe / 36524) - (doe / 146096)) / 365;
    const std::int64_t year = yoe + (era * 400);
    const std::int64_t doy = doe - ((365 * yoe) + (yoe / 4) - (yoe / 100));
    const std::int64_t mp = ((5 * doy) + 2) / 153;
    const std::int64_t day = doy - (((153 * mp) + 2) / 5) + 1;
    const std::int64_t month = mp + (mp < 10 ? 3 : -9);
    const std::int64_t calendar_year = year + (month <= 2 ? 1 : 0);

    // 1970-01-01 was a Thursday, which is index 4.
    const std::int64_t weekday = ((days % 7) + 7 + 4) % 7;

    std::array<char, 64> buffer{};
    const int written = std::snprintf(
        buffer.data(), buffer.size(), "%s, %02d %s %04d %02d:%02d:%02d +0000",
        std::string{kDays[static_cast<std::size_t>(weekday)]}.c_str(),
        static_cast<int>(day),
        std::string{kMonths[static_cast<std::size_t>(month - 1)]}.c_str(),
        static_cast<int>(calendar_year), static_cast<int>(rest / 3600),
        static_cast<int>((rest / 60) % 60), static_cast<int>(rest % 60));
    if (written <= 0) { return "Thu, 01 Jan 1970 00:00:00 +0000"; }
    return std::string{buffer.data(), static_cast<std::size_t>(written)};
}

void append_header(std::string& out, std::string_view name, std::string_view value) {
    out.append(name).append(": ").append(value).append(kCrlf);
}

// Doubles a leading "." on every line, and normalises line endings to CRLF.
//
// DATA ends at a line containing exactly ".". A body line that starts with one
// would end the message early, and the server would then read the rest of the
// body AS SMTP COMMANDS — so anybody who can put a line into a notification could
// send mail as this server. It happens here, once, on the way out.
void append_dot_stuffed(std::string& out, std::string_view body) {
    bool at_line_start = true;
    for (std::size_t i = 0; i < body.size(); ++i) {
        const char c = body[i];
        if (c == '\r') {
            // A bare CR, a bare LF and a CRLF all become one CRLF. A body that
            // mixed them would produce lines an intermediate relay re-folds, and
            // re-folding is where a stuffed dot stops being at a line start.
            if (i + 1 < body.size() && body[i + 1] == '\n') { ++i; }
            out.append(kCrlf);
            at_line_start = true;
            continue;
        }
        if (c == '\n') {
            out.append(kCrlf);
            at_line_start = true;
            continue;
        }
        if (at_line_start && c == '.') { out.push_back('.'); }
        out.push_back(c);
        at_line_start = false;
    }
    if (!at_line_start) { out.append(kCrlf); }
}

[[nodiscard]] bool is_atext(char c) noexcept {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
        return true;
    }
    return std::string_view{"!#$%&'*+-/=?^_`{|}~."}.find(c) != std::string_view::npos;
}

[[nodiscard]] bool is_domain_char(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '-' || c == '.';
}

}  // namespace

bool header_value_is_safe(std::string_view value) noexcept {
    return std::none_of(value.begin(), value.end(), [](char c) {
        const auto byte = static_cast<unsigned char>(c);
        // Everything in C0, not only CR and LF. A NUL truncates in whatever C
        // string handling sits between here and the wire, and the rest are
        // control codes no header value has a use for.
        return byte < 0x20U || byte == 0x7FU;
    });
}

bool address_is_safe(std::string_view address) noexcept {
    // Deliberately narrower than RFC 5321 allows. The quoted-string local part and
    // the bracketed address literal are both legal and both are where this and a
    // relay can disagree about which address a message is going to.
    if (address.empty() || address.size() > 254) { return false; }
    const std::size_t at = address.find('@');
    if (at == std::string_view::npos || at == 0 || at + 1 >= address.size()) { return false; }
    if (address.find('@', at + 1) != std::string_view::npos) { return false; }

    const std::string_view local = address.substr(0, at);
    const std::string_view domain = address.substr(at + 1);
    if (local.size() > 64) { return false; }
    if (local.front() == '.' || local.back() == '.') { return false; }
    if (!std::all_of(local.begin(), local.end(), is_atext)) { return false; }

    if (domain.front() == '.' || domain.back() == '.' || domain.front() == '-') {
        return false;
    }
    if (domain.find('.') == std::string_view::npos) { return false; }
    return std::all_of(domain.begin(), domain.end(), is_domain_char);
}

std::string encode_header_value(std::string_view value) {
    // An encoded word where none is needed is legal and renders as one literally
    // in clients that do not decode it — which is most of the ones that matter
    // and none of the ones anybody tests against.
    if (is_printable_ascii(value)) { return std::string{value}; }
    return "=?UTF-8?B?" + base64_of(value) + "?=";
}

Result<std::string> build_message(const MailMessage& message, db::TimeMs now) {
    if (!address_is_safe(message.from) || !address_is_safe(message.to)) {
        return fail(ErrorCode::ValidationFailed, kMailField);
    }
    // Every value that reaches a header, checked before any of it is assembled. A
    // CRLF here is not a broken subject, it is whatever headers the attacker wrote
    // after it and then a body of their choosing.
    for (const std::string_view value :
         {message.subject, message.from_name, message.message_id, message.references}) {
        if (!header_value_is_safe(value)) { return fail(ErrorCode::ValidationFailed, kMailField); }
    }
    if (message.message_id.empty() || message.message_id.find('>') != std::string_view::npos) {
        return fail(ErrorCode::ValidationFailed, kMailField);
    }

    std::string out;
    out.reserve(512 + message.body_text.size());

    append_header(out, "Date", rfc5322_date(now));
    if (message.from_name.empty()) {
        append_header(out, "From", message.from);
    } else {
        std::string from;
        from.reserve(message.from_name.size() + message.from.size() + 8);
        from.append(encode_header_value(message.from_name)).append(" <").append(message.from);
        from.push_back('>');
        append_header(out, "From", from);
    }
    append_header(out, "To", message.to);
    append_header(out, "Subject", encode_header_value(message.subject));

    std::string id;
    id.reserve(message.message_id.size() + 2);
    id.push_back('<');
    id.append(message.message_id);
    id.push_back('>');
    // The same id for a redelivered job, because the caller derives it from the
    // notification id. That is what lets a mail client collapse a duplicate rather
    // than show it twice — the queue is at-least-once and the transport does not
    // try to make it otherwise.
    append_header(out, "Message-ID", id);
    if (!message.references.empty()) {
        std::string reference;
        reference.reserve(message.references.size() + 2);
        reference.push_back('<');
        reference.append(message.references);
        reference.push_back('>');
        append_header(out, "References", reference);
        append_header(out, "In-Reply-To", reference);
    }

    append_header(out, "MIME-Version", "1.0");
    append_header(out, "Content-Type", "text/plain; charset=UTF-8");
    // base64 rather than 8bit: a relay that does not advertise 8BITMIME is
    // entitled to mangle a non-ASCII byte, and the notification text is routinely
    // not ASCII. Quoted-printable would be smaller for Latin text and larger for
    // Arabic, which is the case this library exists to handle properly.
    append_header(out, "Content-Transfer-Encoding", "base64");
    // A notification is not a conversation. Without this, an out-of-office reply
    // arrives for every notification the account receives, addressed to whatever
    // the envelope sender is.
    append_header(out, "Auto-Submitted", "auto-generated");
    append_header(out, "X-Auto-Response-Suppress", "All");
    out.append(kCrlf);

    // Wrapped at 76 characters, which is what RFC 2045 asks for and comfortably
    // inside the 998-octet line limit a relay may otherwise fold.
    const std::string encoded = base64_of(message.body_text);
    std::string wrapped;
    wrapped.reserve(encoded.size() + (encoded.size() / 76 * 2) + 2);
    for (std::size_t i = 0; i < encoded.size(); i += 76) {
        wrapped.append(encoded, i, 76);
        wrapped.append(kCrlf);
    }
    if (encoded.empty()) { wrapped.append(kCrlf); }

    // The body is base64 and so cannot begin a line with ".", but it goes through
    // the stuffing anyway: the protection belongs to the path, not to an argument
    // about what the current encoding happens to produce.
    append_dot_stuffed(out, wrapped);
    out.append(".").append(kCrlf);

    if (out.size() > kMaxMessageBytes) { return fail(ErrorCode::PayloadTooLarge, kMailField); }
    return out;
}

DeliveryVerdict verdict_for(std::int32_t code) noexcept {
    if (code >= 200 && code < 300) { return DeliveryVerdict::Delivered; }
    // The mailbox does not exist and will not start to. Distinguished from the
    // rest of 5xx because it is the endpoint being finished rather than this
    // message being refused, and the two cost the client different things.
    if (code == 550 || code == 551 || code == 553) { return DeliveryVerdict::Gone; }
    if (code >= 500 && code < 600) { return DeliveryVerdict::Rejected; }
    // 4xx, and anything unrecognised including a connection that never answered.
    return DeliveryVerdict::Transient;
}

// --- the conversation --------------------------------------------------------

Result<std::string> SmtpSession::next_line() {
    for (;;) {
        if (const std::size_t end = pending_.find('\n'); end != std::string::npos) {
            std::string line = pending_.substr(0, end);
            pending_.erase(0, end + 1);
            if (!line.empty() && line.back() == '\r') { line.pop_back(); }
            return line;
        }
        // A server that sends an unbounded line without a newline is a memory
        // exhaustion with a network trigger.
        if (pending_.size() > kMaxReplyLineBytes) {
            return fail(ErrorCode::ServiceUnavailable, kMailField);
        }

        std::array<char, 512> buffer{};
        const Result<std::size_t> read = stream_.read_some(buffer);
        if (!read) { return read.error(); }
        // SMTP ends with QUIT, not with a silent close. Zero here is a failure in
        // every state this protocol is ever in.
        if (read.value() == 0) { return fail(ErrorCode::ServiceUnavailable, kMailField); }
        pending_.append(buffer.data(), read.value());
    }
}

Result<SmtpReply> SmtpSession::read_reply() {
    SmtpReply reply{};
    for (std::size_t line_number = 0; line_number < kMaxReplyLines; ++line_number) {
        Result<std::string> line = next_line();
        if (!line) { return line.error(); }
        const std::string text = std::move(line).value();
        if (text.size() < 3) { return fail(ErrorCode::ServiceUnavailable, kMailField); }

        std::int32_t code = 0;
        for (std::size_t i = 0; i < 3; ++i) {
            if (text[i] < '0' || text[i] > '9') {
                return fail(ErrorCode::ServiceUnavailable, kMailField);
            }
            code = (code * 10) + (text[i] - '0');
        }
        reply.code = code;
        reply.text = text.size() > 4 ? text.substr(4) : std::string{};

        // A '-' in the fourth column is a continuation. Mistaking one for a final
        // line sends the next command into the middle of an answer, and every
        // reply after that is off by one.
        if (text.size() == 3 || text[3] != '-') { return reply; }
    }
    // A multiline reply with no end.
    return fail(ErrorCode::ServiceUnavailable, kMailField);
}

Status SmtpSession::send_line(std::string_view line) {
    std::string out;
    out.reserve(line.size() + 2);
    out.append(line).append(kCrlf);
    return stream_.write_all(out);
}

Result<SmtpReply> SmtpSession::command(std::string_view line) {
    if (const Status written = send_line(line); !written) { return written.error(); }
    return read_reply();
}

Result<DeliveryVerdict> SmtpSession::send(std::string_view envelope_from,
                                          std::string_view envelope_to,
                                          std::string_view payload) {
    // Every one of these reaches the wire inside a command. A CRLF in any of them
    // would end the command and start another one of the attacker's choosing,
    // which is the same injection build_message refuses one layer up — and it is
    // refused again here rather than assumed, because this is a public entry
    // point.
    if (!address_is_safe(envelope_from) || !address_is_safe(envelope_to)) {
        return fail(ErrorCode::ValidationFailed, kMailField);
    }
    if (!header_value_is_safe(config_.ehlo_name) ||
        !header_value_is_safe(config_.credentials.username) ||
        !header_value_is_safe(config_.credentials.password)) {
        return fail(ErrorCode::ValidationFailed, kMailField);
    }

    const Result<SmtpReply> greeting = read_reply();
    if (!greeting) { return greeting.error(); }
    if (greeting.value().code != 220) { return verdict_for(greeting.value().code); }

    const Result<SmtpReply> ehlo = command(std::string{"EHLO "} + std::string{config_.ehlo_name});
    if (!ehlo) { return ehlo.error(); }
    if (verdict_for(ehlo.value().code) != DeliveryVerdict::Delivered) {
        return verdict_for(ehlo.value().code);
    }

    if (!config_.credentials.username.empty()) {
        // AUTH PLAIN: authorization-id NUL authentication-id NUL password, base64.
        // Safe to send here and nowhere else — the stream was TLS before the
        // first byte of SMTP, so there is no STARTTLS window in which this could
        // go out in cleartext.
        std::string credential;
        credential.reserve(2 + config_.credentials.username.size() +
                           config_.credentials.password.size());
        credential.push_back('\0');
        credential.append(config_.credentials.username);
        credential.push_back('\0');
        credential.append(config_.credentials.password);

        const Result<SmtpReply> auth = command("AUTH PLAIN " + base64_of(credential));
        if (!auth) { return auth.error(); }
        if (auth.value().code != 235) {
            // Rejected rather than Transient whatever the code says: a refused
            // credential is a configuration problem, and retrying it repeatedly is
            // how an account gets locked out by its own mail server.
            return DeliveryVerdict::Rejected;
        }
    }

    const Result<SmtpReply> from =
        command("MAIL FROM:<" + std::string{envelope_from} + ">");
    if (!from) { return from.error(); }
    if (verdict_for(from.value().code) != DeliveryVerdict::Delivered) {
        return verdict_for(from.value().code);
    }

    const Result<SmtpReply> to = command("RCPT TO:<" + std::string{envelope_to} + ">");
    if (!to) { return to.error(); }
    if (verdict_for(to.value().code) != DeliveryVerdict::Delivered) {
        return verdict_for(to.value().code);
    }

    const Result<SmtpReply> data = command("DATA");
    if (!data) { return data.error(); }
    if (data.value().code != 354) { return verdict_for(data.value().code); }

    // Already dot-stuffed and already terminated by build_message. Nothing between
    // here and the socket adds a line.
    if (const Status written = stream_.write_all(payload); !written) { return written.error(); }
    const Result<SmtpReply> accepted = read_reply();
    if (!accepted) { return accepted.error(); }

    // Best effort and deliberately unchecked: the message is accepted or not by
    // the reply above, and a server that drops the connection instead of
    // answering QUIT has changed nothing about that.
    static_cast<void>(send_line("QUIT"));
    return verdict_for(accepted.value().code);
}

// --- TLS ---------------------------------------------------------------------

namespace {

struct SslDeleter final {
    void operator()(SSL* ssl) const noexcept { SSL_free(ssl); }
};
struct SslCtxDeleter final {
    void operator()(SSL_CTX* ctx) const noexcept { SSL_CTX_free(ctx); }
};
struct AddrInfoDeleter final {
    void operator()(addrinfo* info) const noexcept { freeaddrinfo(info); }
};

// One file descriptor, owned. The only place a close() happens on a normal path
// (CLAUDE.md §3.3).
class Socket final {
public:
    explicit Socket(int fd) noexcept : fd_{fd} {}
    ~Socket() {
        if (fd_ >= 0) { ::close(fd_); }
    }
    Socket(Socket&& other) noexcept : fd_{other.fd_} { other.fd_ = -1; }
    Socket& operator=(Socket&&) = delete;
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    [[nodiscard]] int get() const noexcept { return fd_; }

private:
    int fd_;
};

class TlsStream final : public SmtpStream {
public:
    TlsStream(Socket socket, std::unique_ptr<SSL_CTX, SslCtxDeleter> ctx,
              std::unique_ptr<SSL, SslDeleter> ssl) noexcept
        : socket_{std::move(socket)}, ctx_{std::move(ctx)}, ssl_{std::move(ssl)} {}

    [[nodiscard]] Status write_all(std::string_view data) override {
        std::size_t sent = 0;
        while (sent < data.size()) {
            const int written = SSL_write(ssl_.get(), data.data() + sent,
                                          static_cast<int>(data.size() - sent));
            if (written <= 0) { return fail(ErrorCode::ServiceUnavailable, "smtp"); }
            sent += static_cast<std::size_t>(written);
        }
        return ok();
    }

    [[nodiscard]] Result<std::size_t> read_some(std::span<char> out) override {
        const int read = SSL_read(ssl_.get(), out.data(), static_cast<int>(out.size()));
        if (read < 0) { return fail(ErrorCode::ServiceUnavailable, "smtp"); }
        return static_cast<std::size_t>(read);
    }

private:
    // Declaration order is DESTRUCTION order reversed: the SSL is torn down
    // before the socket it is layered over, which is the only order that is not a
    // use-after-close.
    Socket                                  socket_;
    std::unique_ptr<SSL_CTX, SslCtxDeleter> ctx_;
    std::unique_ptr<SSL, SslDeleter>        ssl_;
};

[[nodiscard]] Result<Socket> connect_socket(const SmtpConfig& config) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    const std::string host{config.host};
    const std::string port = std::to_string(config.port);
    addrinfo* raw = nullptr;
    if (::getaddrinfo(host.c_str(), port.c_str(), &hints, &raw) != 0 || raw == nullptr) {
        return fail(ErrorCode::ServiceUnavailable, "smtp");
    }
    const std::unique_ptr<addrinfo, AddrInfoDeleter> resolved{raw};

    for (const addrinfo* candidate = resolved.get(); candidate != nullptr;
         candidate = candidate->ai_next) {
        Socket socket{::socket(candidate->ai_family, candidate->ai_socktype,
                               candidate->ai_protocol)};
        if (socket.get() < 0) { continue; }

        // A send and a receive timeout, both. Without them a mail server that
        // accepts the connection and then says nothing holds this job worker
        // forever, and the queue drains one worker at a time.
        timeval timeout{};
        timeout.tv_sec = config.timeout.count();
        static_cast<void>(::setsockopt(socket.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout,
                                       sizeof(timeout)));
        static_cast<void>(::setsockopt(socket.get(), SOL_SOCKET, SO_SNDTIMEO, &timeout,
                                       sizeof(timeout)));

        if (::connect(socket.get(), candidate->ai_addr, candidate->ai_addrlen) == 0) {
            return socket;
        }
    }
    return fail(ErrorCode::ServiceUnavailable, "smtp");
}

}  // namespace

Result<std::unique_ptr<SmtpStream>> connect_tls(const SmtpConfig& config) {
    if (config.host.empty()) { return fail(ErrorCode::ValidationFailed, kMailField); }

    Result<Socket> socket = connect_socket(config);
    if (!socket) { return socket.error(); }

    std::unique_ptr<SSL_CTX, SslCtxDeleter> ctx{SSL_CTX_new(TLS_client_method())};
    if (!ctx) { return fail(ErrorCode::Internal, kMailField); }
    // TLS 1.2 at the lowest. The credentials go over this connection, and 1.0 and
    // 1.1 have been deprecated for long enough that a server offering only them
    // is a server that is not being maintained.
    if (SSL_CTX_set_min_proto_version(ctx.get(), TLS1_2_VERSION) != 1) {
        return fail(ErrorCode::Internal, kMailField);
    }
    // The chain AND the hostname. A chain that verifies for some other host is a
    // chain an attacker can obtain, so verifying only the chain verifies nothing
    // useful here.
    SSL_CTX_set_verify(ctx.get(), SSL_VERIFY_PEER, nullptr);
    if (SSL_CTX_set_default_verify_paths(ctx.get()) != 1) {
        return fail(ErrorCode::Internal, kMailField);
    }

    std::unique_ptr<SSL, SslDeleter> ssl{SSL_new(ctx.get())};
    if (!ssl) { return fail(ErrorCode::Internal, kMailField); }

    const std::string host{config.host};
    // SNI, so a server hosting several domains presents the right certificate,
    // and the verification parameter, which is what actually checks the name
    // against the certificate rather than merely sending it.
    // SSL_ctrl rather than the SSL_set_tlsext_host_name macro it hides behind:
    // the macro expands to a C-style cast, which this build refuses
    // (-Wold-style-cast). Same call, spelled so the cast is one we wrote.
    if (SSL_ctrl(ssl.get(), SSL_CTRL_SET_TLSEXT_HOSTNAME, TLSEXT_NAMETYPE_host_name,
                 const_cast<char*>(host.c_str())) != 1) {
        return fail(ErrorCode::Internal, kMailField);
    }
    if (SSL_set1_host(ssl.get(), host.c_str()) != 1) {
        return fail(ErrorCode::Internal, kMailField);
    }
    if (SSL_set_fd(ssl.get(), socket.value().get()) != 1) {
        return fail(ErrorCode::Internal, kMailField);
    }
    // Implicit TLS: the handshake happens before the first byte of SMTP, so there
    // is no cleartext negotiation for anybody on the path to strip.
    if (SSL_connect(ssl.get()) != 1) { return fail(ErrorCode::ServiceUnavailable, kMailField); }
    if (SSL_get_verify_result(ssl.get()) != X509_V_OK) {
        return fail(ErrorCode::ServiceUnavailable, kMailField);
    }

    return std::unique_ptr<SmtpStream>{
        std::make_unique<TlsStream>(std::move(socket).value(), std::move(ctx), std::move(ssl))};
}

Result<DeliveryVerdict> deliver_mail(const SmtpConfig& config, const MailMessage& message,
                                     db::TimeMs now) {
    Result<std::string> payload = build_message(message, now);
    // A message that cannot be built safely is not sent, and it is the MESSAGE
    // that is wrong rather than the endpoint — so it is Rejected rather than
    // retried, and the mailbox is not blamed for it.
    if (!payload) { return DeliveryVerdict::Rejected; }

    Result<std::unique_ptr<SmtpStream>> stream = connect_tls(config);
    // A connection that could not be made says nothing about the mailbox. It is
    // transient, and the streak is what eventually decides otherwise.
    if (!stream) { return DeliveryVerdict::Transient; }

    SmtpSession session{*stream.value(), config};
    return session.send(message.from, message.to, payload.value());
}

}  // namespace anvil::notifications
