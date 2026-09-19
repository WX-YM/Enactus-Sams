#pragma once

// A `tel:` href, built from a number a validator produced and from nothing else.
//
// --- why this is not a scheme added to the allow-list -----------------------
//
// `http::append_url_attr` delegates its accept/reject decision to
// `input::is_safe_link_target`, whose list is site-relative, `https:` and
// `mailto:`. `tel:` is not on it, and it must not be added: every scheme on that
// list is one somebody argued for, and a URL rule an application writes for
// itself is the second implementation of "is this link safe" — the one with no
// attacker reading it, and therefore the one that drifts.
//
// The cost of leaving it off is easy to miss, because `append_url_attr` fails
// SILENTLY BY DESIGN: it emits nothing and returns `false`. A handler that hands
// it a `tel:` URL and ignores the return renders an anchor with no `href` — not
// a link, not focusable, not announced as one — and on a contact page the
// symptom is that a phone number stops being tappable on the device every
// visitor is holding, with nothing anywhere saying why.
//
// --- what replaces it -------------------------------------------------------
//
// The shape `append_sanitized` already uses for markup
// (docs/19-server-side-rendering.md §3), applied to a scheme: make the dangerous
// call impossible to reach with the wrong thing. This takes a type only a
// scanner fills, builds the URI itself, and emits through
// `http::append_html_attr`, which still owns the quoting and the escape set. A
// function that cannot be handed a request byte cannot be made to accept one by
// a refactor that was not thinking about it.
//
// It lives in the locale module rather than beside `append_url_attr` because
// `PhoneEgy` is one country's rules and `anvil::http` is not. A general
// `append_tel_attr(std::string_view)` would be the URL rule again, one layer
// down; the general form waits for a second validator to generalise FROM rather
// than being guessed at from one.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "anvil/http/html_writer.h"
#include "anvil/locale_egy/phone_egy.h"

namespace anvil::http {

inline constexpr std::string_view kTelScheme = "tel:";

// `tel:` plus the E.164 form. A compile-time constant because the buffer it
// sizes is automatic storage and the whole point is that nothing here allocates.
inline constexpr std::size_t kTelUriBytes =
    kTelScheme.size() + sizeof(input::PhoneEgy::e164);

// The one shape `validate_phone_egy` writes: `+20`, the mobile `1`, an assigned
// operator digit, then eight more digits — and the operator member agreeing with
// the byte it was taken from.
//
// Re-checked HERE, at the point of emission, rather than trusted. The type
// narrows what can be passed; it cannot prove the value was filled. `PhoneEgy`
// is an aggregate behind an out-parameter API, so `PhoneEgy phone{};` compiles
// and must, which means a caller that ignored the `Reason` holds thirteen NUL
// bytes of a perfectly well-typed phone number. That is the same reason
// `append_sanitized` re-runs the sanitiser's verdict at render instead of
// arguing from the write path: a value that fails the second check got here by a
// route the first one never saw.
[[nodiscard]] constexpr bool is_egyptian_mobile_e164(const input::PhoneEgy& phone) noexcept {
    static_assert(sizeof(input::PhoneEgy::e164) == 13,
                  "the offsets below are the layout validate_phone_egy writes");
    if (phone.e164[0] != '+') { return false; }
    if (phone.e164[1] != input::kEgyptianCallingCode[0]) { return false; }
    if (phone.e164[2] != input::kEgyptianCallingCode[1]) { return false; }
    // The mobile marker. Every Egyptian landline area code starts with something
    // else, which is what makes this one byte the whole of the check.
    if (phone.e164[3] != '1') { return false; }
    if (!input::is_assigned_operator_digit(phone.e164[4])) { return false; }
    if (phone.operator_digit != static_cast<std::uint8_t>(phone.e164[4] - '0')) {
        return false;
    }
    for (std::size_t i = 5; i < phone.e164.size(); ++i) {
        if (phone.e164[i] < '0' || phone.e164[i] > '9') { return false; }
    }
    return true;
}

// Emits ` href="tel:+20…"`, or nothing at all.
//
// No attribute NAME parameter, and that is deliberate rather than an omission: a
// `tel:` URI is an `href` and is nothing else, so there is no name to get wrong
// and no second failure mode where a bad name emits nothing while the call still
// reports success.
//
// Returns false only for a `PhoneEgy` no validator filled — a programming error
// rather than a content one — so a caller that checks it is checking its own
// code path, not the number a customer typed. The escape set still runs over the
// value even though no byte that survives the check is in it: the escaping is
// what keeps this correct if the check is ever loosened, and a writer that skips
// it because "these bytes are safe" is one refactor from not being.
[[nodiscard]] inline bool append_tel_attr(std::string& out, const input::PhoneEgy& phone) {
    if (!is_egyptian_mobile_e164(phone)) { return false; }

    std::array<char, kTelUriBytes> uri{};
    for (std::size_t i = 0; i < kTelScheme.size(); ++i) { uri[i] = kTelScheme[i]; }
    for (std::size_t i = 0; i < phone.e164.size(); ++i) {
        uri[kTelScheme.size() + i] = phone.e164[i];
    }
    append_html_attr(out, "href", std::string_view{uri.data(), uri.size()});
    return true;
}

}  // namespace anvil::http
