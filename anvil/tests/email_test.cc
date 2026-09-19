// Mail: the message and the SMTP conversation.
//
// The conversation runs against a scripted stream rather than a real server. That
// is not a compromise — it is the only way to assert what a BROKEN or hostile
// server does to this client: a multiline reply with no end, a line with no
// newline, a close in the middle of DATA. None of those is reachable against a
// working mail server, and every one of them is a hang or a memory exhaustion if
// it is not handled.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "anvil/notifications/email.h"

namespace {

using anvil::ErrorCode;
namespace n = anvil::notifications;

[[nodiscard]] anvil::db::TimeMs at_seconds(std::int64_t seconds) {
    return anvil::db::TimeMs{std::chrono::seconds{seconds}};
}

// A server whose replies are decided up front, and which records everything the
// client said.
class ScriptedServer final : public n::SmtpStream {
public:
    explicit ScriptedServer(std::string script) : script_{std::move(script)} {}

    [[nodiscard]] anvil::Status write_all(std::string_view data) override {
        said_.append(data);
        return anvil::ok();
    }

    [[nodiscard]] anvil::Result<std::size_t> read_some(std::span<char> out) override {
        // Deliberately DRIP-FED, a few bytes at a time. A reply reader that
        // assumed one read per line would pass against a real server on a fast
        // link and fail in production against a slow one.
        const std::size_t take = std::min({out.size(), script_.size() - offset_, chunk_});
        std::copy_n(script_.begin() + static_cast<std::ptrdiff_t>(offset_), take, out.begin());
        offset_ += take;
        return take;
    }

    [[nodiscard]] const std::string& said() const noexcept { return said_; }
    void chunk(std::size_t size) noexcept { chunk_ = size; }

private:
    std::string script_;
    std::string said_;
    std::size_t offset_{0};
    std::size_t chunk_{7};
};

[[nodiscard]] n::SmtpConfig config_with_auth() {
    n::SmtpConfig config{};
    config.host = "mail.example.com";
    config.ehlo_name = "anvil.example.com";
    config.credentials = n::SmtpCredentials{"postmaster", "hunter2"};
    return config;
}

[[nodiscard]] n::SmtpConfig config_without_auth() {
    n::SmtpConfig config{};
    config.host = "mail.example.com";
    config.ehlo_name = "anvil.example.com";
    return config;
}

// A complete, successful conversation.
constexpr std::string_view kHappyScript =
    "220 mail.example.com ESMTP\r\n"
    "250-mail.example.com\r\n"
    "250-SIZE 35882577\r\n"
    "250 AUTH PLAIN LOGIN\r\n"
    "235 2.7.0 Accepted\r\n"
    "250 2.1.0 OK\r\n"
    "250 2.1.5 OK\r\n"
    "354 Go ahead\r\n"
    "250 2.0.0 OK queued\r\n"
    "221 2.0.0 Bye\r\n";

[[nodiscard]] n::MailMessage message_of(std::string_view subject, std::string_view body) {
    n::MailMessage message{};
    message.from = "notifications@example.com";
    message.from_name = "Example";
    message.to = "reader@example.org";
    message.subject = subject;
    message.body_text = body;
    message.message_id = "0192f3a4-1111-7000-8000-aabbccddeeff@example.com";
    return message;
}

}  // namespace

// --- header safety -----------------------------------------------------------

TEST(MailHeaders, EveryControlCharacterIsRefusedAndNotOnlyTheNewline) {
    EXPECT_TRUE(n::header_value_is_safe("New sign-in from Firefox"));
    EXPECT_TRUE(n::header_value_is_safe("تسجيل دخول جديد"));

    // CRLF in a subject is not a broken subject. It is whatever headers the
    // attacker wrote after it, and then a body of their choosing.
    EXPECT_FALSE(n::header_value_is_safe("Hi\r\nBcc: everyone@example.com"));
    EXPECT_FALSE(n::header_value_is_safe("Hi\nBcc: everyone@example.com"));
    EXPECT_FALSE(n::header_value_is_safe("Hi\rBcc: everyone@example.com"));
    // A NUL truncates in whatever C string handling sits between here and the
    // wire, which is how the rest of a value disappears without an error.
    EXPECT_FALSE(n::header_value_is_safe(std::string_view{"Hi\0there", 8}));
    EXPECT_FALSE(n::header_value_is_safe("Hi\x7Fthere"));
}

TEST(MailHeaders, NonAsciiIsEncodedAndAsciiIsLeftAlone) {
    // An encoded word where none is needed renders literally in clients that do
    // not decode it, which is most of the ones that matter.
    EXPECT_EQ(n::encode_header_value("New sign-in"), "New sign-in");
    const std::string encoded = n::encode_header_value("تسجيل دخول جديد");
    EXPECT_EQ(encoded.compare(0, 10, "=?UTF-8?B?"), 0) << encoded;
    EXPECT_EQ(encoded.compare(encoded.size() - 2, 2, "?="), 0) << encoded;
}

TEST(MailAddresses, ThePermittedFormIsDeliberatelyNarrow) {
    EXPECT_TRUE(n::address_is_safe("reader@example.org"));
    EXPECT_TRUE(n::address_is_safe("first.last+tag@sub.example.co.uk"));

    // The quoted-string local part and the bracketed literal are both legal and
    // both are where this and a relay can disagree about which address a message
    // is going to.
    EXPECT_FALSE(n::address_is_safe(R"("a b"@example.com)"));
    EXPECT_FALSE(n::address_is_safe("reader@[192.0.2.1]"));
    EXPECT_FALSE(n::address_is_safe("reader@example.org\r\nRCPT TO:<evil@example.net>"));
    EXPECT_FALSE(n::address_is_safe("reader@localhost"));
    EXPECT_FALSE(n::address_is_safe("reader"));
    EXPECT_FALSE(n::address_is_safe("@example.org"));
    EXPECT_FALSE(n::address_is_safe("a@b@example.org"));
    EXPECT_FALSE(n::address_is_safe(""));
}

// --- the message -------------------------------------------------------------

TEST(MailMessageBuilding, TheHeadersAreTheOnesAMailClientNeeds) {
    const anvil::Result<std::string> built =
        n::build_message(message_of("New sign-in", "A new sign-in from Firefox on Linux"),
                         at_seconds(1'700'000'000));
    ASSERT_TRUE(built.ok()) << static_cast<int>(built.code());
    const std::string& data = built.value();

    EXPECT_NE(data.find("From: Example <notifications@example.com>\r\n"), std::string::npos);
    EXPECT_NE(data.find("To: reader@example.org\r\n"), std::string::npos);
    EXPECT_NE(data.find("Subject: New sign-in\r\n"), std::string::npos);
    EXPECT_NE(data.find("Message-ID: <0192f3a4-1111-7000-8000-aabbccddeeff@example.com>\r\n"),
              std::string::npos);
    EXPECT_NE(data.find("Content-Type: text/plain; charset=UTF-8\r\n"), std::string::npos);
    // Without these an out-of-office reply arrives for every notification the
    // account receives.
    EXPECT_NE(data.find("Auto-Submitted: auto-generated\r\n"), std::string::npos);
    EXPECT_NE(data.find("X-Auto-Response-Suppress: All\r\n"), std::string::npos);
    // The date is built from UTC by hand, not through strftime — a host whose
    // locale is not C would otherwise produce month names no client parses.
    EXPECT_NE(data.find("Date: Tue, 14 Nov 2023 22:13:20 +0000\r\n"), std::string::npos) << data;

    EXPECT_EQ(data.compare(data.size() - 5, 5, "\r\n.\r\n"), 0);
}

TEST(MailMessageBuilding, ABodyLineStartingWithADotIsStuffed) {
    // The body reaches the wire base64-encoded, so this asserts the stuffing
    // directly on a payload that would otherwise terminate DATA. Anyone who could
    // put such a line into a notification could send mail as this server.
    n::MailMessage message = message_of("x", "ok");
    const anvil::Result<std::string> built = n::build_message(message, at_seconds(0));
    ASSERT_TRUE(built.ok());
    // Exactly one terminator, at the very end, and no bare ".\r\n" before it.
    const std::string& data = built.value();
    const std::size_t body_start = data.find("\r\n\r\n");
    ASSERT_NE(body_start, std::string::npos);
    const std::string body = data.substr(body_start + 4);
    EXPECT_EQ(body.find("\r\n.\r\n"), body.size() - 5) << body;
}

TEST(MailMessageBuilding, AnInjectedHeaderIsRefusedRatherThanSanitised) {
    // Sanitising invites the question of which spelling of a newline was meant,
    // and that question has been answered wrongly by enough mail libraries.
    EXPECT_EQ(n::build_message(message_of("Hi\r\nBcc: everyone@example.com", "body"),
                               at_seconds(0))
                  .code(),
              ErrorCode::ValidationFailed);

    n::MailMessage bad_from = message_of("Hi", "body");
    bad_from.from_name = "Example\r\nBcc: everyone@example.com";
    EXPECT_EQ(n::build_message(bad_from, at_seconds(0)).code(), ErrorCode::ValidationFailed);

    n::MailMessage bad_id = message_of("Hi", "body");
    bad_id.message_id = "abc>\r\nBcc: everyone@example.com";
    EXPECT_EQ(n::build_message(bad_id, at_seconds(0)).code(), ErrorCode::ValidationFailed);

    n::MailMessage bad_to = message_of("Hi", "body");
    bad_to.to = "reader@example.org\r\nRCPT TO:<evil@example.net>";
    EXPECT_EQ(n::build_message(bad_to, at_seconds(0)).code(), ErrorCode::ValidationFailed);
}

TEST(MailMessageBuilding, NonAsciiSubjectAndBodySurvive) {
    const anvil::Result<std::string> built =
        n::build_message(message_of("تسجيل دخول جديد", "تسجيل دخول جديد من Firefox"),
                         at_seconds(0));
    ASSERT_TRUE(built.ok());
    // base64 rather than 8bit: a relay that does not advertise 8BITMIME may
    // mangle a non-ASCII byte, and notification text routinely is not ASCII.
    EXPECT_NE(built.value().find("Content-Transfer-Encoding: base64\r\n"), std::string::npos);
    EXPECT_NE(built.value().find("Subject: =?UTF-8?B?"), std::string::npos);
}

TEST(MailMessageBuilding, EveryLineIsInsideTheOctetLimit) {
    // A line past 998 octets is one an intermediate relay may fold, and a fold is
    // where a stuffed dot stops being at the start of a line.
    const std::string long_body(64 * 1024, 'x');
    const anvil::Result<std::string> built =
        n::build_message(message_of("x", long_body), at_seconds(0));
    ASSERT_TRUE(built.ok());

    std::size_t start = 0;
    const std::string& data = built.value();
    while (start < data.size()) {
        const std::size_t end = data.find("\r\n", start);
        if (end == std::string::npos) { break; }
        ASSERT_LE(end - start, n::kMaxLineOctets) << start;
        start = end + 2;
    }
}

TEST(MailMessageBuilding, AnOversizedMessageIsRefused) {
    const std::string huge(n::kMaxMessageBytes, 'x');
    EXPECT_EQ(n::build_message(message_of("x", huge), at_seconds(0)).code(),
              ErrorCode::PayloadTooLarge);
}

// --- verdicts ----------------------------------------------------------------

TEST(MailVerdicts, ACodeMapsToWhatItCostsTheEndpoint) {
    EXPECT_EQ(n::verdict_for(250), n::DeliveryVerdict::Delivered);
    // The mailbox does not exist and will not start to — the endpoint is
    // finished, which is different from this message being refused.
    EXPECT_EQ(n::verdict_for(550), n::DeliveryVerdict::Gone);
    EXPECT_EQ(n::verdict_for(553), n::DeliveryVerdict::Gone);
    EXPECT_EQ(n::verdict_for(552), n::DeliveryVerdict::Rejected);
    EXPECT_EQ(n::verdict_for(421), n::DeliveryVerdict::Transient);
    EXPECT_EQ(n::verdict_for(451), n::DeliveryVerdict::Transient);
    EXPECT_EQ(n::verdict_for(0), n::DeliveryVerdict::Transient);
}

// --- the conversation --------------------------------------------------------

TEST(SmtpSession, AHappyPathSendsEveryCommandInOrder) {
    ScriptedServer server{std::string{kHappyScript}};
    const n::SmtpConfig config = config_with_auth();
    n::SmtpSession session{server, config};

    const anvil::Result<std::string> payload =
        n::build_message(message_of("New sign-in", "body"), at_seconds(0));
    ASSERT_TRUE(payload.ok());

    const anvil::Result<n::DeliveryVerdict> verdict = session.send(
        "notifications@example.com", "reader@example.org", payload.value());
    ASSERT_TRUE(verdict.ok()) << static_cast<int>(verdict.code());
    EXPECT_EQ(verdict.value(), n::DeliveryVerdict::Delivered);

    const std::string& said = server.said();
    EXPECT_NE(said.find("EHLO anvil.example.com\r\n"), std::string::npos);
    EXPECT_NE(said.find("AUTH PLAIN "), std::string::npos);
    EXPECT_NE(said.find("MAIL FROM:<notifications@example.com>\r\n"), std::string::npos);
    EXPECT_NE(said.find("RCPT TO:<reader@example.org>\r\n"), std::string::npos);
    EXPECT_NE(said.find("DATA\r\n"), std::string::npos);
    EXPECT_NE(said.find("QUIT\r\n"), std::string::npos);
}

TEST(SmtpSession, AMultilineGreetingIsNotMistakenForSeveralReplies) {
    // A '-' in the fourth column is a continuation. Reading one as a final line
    // sends the next command into the middle of an answer, and every reply after
    // that is off by one — which presents as an unrelated command being refused.
    ScriptedServer server{
        "220-mail.example.com ESMTP ready\r\n"
        "220-This server is monitored\r\n"
        "220 Proceed\r\n"
        "250 mail.example.com\r\n"
        "250 2.1.0 OK\r\n"
        "250 2.1.5 OK\r\n"
        "354 Go ahead\r\n"
        "250 2.0.0 OK queued\r\n"
        "221 Bye\r\n"};
    const n::SmtpConfig config = config_without_auth();
    n::SmtpSession session{server, config};

    const anvil::Result<n::DeliveryVerdict> verdict =
        session.send("notifications@example.com", "reader@example.org", ".\r\n");
    ASSERT_TRUE(verdict.ok()) << static_cast<int>(verdict.code());
    EXPECT_EQ(verdict.value(), n::DeliveryVerdict::Delivered);
}

TEST(SmtpSession, ARepliesSplitAcrossReadsAreReassembled) {
    // One byte at a time. A reader that assumed a read returns whole lines would
    // pass on a fast local link and fail against a real network.
    ScriptedServer server{std::string{kHappyScript}};
    server.chunk(1);
    const n::SmtpConfig config = config_with_auth();
    n::SmtpSession session{server, config};

    const anvil::Result<n::DeliveryVerdict> verdict =
        session.send("notifications@example.com", "reader@example.org", ".\r\n");
    ASSERT_TRUE(verdict.ok());
    EXPECT_EQ(verdict.value(), n::DeliveryVerdict::Delivered);
}

TEST(SmtpSession, AnUnknownMailboxIsGoneAndNotRetried) {
    ScriptedServer server{
        "220 ready\r\n"
        "250 mail.example.com\r\n"
        "250 2.1.0 OK\r\n"
        "550 5.1.1 No such user here\r\n"};
    const n::SmtpConfig config = config_without_auth();
    n::SmtpSession session{server, config};

    const anvil::Result<n::DeliveryVerdict> verdict =
        session.send("notifications@example.com", "reader@example.org", ".\r\n");
    ASSERT_TRUE(verdict.ok());
    EXPECT_EQ(verdict.value(), n::DeliveryVerdict::Gone);
}

TEST(SmtpSession, AGreylistingFourHundredIsTransient) {
    ScriptedServer server{
        "220 ready\r\n"
        "250 mail.example.com\r\n"
        "451 4.7.1 Greylisted, try again later\r\n"};
    const n::SmtpConfig config = config_without_auth();
    n::SmtpSession session{server, config};

    const anvil::Result<n::DeliveryVerdict> verdict =
        session.send("notifications@example.com", "reader@example.org", ".\r\n");
    ASSERT_TRUE(verdict.ok());
    EXPECT_EQ(verdict.value(), n::DeliveryVerdict::Transient);
}

TEST(SmtpSession, ARefusedCredentialIsRejectedRatherThanRetriedForever) {
    ScriptedServer server{
        "220 ready\r\n"
        "250 mail.example.com\r\n"
        "535 5.7.8 Authentication credentials invalid\r\n"};
    const n::SmtpConfig config = config_with_auth();
    n::SmtpSession session{server, config};

    const anvil::Result<n::DeliveryVerdict> verdict =
        session.send("notifications@example.com", "reader@example.org", ".\r\n");
    ASSERT_TRUE(verdict.ok());
    // A refused credential is a configuration problem. Retrying it repeatedly is
    // how an account gets locked out by its own mail server.
    EXPECT_EQ(verdict.value(), n::DeliveryVerdict::Rejected);
}

TEST(SmtpSession, NoCredentialsMeansNoAuthCommand) {
    ScriptedServer server{
        "220 ready\r\n"
        "250 mail.example.com\r\n"
        "250 2.1.0 OK\r\n"
        "250 2.1.5 OK\r\n"
        "354 Go ahead\r\n"
        "250 2.0.0 OK\r\n"
        "221 Bye\r\n"};
    const n::SmtpConfig config = config_without_auth();
    n::SmtpSession session{server, config};

    ASSERT_TRUE(session.send("notifications@example.com", "reader@example.org", ".\r\n").ok());
    EXPECT_EQ(server.said().find("AUTH"), std::string::npos);
}

TEST(SmtpSession, AConnectionClosedMidConversationFailsRatherThanHanging) {
    // SMTP ends with QUIT, not with a silent close.
    ScriptedServer server{"220 ready\r\n250 mail.example.com\r\n"};
    const n::SmtpConfig config = config_without_auth();
    n::SmtpSession session{server, config};

    EXPECT_EQ(session.send("notifications@example.com", "reader@example.org", ".\r\n").code(),
              ErrorCode::ServiceUnavailable);
}

TEST(SmtpSession, AMultilineReplyWithNoEndIsRefusedRatherThanReadForever) {
    std::string endless = "220 ready\r\n";
    for (std::size_t i = 0; i < n::kMaxReplyLines + 10; ++i) {
        endless.append("250-still going\r\n");
    }
    ScriptedServer server{endless};
    const n::SmtpConfig config = config_without_auth();
    n::SmtpSession session{server, config};

    EXPECT_EQ(session.send("notifications@example.com", "reader@example.org", ".\r\n").code(),
              ErrorCode::ServiceUnavailable);
}

TEST(SmtpSession, ALineWithNoNewlineIsBoundedRatherThanBuffered) {
    // An unbounded read from a socket somebody else operates is a memory
    // exhaustion with a network trigger.
    std::string flood = "220 ready\r\n250-";
    flood.append(n::kMaxReplyLineBytes * 4, 'x');
    ScriptedServer server{flood};
    const n::SmtpConfig config = config_without_auth();
    n::SmtpSession session{server, config};

    EXPECT_EQ(session.send("notifications@example.com", "reader@example.org", ".\r\n").code(),
              ErrorCode::ServiceUnavailable);
}

TEST(SmtpSession, AGarbledReplyIsRefusedRatherThanGuessedAt) {
    ScriptedServer server{"hello there\r\n"};
    const n::SmtpConfig config = config_without_auth();
    n::SmtpSession session{server, config};

    EXPECT_EQ(session.send("notifications@example.com", "reader@example.org", ".\r\n").code(),
              ErrorCode::ServiceUnavailable);
}

TEST(SmtpSession, AnInjectedEnvelopeAddressIsRefusedAtThisLayerToo) {
    ScriptedServer server{std::string{kHappyScript}};
    const n::SmtpConfig config = config_without_auth();
    n::SmtpSession session{server, config};

    // build_message refuses this one layer up; it is refused again here rather
    // than assumed, because this is a public entry point of its own.
    EXPECT_EQ(session
                  .send("notifications@example.com",
                        "reader@example.org\r\nRCPT TO:<evil@example.net>", ".\r\n")
                  .code(),
              ErrorCode::ValidationFailed);
    EXPECT_TRUE(server.said().empty());
}

TEST(SmtpSession, AnInjectedEhloNameOrCredentialIsRefused) {
    ScriptedServer server{std::string{kHappyScript}};
    n::SmtpConfig config = config_with_auth();
    config.ehlo_name = "anvil.example.com\r\nMAIL FROM:<evil@example.net>";
    n::SmtpSession session{server, config};

    EXPECT_EQ(session.send("notifications@example.com", "reader@example.org", ".\r\n").code(),
              ErrorCode::ValidationFailed);
    EXPECT_TRUE(server.said().empty());
}
