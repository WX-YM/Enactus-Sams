// The bundle an anonymous visitor downloads.
//
// It exists to be BUILT and then read, by tests/codegen/built_output.test.ts.
// Asserting against the source would assert the thing a bundler configuration
// can silently invalidate; asserting against the output is the only form of the
// claim that survives a change to how the application is built — and a lazily
// loaded chunk is a public URL either way, so code-splitting was never the
// control (docs/01-seams.md §4.1).

import { PermSet } from "hammer";

import { kPermContentRead, publicRoutes, publicTopics } from "../api/hammer.generated.js";
import { copy } from "./copy.js";

export function loginRoute(): (typeof publicRoutes)["auth.login"] {
    return publicRoutes["auth.login"];
}

export function mayReadContent(encoded: string): boolean {
    const set = PermSet.fromBase64Url(encoded);
    return set.ok && set.value.has(kPermContentRead);
}

// The other half of the split, from the side that is allowed to see it: a topic
// nothing gates has its key in this bundle, because subscribing to it discloses
// nothing a visitor did not already have.
export function publishedTopic(): (typeof publicTopics)["content.published"] {
    return publicTopics["content.published"];
}

export function signInPrompt(): string {
    return copy.en.errors.UNAUTHENTICATED;
}
