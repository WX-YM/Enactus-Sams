#pragma once

// Validation of the plaintext parts of a chat message: the body, its mention
// spans, a client-built link preview and a reaction (docs/22-chat.md §4.3,
// §4.6).
//
// Pure functions over views, in the shape of input/fields.h: no allocation, no
// exceptions, no database. They answer "is this value well formed"; whether a
// mentioned user is a member, whether a reaction is on an application's
// palette, and whether the sender may post at all are questions about state,
// and belong to the service that holds it.
//
// Every text here is REFUSED rather than repaired, as everywhere at this trust
// boundary (i18n/utf8.h). Two choices follow from that and are easy to undo by
// accident:
//
//   NFC is required, not applied. Mention spans are code-point offsets into the
//   text the client sent. Normalising on the server can change the number of
//   code points (a composition exclusion such as U+0958 decomposes under NFC),
//   which would silently move every span after it onto the wrong characters.
//   Refusing non-NFC text keeps the client's offsets and the stored text in one
//   frame, and every client platform can normalise before it sends.
//
//   One newline spelling: `\n`. `\r` is refused, including as part of CRLF, and
//   so are U+2028/U+2029. Each alternative spelling of a line break is another
//   code point the bound counts differently per platform, another way for a
//   span offset to disagree with what the reader sees, and in a log viewer a
//   bare `\r` overwrites the line it sits on.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>

#include "anvil/core/types.h"
#include "anvil/input/fields.h"

namespace anvil::chat {

// --- message body -----------------------------------------------------------

// anvil's ceiling for a message body, in code points (docs/22-chat.md §2.4). A
// conversation kind may lower it; it cannot raise it.
inline constexpr std::uint32_t kMaxMessageCodePoints = 4096;

// `max_code_points` is the kind's bound. A value above kMaxMessageCodePoints is
// CLAMPED to it rather than refused: the bound comes from the application's
// kind table, not from the request, and a misconfigured table should not turn
// every send into a failure. The table itself is checked when it is declared.
//
// Refused:
//   Required    empty, or nothing but whitespace and default-ignorable code
//               points — a message that renders as nothing is a notification
//               that says nothing, and a cheap way to bump a conversation.
//   TooLong     over the bound, in code points.
//   BadCharset  invalid UTF-8, NUL, a non-character, any C0 or C1 control
//               except `\n` and `\t`, `\r`, U+2028/U+2029, or a bidi override
//               or embedding. Isolates (U+2066..U+2069) are ALLOWED: this is
//               i18n::TextClass::Prose, and a mixed Arabic/English sentence
//               genuinely needs them.
//   BadFormat   not in NFC.
[[nodiscard]] input::Reason validate_message_text(std::string_view text,
                                                  std::uint32_t max_code_points) noexcept;

// --- mentions ---------------------------------------------------------------

// One line of prose: a conversation's title, an attachment's file name. The
// message rules without line breaks. Empty is Required; the caller decides
// whether an empty value means "none".
[[nodiscard]] input::Reason validate_line(std::string_view text,
                                          std::uint32_t max_code_points) noexcept;

// Prose that may span lines: a conversation's description. The message rules,
// under the caller's bound.
[[nodiscard]] input::Reason validate_prose(std::string_view text,
                                           std::uint32_t max_code_points) noexcept;

inline constexpr std::size_t kMaxMentions = 32;

// A mention is an id plus where it sits in the text. The id is the authority:
// display names are neither unique nor stable, so nothing is parsed out of
// `@name`. Offsets and lengths are in CODE POINTS, the unit every client's
// string indexing can be converted to without knowing the server's encoding.
struct MentionSpan final {
    Uuid          user;     // 16
    std::uint32_t offset;   //  4
    std::uint32_t length;   //  4
};
static_assert(sizeof(MentionSpan) == 24);
static_assert(std::is_trivially_copyable_v<MentionSpan>);

// `text` must already have passed validate_message_text.
//
// Refused:
//   TooLong     more than kMaxMentions spans.
//   Required    a nil user id.
//   BadFormat   an empty span, or spans out of offset order or overlapping.
//   OutOfRange  a span that ends past the end of the text.
//
// Not checked here, deliberately:
//   Membership: whether each user is a current member is the service's
//   question, asked against the conversation it has loaded.
//   Duplicates: the same person may be mentioned twice in one message.
//   Grapheme boundaries: code-point indexing already guarantees no span cuts a
//   code point. A span starting on a combining mark mis-highlights the sender's
//   own text and nothing else — the id, not the highlighted text, decides who
//   is notified — and a grapheme check would put an ICU break iterator over up
//   to 4096 code points on every send to prevent a cosmetic fault.
[[nodiscard]] input::Reason validate_mentions(std::string_view text,
                                              std::span<const MentionSpan> mentions) noexcept;

// --- link preview -----------------------------------------------------------

// The server never fetches a preview (docs/22-chat.md §4.3: that is SSRF by
// design, and in an encrypted conversation a disclosure of the URL). Every part
// of one is therefore client-supplied, untrusted input.
struct LinkPreview final {
    std::string_view url;
    std::string_view title;
    std::string_view description;
};

inline constexpr std::size_t kMaxPreviewUrlBytes = 2048;
inline constexpr std::uint32_t kMaxPreviewTitleCodePoints = 200;
inline constexpr std::uint32_t kMaxPreviewDescriptionCodePoints = 500;

inline constexpr std::string_view kPreviewUrlField = "preview.url";
inline constexpr std::string_view kPreviewTitleField = "preview.title";
inline constexpr std::string_view kPreviewDescriptionField = "preview.description";

// nullopt when the preview is acceptable; otherwise the first failing part.
//
// The URL must be an absolute `https:` URL: a preview of a site-relative,
// fragment or mailto: target describes nothing a reader could open. Its bound
// is in BYTES, the one exception to code points here, because a URL that has
// been IDNA- and percent-encoded is ASCII — any byte above 0x7F means it has
// not been, and an un-encoded host is where a homoglyph domain hides. The
// scheme decision is the HTML writer's own (input::is_safe_link_target), so
// the two cannot drift apart, and the authority goes through input::check_url,
// which refuses credentials (`https://bank.test@evil.test/`) and IP literals.
//
// Title and description are optional (an empty one is accepted) and follow the
// message body's character rules. The title is a single line: no `\n`, no
// `\t`.
[[nodiscard]] std::optional<input::FieldError> validate_link_preview(
    const LinkPreview& preview) noexcept;

// --- reactions --------------------------------------------------------------

inline constexpr std::uint32_t kMaxReactionCodePoints = 8;
// Eight code points of at most four bytes each. Checked FIRST, before a byte is
// decoded or the break iterator is touched, so the cost of an oversized value
// is one comparison.
inline constexpr std::size_t kMaxReactionBytes = 32;

// Exactly one extended grapheme cluster (UAX #29) of at most
// kMaxReactionCodePoints code points, in NFC. Whether that cluster is "an
// emoji" is not decided here (docs/22-chat.md §4.6): an application that wants
// a fixed palette checks membership in its own list.
//
// The character policy is i18n::TextClass::Identifier with one exception: ZWJ
// (U+200D) is allowed, because it is how every family, profession and
// multi-skin-tone emoji is spelled. It may not be the first or last code
// point, where it joins nothing and only makes a second spelling of the same
// visible reaction. Reactions are counted per distinct value, so two spellings
// of one picture would split its tally.
//
// Refused: Required (empty, or blank), TooLong (over either bound), BadCharset
// (invalid UTF-8, controls, bidi controls and marks, zero-width characters
// other than an interior ZWJ), BadFormat (not NFC, or not exactly one cluster,
// or the segmenter was unavailable — which fails closed).
[[nodiscard]] input::Reason validate_reaction(std::string_view reaction) noexcept;

// --- bounds the rows carry ------------------------------------------------------

inline constexpr std::uint32_t kMaxTitleCodePoints = 100;
inline constexpr std::uint32_t kMaxDescriptionCodePoints = 1024;
inline constexpr std::size_t kMaxAttachments = 10;
inline constexpr std::uint32_t kMaxAttachmentNameCodePoints = 255;

// --- bounds on an encrypted send (docs/22-chat.md §7.6) ---------------------------

// One ciphertext, common or per device: the socket's frame bound
// (http/upgrade.h), so a message that arrives by wake arrives whole (§2.4).
inline constexpr std::size_t kMaxCiphertextBytes = 64U * 1024U;

// Per-device ciphertexts in one request. A pairwise fan-out to a large group
// would exceed the request bound, which is why groups use sender keys and send
// their distribution in pages of this many (§7.6).
inline constexpr std::size_t kMaxDeviceCiphertexts = 64;

}  // namespace anvil::chat
