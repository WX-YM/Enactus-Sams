// `it.each`'s name formatting, factored out of `tests/support/test.ts` so the
// node backend and the browser backend (`tests/support/test_browser.ts`, Phase 8
// B2) implement it identically rather than maintaining two copies that drift
// apart one bug fix at a time.
//
// **No `node:` import.** Row B2 runs the browser backend inside a real Chromium
// page in place of happy-dom, and a `node:` import here would resolve in this
// process and never in the page's — the same reason `expect.ts`'s header gives
// for the same rule.
//
// Formatting follows vitest's `printf`-style substitution
// (https://vitest.dev/api/#test-each) only as far as the suite actually spells a
// template: `%i` is the one token in use today (`tests/wire/retry.test.ts`), and
// `%s`/`%d`/`%f`/`%j`/`%o`/`%#`/`%%` are carried along because they cost nothing
// extra to support faithfully and a future case is far more likely to reach for
// one of THOSE than to invent a new token.

export type EachRow<T> = T extends readonly unknown[] ? T : readonly [T];

export function asRow<T>(value: T): EachRow<T> {
    return (Array.isArray(value) ? value : [value]) as EachRow<T>;
}

export function formatCaseName(template: string, args: readonly unknown[], index: number): string {
    let cursor = 0;
    const substituted = template.replace(/%[sdifjoOp#%]/g, (token) => {
        if (token === "%%") return "%";
        if (token === "%#") return String(index);
        const value = args[cursor];
        cursor += 1;
        switch (token) {
            case "%s":
                return String(value);
            case "%d":
            case "%i":
                return String(Math.trunc(Number(value)));
            case "%f":
                return String(Number(value));
            case "%j":
                return JSON.stringify(value);
            default:
                return JSON.stringify(value);
        }
    });

    if (cursor > 0 || !template.includes("$")) {
        return substituted;
    }

    // `$name`-style substitution reads a property off a single object-shaped
    // case, e.g. `it.each([{ name: "a", value: 1 }])("case $name", ...)`. Not
    // exercised today, but it is the other half of vitest's own naming
    // convention and costs nothing extra once the `%`-token loop above exists.
    const single = args[0];
    if (typeof single !== "object" || single === null) {
        return substituted;
    }
    const row = single as Record<string, unknown>;
    return substituted.replace(/\$([a-zA-Z0-9_.]+)/g, (_match, path: string) => {
        const value = path.split(".").reduce<unknown>((acc, key) => {
            if (typeof acc !== "object" || acc === null) return undefined;
            return (acc as Record<string, unknown>)[key];
        }, row);
        return String(value);
    });
}
