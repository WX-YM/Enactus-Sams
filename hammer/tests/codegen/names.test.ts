// The identifier derivation, tested directly rather than through the emitted
// file.
//
// It is here because the one defect this module has had was invisible from
// anywhere else: it lowercased the tail of every part, so `ContentRead` became
// `kPermContentread`, and every assertion written at the time checked the BIT a
// permission const holds rather than the name it is bound to. A test that
// derives its expectation the same way the code does would have agreed with the
// bug, so every expectation below is written out.

import { describe, expect, it } from "vitest";

import {
    isSpellable,
    pascalCase,
    permissionIdentifier,
    routeIdentifier,
} from "../../src/codegen/names.js";

describe("a name that already carries its own capitals", () => {
    it("keeps them", () => {
        // `ContentRead` is the word in the server's log and on the staff
        // screen. `Contentread` is a word in neither.
        expect(permissionIdentifier("ContentRead")).toBe("kPermContentRead");
        expect(permissionIdentifier("FormPii")).toBe("kPermFormPii");
        expect(pascalCase("DraftPreview")).toBe("DraftPreview");
    });
});

describe("a name that is entirely upper", () => {
    it("reads as one word", () => {
        expect(pascalCase("TOO_LONG")).toBe("TooLong");
        expect(pascalCase("BAD_CHARSET")).toBe("BadCharset");
    });
});

describe("the separators anvil's names use", () => {
    it("joins a dotted route id", () => {
        expect(routeIdentifier("auth.login")).toBe("routeAuthLogin");
        expect(routeIdentifier("content.preview")).toBe("routeContentPreview");
    });

    it("joins a hyphenated bucket", () => {
        expect(pascalCase("login-acct")).toBe("LoginAcct");
        expect(pascalCase("resend-addr")).toBe("ResendAddr");
    });
});

describe("two names that land on one identifier", () => {
    it("are detectable, which is why the reader refuses them", () => {
        // The derivation cannot resolve this and must not try: one of the two
        // `const`s would silently shadow the other. It is reported against the
        // name, by readDescriptor.
        expect(permissionIdentifier("ContentRead")).toBe(permissionIdentifier("CONTENT_READ"));
        expect(routeIdentifier("auth.login")).toBe(routeIdentifier("auth-login"));
    });
});

describe("a name TypeScript cannot carry", () => {
    it("is refused before anything is emitted", () => {
        expect(isSpellable("ContentRead")).toBe(true);
        expect(isSpellable("login-acct")).toBe(true);
        expect(isSpellable("auth.login")).toBe(true);
        expect(isSpellable("")).toBe(false);
        expect(isSpellable("Media Delete")).toBe(false);
        expect(isSpellable("1st")).toBe(false);
        expect(isSpellable("trailing.")).toBe(false);
        expect(isSpellable('x"; drop')).toBe(false);
    });
});
