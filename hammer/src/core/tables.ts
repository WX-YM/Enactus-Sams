// The three tables the descriptor cannot carry, as types with no contents.
//
// The descriptor holds what the server knows: names, shapes and bounds
// (`docs/01-seams.md` §13). Three things it cannot know are the application's,
// and each of them is the shape of a table rather than a table — a type hammer
// ships is machinery, a table hammer populates is a bug (`CLAUDE.md` §1).
//
//   COPY          words are an audience decision, and this library ships no
//                 string in any language. An English default is a string that
//                 ships to an Arabic user, from a library, so it ships to every
//                 application at once.
//   CLASS NAMES   a library that ships class names ships a design system.
//   INVALIDATION  the server knows what a write touches. It does not know what
//                 a screen is showing.
//
// What each of them buys is the same thing anvil's `static_assert`s buy, in the
// only currency TypeScript has: the record types below are TOTAL over the
// generated unions, so a validation reason, a locale or an error code added
// server-side is a compile error in every client until somebody writes the
// words for it. The alternative is a fallback string, and a fallback string is
// how a wire enumerator reaches a user in an interface that is otherwise
// entirely in Arabic.

// Every word one locale needs for a failure. Total over both vocabularies: the
// map is `Record`, never `Partial<Record>`, because the missing entry is exactly
// the one nobody notices until it is on a screen.
export type FailureCopy<Code extends string, Reason extends string> = {
    // Keyed by the error code the envelope decoded to, including the `Unknown`
    // member the generator adds — a code this bundle predates still has to say
    // something, and what it says is the application's.
    readonly errors: Readonly<Record<Code, string>>;

    // Keyed by the reason a field failed. anvil names the reason and never the
    // sentence, for the reason this whole type exists.
    readonly reasons: Readonly<Record<Reason, string>>;
};

export type Copy<Locale extends string, Code extends string, Reason extends string> = Readonly<
    Record<Locale, FailureCopy<Code, Reason>>
>;

// The parts of one component, named by the component and spelled by the
// application. The part union belongs to whichever component declares it —
// hammer ships structure, behaviour, ARIA and direction, and not one class name
// (`CLAUDE.md` §1, §9).
export type ClassNames<Part extends string> = Readonly<Record<Part, string>>;

// Which resources a mutation makes stale, by route id.
//
// Partial rather than total, and it is the one of the three that is: a read
// invalidates nothing, so requiring an entry for every route would be a table of
// empty arrays whose only effect is to hide the routes that matter. What the
// type does enforce is that both sides are real route ids — a mutation that
// names a resource the server retired is an invalidation that silently stops
// happening, and nothing renders wrongly until somebody notices a stale row.
export type Invalidations<RouteId extends string> = Readonly<
    Partial<Record<RouteId, readonly RouteId[]>>
>;
