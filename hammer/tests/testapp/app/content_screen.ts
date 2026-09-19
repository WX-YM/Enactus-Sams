// The content seams, satisfied from outside hammer.
//
// Every seam is exercised by type-checking it, and a seam that cannot be
// satisfied from outside the library fails HERE — which is the only place it can
// fail cheaply (`ENGINEERING_RULES.md` §1). Nothing in this file renders: the form renderer,
// the section editor, the image element and the event sink are later phases.
// What is being proved is narrower and is the half that has to be right first —
// that the generated tables carry what those will need, and that a consumer can
// spell them without reaching past the generated module for anything.

import type {
    AnswerKind,
    FieldTypeName,
    MediaNamespace,
    SectionKey,
} from "../api/hammer.generated.js";
import {
    eventSignupCompleted,
    kFieldTypes,
    kMediaDefaultRole,
    kMediaWidths,
    sectionHomeHero,
    sections,
} from "../api/hammer.generated.js";

// The bound a control enforces before a round trip finds out, in CODE POINTS.
// It is the same number the server checks against, which is the whole point of
// generating both sides from one table: a form that accepts what the server
// refuses is a person staring at a field with no error on it.
export function headlineMaxCodePoints(): number {
    const headline = sectionHomeHero.fields[0];
    return headline === undefined ? 0 : headline.maxCodePoints;
}

// Indexed by the LOCALE TABLE's order, which is persisted and append-only. A
// consumer that sorted or renumbered it would label a field in the wrong
// language, and index 0 means nothing in particular.
export function headlineLabel(localeIndex: number): string | undefined {
    const headline = sectionHomeHero.fields[0];
    return headline === undefined ? undefined : headline.labels[localeIndex];
}

export function sitePathOf(key: SectionKey): string {
    return sections[key].sitePath;
}

// A field type is resolved by the `code` a definition carries, which is why the
// table is a table: the type arrives with the document rather than being spelled
// here.
export function answerKindOfCode(code: number): AnswerKind | null | undefined {
    for (const spec of Object.values(kFieldTypes)) {
        if (spec.code === code) {
            return spec.answer;
        }
    }
    return undefined;
}

// A PII type produces no answer a client reads back, so a renderer has to branch
// on it: the value it submitted is not in the document it reads afterwards.
export function isReadBack(name: FieldTypeName): boolean {
    return kFieldTypes[name].answer !== null;
}

// The closed set the server stores an index into. The parameter type is the
// values themselves, so a typo is a compile error rather than a row the ingest
// path drops.
export function signupSurface(
    surface: (typeof eventSignupCompleted.dimensions.surface)[number],
): string {
    return `${eventSignupCompleted.name}:${surface}`;
}

export function needsConsent(): boolean {
    return eventSignupCompleted.requiresConsent;
}

// The width half of a `srcset`, which is the reason a width is published beside
// its role at all: without descriptors the browser has no basis on which to
// choose between the sources it is handed. The other half is an address, and an
// address comes from the route builder — never from a width.
export function widthDescriptors(ns: MediaNamespace): readonly string[] {
    const out: string[] = [];
    for (const [role, width] of Object.entries(kMediaWidths[ns])) {
        out.push(`${role} ${width}w`);
    }
    return out;
}

export function defaultRole(): typeof kMediaDefaultRole {
    return kMediaDefaultRole;
}
