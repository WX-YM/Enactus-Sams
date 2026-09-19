#pragma once

// Mail: the message, and the SMTP conversation that carries it.
//
// The two are separated because they fail differently. Building a message is a
// pure function whose mistakes are injections; the conversation is I/O whose
// mistakes are hangs, and the protocol logic in the middle is where the bugs
// actually live. The conversation therefore runs over an abstract stream, so the
// state machine is exercised against a scripted server rather than a real one.
//
// --- CRLF in a header is not a formatting bug, it is an injection ------------
//
// SMTP delimits headers with CRLF and ends them with a blank line. A subject
// carrying CRLF therefore does not produce a broken subject: it produces
// whatever headers the attacker wrote after it, and then a message body of their
// choosing. A `Bcc:` is the cheap version; a complete second message is the
// expensive one. Every header value here is refused rather than sanitised,
// because sanitising invites the question of which of several encodings of a
// newline was meant.
//
// --- dot-stuffing is the same problem one layer down -------------------------
//
// DATA is terminated by a line containing exactly ".". A body line beginning with
// "." must be doubled or the message ends early and the REST OF THE BODY is read
// by the server as SMTP commands. Anyone who can put a line into a notification
// can then send mail as this server. It is done on the way out, once, in
// build_message, so there is no path that reaches the socket without it.
//
// --- implicit TLS, and the certificate is CHECKED ----------------------------
//
// Port 465: TLS before the first byte of SMTP, rather than STARTTLS, which is
// negotiated in cleartext and can therefore be stripped by anybody on the path.
// The credentials go over this connection, so the certificate and the hostname
// are verified — a client that skips that is a client whose SMTP password is
// available to whoever is nearest the wire.
//
// --- threading --------------------------------------------------------------
//
// BLOCKING, with a network round trip per command. A job worker, never a Trantor
// loop thread and never db_pool: a mailbox that takes thirty seconds to answer
// would otherwise hold a database connection for thirty seconds.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "anvil/core/result.h"
#include "anvil/core/types.h"
#include "anvil/db/codec.h"
#include "anvil/notifications/record.h"

namespace anvil::notifications {

// RFC 5321 §4.5.3.1. A line longer than this is one an intermediate relay may
// fold or refuse, so the encoder wraps rather than emitting it.
inline constexpr std::size_t kMaxLineOctets = 998;

// What a single message may weigh. Not an SMTP limit — servers vary — but a
// bound on what one notification can turn into, so a template with a runaway
// parameter cannot become a multi-megabyte send.
inline constexpr std::size_t kMaxMessageBytes = 256 * 1024;

// A reply is at most this many lines, and each at most this long.
//
// Both are bounds on a HOSTILE OR BROKEN server, not on a well-behaved one: an
// unbounded read from a socket somebody else operates is a memory exhaustion with
// a network trigger, and a multiline reply with no end is a hang.
inline constexpr std::size_t kMaxReplyLines = 64;
inline constexpr std::size_t kMaxReplyLineBytes = 1024;

// Whether a header value may be used as written.
//
// The refusal is total: no CR, no LF, no NUL, and nothing else in the C0 range.
// Sanitising instead would invite the question of which of several spellings of a
// newline was meant, and that question has been answered wrongly by enough mail
// libraries to be worth not asking.
[[nodiscard]] bool header_value_is_safe(std::string_view value) noexcept;

// An address usable in MAIL FROM / RCPT TO and in a header.
//
// Deliberately narrower than RFC 5321 permits. The quoted-string local part and
// the bracketed address literal are both legal and neither is worth accepting:
// they are where parser disagreements between this and a relay live, and nobody's
// notification address is `"a b"@example.com`.
[[nodiscard]] bool address_is_safe(std::string_view address) noexcept;

// RFC 2047 encoded-word, base64, UTF-8 charset.
//
// Applied only when the value is not already printable ASCII — an encoded word
// where none is needed is legal and renders as one in clients that do not decode
// it, which is most of the ones that matter and none of the ones that are tested.
[[nodiscard]] std::string encode_header_value(std::string_view value);

struct MailMessage final {
    std::string_view from;         // envelope sender and From:
    std::string_view from_name;    // display name, may be empty
    std::string_view to;
    std::string_view subject;
    std::string_view body_text;
    // The Message-ID, without angle brackets. Derived from the notification id by
    // the caller, so a redelivered job produces the SAME id — which is what lets a
    // mail client collapse a duplicate rather than show it twice.
    std::string_view message_id;
    // A stable reference so a client threads related notifications together. May
    // be empty.
    std::string_view references;
};

// The complete DATA payload: headers, a blank line, the dot-stuffed body, and the
// terminating ".".
//
// ValidationFailed when any value carries CRLF or a control character, when an
// address is not usable, or when the result would exceed kMaxMessageBytes. There
// is no "best effort" version: a message that cannot be built safely is not sent.
[[nodiscard]] Result<std::string> build_message(const MailMessage& message, db::TimeMs now);

// --- the conversation --------------------------------------------------------

// A bidirectional byte stream. One implementation is TLS over a socket; the other
// is a test's scripted server.
//
// A virtual interface rather than a template because the choice is made at
// runtime from configuration, and because it is exactly one indirection per
// command on a path that is already waiting for a network round trip.
class SmtpStream {
public:
    SmtpStream() = default;
    virtual ~SmtpStream() = default;

    SmtpStream(const SmtpStream&) = delete;
    SmtpStream& operator=(const SmtpStream&) = delete;

    // Writes all of `data` or fails. A short write is retried internally;
    // a caller that had to loop would be a caller that could forget to.
    [[nodiscard]] virtual Status write_all(std::string_view data) = 0;

    // Up to `out.size()` bytes. Zero means the peer closed, which is a failure
    // for every state this protocol is ever in — SMTP ends with QUIT, not with a
    // silent close.
    [[nodiscard]] virtual Result<std::size_t> read_some(std::span<char> out) = 0;
};

struct SmtpReply final {
    // The three-digit code. 0 when the reply could not be read at all.
    std::int32_t code;
    // The final line's text, for a log. Never returned to a client: it is a third
    // party's message about our configuration and it routinely names the
    // credentials that failed.
    std::string  text;
};

// What a reply code means for the endpoint.
//
//   2xx           Delivered
//   550/551/553   Gone — the mailbox does not exist and will not start to
//   other 5xx     Rejected — permanent, and retrying cannot fix a refusal
//   4xx           Transient
//   anything else Transient, including a connection that failed outright
[[nodiscard]] DeliveryVerdict verdict_for(std::int32_t code) noexcept;

struct SmtpCredentials final {
    std::string_view username;
    std::string_view password;
};

struct SmtpConfig final {
    std::string_view host;
    // The name this client announces. A relay that checks it against a reverse
    // lookup will refuse a default, so it is required rather than invented.
    std::string_view ehlo_name;
    SmtpCredentials  credentials;
    std::uint16_t    port{465};
    std::chrono::seconds timeout{30};
};

// One message over an established stream.
//
// The stream is already TLS by the time this runs — implicit TLS, so there is no
// STARTTLS negotiation to strip and no window in which AUTH could be sent in
// cleartext. This is the state machine and nothing else, which is what makes it
// testable against a scripted server.
class SmtpSession final {
public:
    SmtpSession(SmtpStream& stream, const SmtpConfig& config) noexcept
        : stream_{stream}, config_{config} {}

    // Greeting, EHLO, AUTH, MAIL FROM, RCPT TO, DATA, QUIT.
    //
    // Returns the verdict rather than a Status, because every outcome here is
    // something the caller records against the endpoint — including success.
    [[nodiscard]] Result<DeliveryVerdict> send(std::string_view envelope_from,
                                               std::string_view envelope_to,
                                               std::string_view payload);

    // One reply, multiline handled. Exposed for the same reason the state machine
    // is: a continuation line that is mistaken for a final one is a session that
    // sends its next command into the middle of an answer.
    [[nodiscard]] Result<SmtpReply> read_reply();

private:
    [[nodiscard]] Status send_line(std::string_view line);
    [[nodiscard]] Result<SmtpReply> command(std::string_view line);
    [[nodiscard]] Result<std::string> next_line();

    SmtpStream&       stream_;
    const SmtpConfig& config_;
    // Whatever the last read pulled in past the end of a line. SMTP replies are
    // line-delimited over a stream, so a read that lands mid-line has to keep the
    // remainder — dropping it silently truncates the next reply.
    std::string       pending_;
};

// Connect to `config.host` and complete a TLS handshake before any SMTP.
//
// Verifies the certificate chain AND the hostname. Both, because a chain that
// verifies for some other host is a chain an attacker can obtain: the SMTP
// password goes over this connection.
[[nodiscard]] Result<std::unique_ptr<SmtpStream>> connect_tls(const SmtpConfig& config);

// The whole delivery: connect, converse, close. What a Transport wraps.
[[nodiscard]] Result<DeliveryVerdict> deliver_mail(const SmtpConfig& config,
                                                   const MailMessage& message,
                                                   db::TimeMs now);

}  // namespace anvil::notifications
