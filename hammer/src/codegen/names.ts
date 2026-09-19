// The one place a name from the descriptor becomes an identifier in TypeScript.
//
// It is a module of its own because two callers need to agree on it and they
// pull in opposite directions: the emitter spells the identifier, and the reader
// has to refuse a descriptor whose names cannot be spelled — or would collide
// once they are. A generator that discovers either while writing the file has
// already written half of it.
//
// The mapping is one way on purpose. anvil's names are the names, everywhere a
// person reads one: an investigator comparing a staff screen against a server
// log needs the same word on both (`docs/01-seams.md` §3). What is derived here
// is the identifier a `const` is BOUND to, which is minified out of every bundle
// and appears in no log.

// A name anvil can emit and TypeScript can carry: a letter, then letters and
// digits, then any number of separated parts. Every quantifier consumes a
// character the next one cannot, so there is nothing to backtrack over.
const kSpellable = /^[A-Za-z][A-Za-z0-9]*(?:[._-][A-Za-z0-9]+)*$/;

export function isSpellable(name: string): boolean {
    return kSpellable.test(name);
}

// Splits on the three separators anvil's names actually use: the dot of a route
// id, the underscore of an enumerator, the hyphen of a rate-limit bucket.
function parts(name: string): readonly string[] {
    return name.split(/[._-]/);
}

// The tail is lowered only when the whole part is upper, so that an anvil
// enumerator reads as one word — `TOO_LONG` becomes `TooLong` — while a name
// that already carries its own capitals keeps them: `ContentRead` is the word an
// investigator sees in a server log, and `Contentread` is not.
function capitalise(part: string): string {
    const head = part.charAt(0).toUpperCase();
    const tail = part.slice(1);
    return head + (tail === tail.toUpperCase() ? tail.toLowerCase() : tail);
}

// `auth.login` becomes `AuthLogin`, `login-acct` becomes `LoginAcct`. Two names
// that differ only by separator or by case land on one identifier, which is a
// collision — and collisions are the reader's to report rather than the
// emitter's to resolve silently.
export function pascalCase(name: string): string {
    let out = "";
    for (const part of parts(name)) {
        out += capitalise(part);
    }
    return out;
}

export function routeIdentifier(id: string): string {
    return `route${pascalCase(id)}`;
}

export function permissionIdentifier(name: string): string {
    return `kPerm${pascalCase(name)}`;
}

export function sectionIdentifier(key: string): string {
    return `section${pascalCase(key)}`;
}

export function topicIdentifier(key: string): string {
    return `topic${pascalCase(key)}`;
}

export function eventIdentifier(name: string): string {
    return `event${pascalCase(name)}`;
}
