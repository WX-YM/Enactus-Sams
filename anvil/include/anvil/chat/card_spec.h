#pragma once

// Application message kinds: a poll, a location, a contact card, a product
// (docs/22-chat.md §4.4).
//
// Each is a product's own idea, so the table is the application's. A card is a
// stored code and a body the APPLICATION's binder validates and canonicalises;
// anvil stores the canonical form and hands it back verbatim. The binder is a
// plain function pointer, so the table stays `constexpr` and in `.rodata`, and it
// is written with the same `input::ObjectBinder` every other request body is
// bound with: one way to say what a valid body is.
//
// anvil does not trust the binder's output either. What it answers is parsed
// again, must be a JSON object and must fit kMaxCardBytes, before it is stored:
// a card is served back inside every history page that holds it, and a binder
// that emitted something malformed would otherwise break every one of them.
//
// Plaintext only. Inside an encrypted conversation a card is part of the
// ciphertext, and its rules live in the client.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "anvil/chat/kind_spec.h"
#include "anvil/core/result.h"
#include "anvil/input/json.h"

namespace anvil::chat {

// STORED on every card message, and the table is indexed by it: append only.
using CardCode = std::uint8_t;

inline constexpr std::size_t kMaxCardKinds = 32;

// The canonical body's ceiling. A card is something drawn in a bubble, not a
// document, and it is copied into every history page that holds it.
inline constexpr std::size_t kMaxCardBytes = 4096;

// Validates `body` and answers its canonical JSON, or a Failure naming the
// field and reason (Failure::detail) — compile-time field names, never the
// submitted value.
using CardBinder = Result<std::string> (*)(const input::JsonValue& body);

struct CardSpec final {
    std::string_view key;
    CardBinder       bind;
    CardCode         code;
};

[[nodiscard]] constexpr std::optional<CardCode> card_from_key(std::span<const CardSpec> cards,
                                                              std::string_view key) noexcept {
    for (std::size_t i = 0; i < cards.size(); ++i) {
        if (cards[i].key == key) { return static_cast<CardCode>(i); }
    }
    return std::nullopt;
}

// Codes equal their positions, keys are well formed and unique, and there are
// at most kMaxCardKinds. An EMPTY table is well formed: an application with no
// cards declares none. Whether each binder is set is checked when the service
// is built, not here: comparing a function pointer from a namespace-scope table
// against nullptr is not something GCC folds under -fsanitize=undefined, which
// is the build that has to run this.
[[nodiscard]] constexpr bool cards_are_well_formed(std::span<const CardSpec> cards) noexcept {
    if (cards.size() > kMaxCardKinds) { return false; }
    for (std::size_t i = 0; i < cards.size(); ++i) {
        if (cards[i].code != i) { return false; }
        if (!is_wellformed_kind_key(cards[i].key)) { return false; }
        for (std::size_t j = 0; j < i; ++j) {
            if (cards[j].key == cards[i].key) { return false; }
        }
    }
    return true;
}

}  // namespace anvil::chat
