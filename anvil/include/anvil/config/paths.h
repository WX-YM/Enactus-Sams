#pragma once

// Path containment, as one rule with one implementation.
//
// Both the configuration loader and the storage layer need to answer "is this
// path inside that root", and they must answer it identically. Two
// implementations is how one of them ends up comparing strings.

#include <string_view>

namespace anvil::config {

// True when `path` is inside `root` BY PATH COMPONENT, not by string prefix.
//
// "/srv/storage-evil" has "/srv/storage" as a string prefix and is a completely
// different directory. A containment check written as a prefix comparison
// therefore accepts a sibling directory whose name merely starts the same way,
// which for a storage root means accepting a directory the operator never
// granted.
[[nodiscard]] bool path_is_within(std::string_view root, std::string_view path) noexcept;

// True when `path` sits under a directory a reverse proxy conventionally serves
// as a static root.
//
// A storage root under one of these is reachable without passing through the
// application at all: the `internal;` marker that makes protected media private
// applies to one location block, and a second block serving the same bytes as
// ordinary static files undoes it silently. Nothing errors; the files are simply
// public.
//
// An application's boot check should refuse to start on a true result here. It is
// a heuristic — a proxy can serve anything from anywhere — so it is a floor and
// not a guarantee, and an application that serves from an unconventional root
// still has to check its own.
[[nodiscard]] bool is_web_served_root(std::string_view path) noexcept;

}  // namespace anvil::config
