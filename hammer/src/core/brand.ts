// A branded type is a value that asserts something about itself, and the
// assertion is only worth anything if nobody outside this library can forge one.
//
// The pattern: a `unique symbol` that is declared but never defined. It exists
// in the type system and nowhere at run time, so a brand costs no bytes, no
// property and no check — and a plain string cannot be assigned where a branded
// one is required, because it lacks a property no caller can spell.
//
// The rule that makes it useful is not in this file and cannot be: every brand's
// factory is module-private, and the only exported way to obtain one does the
// work the brand asserts. `SanitizedHtml` is produced by the sanitiser and by
// nothing else; a `Capability` by the call that mints it. A brand with a public
// constructor is decoration.

declare const kBrand: unique symbol;

export type Brand<T, B extends string> = T & { readonly [kBrand]: B };

// Applies a brand. Exported to the library, never re-exported from an entry
// point: a consumer that can call this can forge every guarantee in here.
export function brand<T, B extends string>(value: T): Brand<T, B> {
    return value as Brand<T, B>;
}
