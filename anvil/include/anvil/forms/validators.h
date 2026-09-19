#pragma once

// The seven universal validators, and the rule names every failure they produce
// carries.
//
// "Universal" is the whole claim: each of these is a question every form system
// asks — is this text within its bound, is this number within its range, is this
// one of the values the SERVER offered — and none of them names a locale, a
// document format or a product's vocabulary. Anything that does is the
// application's, written as a free function of the same shape and dropped into
// the table (docs/01-seams.md §5). An identity number is the archetype: its rules
// are a country's, and its value must never become an answer.
//
// Every one of these is pure. No database, no clock, no allocation beyond the
// answer it fills.

#include <string>
#include <string_view>

#include "anvil/core/result.h"
#include "anvil/forms/answer.h"

namespace anvil::forms {

// The compile-time names of the RULES a validation can break. A name taken from
// the request — a client-chosen `fid`, an option value — would put
// client-controlled bytes into a response and a log line, which is a
// log-injection and reflected-XSS primitive and, with Arabic, an encoding hazard
// too. The client already holds the localised text for each of these.
inline constexpr std::string_view kAnswersField = "ans";
inline constexpr std::string_view kUnknownField = "ans.unknown";
inline constexpr std::string_view kMissingField = "ans.missing";
inline constexpr std::string_view kTypeField = "ans.type";
inline constexpr std::string_view kOptionField = "ans.option";
inline constexpr std::string_view kRangeField = "ans.range";
inline constexpr std::string_view kMediaField = "ans.media";
inline constexpr std::string_view kIdentityField = "ans.identity";
inline constexpr std::string_view kFormField = "form";

// Membership in the field's own option list — the SERVER'S list. Whatever the
// browser rendered is not a constraint: a client that posts a value it was never
// shown is exactly the case this exists for.
//
// Linear over at most 50 short strings, which is cheaper than the set a
// loop-free version would allocate per answer.
[[nodiscard]] bool option_offered(const FieldSpec& field, std::string_view value) noexcept;

namespace validators {

// Bounded in CODE POINTS, never bytes, against the cap bind_field_type already
// resolved. Line breaks are allowed exactly when the type declares MultiLine.
[[nodiscard]] Status text(const FieldSpec& field, const RawAnswer& raw, Answer& out,
                          std::string& identity);

// The declared range is checked against the PARSED value rather than the text, so
// a JSON string can never arrive here as an integer.
[[nodiscard]] Status number(const FieldSpec& field, const RawAnswer& raw, Answer& out,
                            std::string& identity);

[[nodiscard]] Status email(const FieldSpec& field, const RawAnswer& raw, Answer& out,
                           std::string& identity);

// Digit-folded first, then the CALENDAR is validated — 2026-02-30 matches a digit
// pattern and is not a date. The folded form is what is stored, so an export
// contains one spelling of each date rather than two.
[[nodiscard]] Status date(const FieldSpec& field, const RawAnswer& raw, Answer& out,
                          std::string& identity);

[[nodiscard]] Status select_single(const FieldSpec& field, const RawAnswer& raw, Answer& out,
                                   std::string& identity);

// Additionally rejects duplicates and enforces the selection ceiling. A repeated
// choice is not a stronger answer; it is a client bug or an attempt to inflate a
// tally.
[[nodiscard]] Status checkbox_multi(const FieldSpec& field, const RawAnswer& raw, Answer& out,
                                    std::string& identity);

// Parses the id and NOTHING ELSE. Whether the object exists, lives in the right
// namespace, belongs to this submitter and is not already bound are four
// questions about the application's storage, answered by the attachment hooks
// inside the submission service — where a client is available to answer them and
// where a failure can be the stealth 404 it has to be.
[[nodiscard]] Status attachment_uuid(const FieldSpec& field, const RawAnswer& raw, Answer& out,
                                     std::string& identity);

}  // namespace validators
}  // namespace anvil::forms
