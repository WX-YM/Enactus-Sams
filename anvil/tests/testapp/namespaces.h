#pragma once

// The reference application's storage namespaces, named.
//
// anvil's Ns is a validated index; this is where an application gives those
// indices names. The enum is for readability only — it is never stored as a C++
// type and never crosses a function boundary. What crosses is anvil::fs::Ns,
// which is one byte and cannot be handed "../content".
//
// Ns::of is constexpr and validating, so an enumerator that outruns the table in
// anvil_app_config.h is a COMPILE error at the declaration below rather than a
// nullopt somebody dereferences at runtime.

#include <cstdint>

#include "anvil/fs/namespace.h"

namespace testapp {

enum class MediaNs : std::uint8_t {
    Content = 0,
    Media   = 1,
    Guest   = 2,
};

inline constexpr anvil::fs::Ns kContent = anvil::fs::Ns::of(MediaNs::Content);
inline constexpr anvil::fs::Ns kMedia   = anvil::fs::Ns::of(MediaNs::Media);
inline constexpr anvil::fs::Ns kGuest   = anvil::fs::Ns::of(MediaNs::Guest);

static_assert(kContent.dir() == "content");
static_assert(kMedia.dir() == "media");
static_assert(kGuest.dir() == "guest");

// The index is what is stored, so it is what must never drift.
static_assert(kContent.stored() == 0);
static_assert(kGuest.stored() == 2);

}  // namespace testapp
