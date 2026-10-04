// The fields each account screen has, derived from the published table rather
// than written by the application — so a registration form asks for exactly
// the identifiers and profile fields the server's schema declares, bounded as
// the server bounds them, and nothing drifts when the schema changes.
//
// Keys, not words: a label is the application's, per locale, keyed by the
// field key this returns (`FormField` in `hammer/dom`).

import type { FieldDefinition, FieldTypeSpec, InputPurpose } from "../state/forms.js";

import type { AccountFlow, AccountsSpec, IdentifierKind } from "./accounts.js";

// A value that is read back, and one that is never read back once sent — the
// PII shape `Form.accepted()` empties, which is what keeps a password and a
// code out of a store after the call that needed them.
function text(maxCodePoints: number): FieldTypeSpec {
    return {
        answer: "text",
        defaultCodePoints: maxCodePoints,
        flags: {
            options: false,
            attachment: false,
            ranged: false,
            codePointCapped: true,
            multiLine: false,
            multiSelect: false,
        },
    };
}

function secret(maxCodePoints: number): FieldTypeSpec {
    return { ...text(maxCodePoints), answer: null };
}

// Bounds for an identifier as anvil canonicalises it (anvil
// `accounts/identifier.h`): an email of at most 254, a username of 32, and a
// phone number's digits with room for the spaces and hyphens people type.
const kIdentifierMax: Readonly<Record<IdentifierKind, number>> = { email: 254, username: 32, phone: 24 };
const kIdentifierPurpose: Readonly<Record<IdentifierKind, InputPurpose>> = {
    email: "email",
    username: "username",
    phone: "tel",
};

// A single box that takes any identifier the table lets a person sign in with.
// `username` is the platform's token for "the name you sign in with", whichever
// kind it turns out to be.
function identifier(table: AccountsSpec): FieldDefinition {
    let max = 0;
    for (const spec of table.identifiers) {
        if (spec.signIn) {
            max = Math.max(max, kIdentifierMax[spec.kind]);
        }
    }
    return { key: "identifier", type: text(max), required: true, maxCodePoints: max, purpose: "username" };
}

function contactBox(table: AccountsSpec): FieldDefinition {
    const max = kIdentifierMax[table.contact];
    return {
        key: "identifier",
        type: text(max),
        required: true,
        maxCodePoints: max,
        purpose: kIdentifierPurpose[table.contact],
    };
}

function password(key: string, table: AccountsSpec, purpose: InputPurpose): FieldDefinition {
    return {
        key,
        type: secret(table.secret.maxCodePoints),
        required: true,
        maxCodePoints: table.secret.maxCodePoints,
        purpose,
    };
}

function code(table: AccountsSpec): FieldDefinition {
    return {
        key: "code",
        type: secret(table.codeDigits),
        required: true,
        maxCodePoints: table.codeDigits,
        purpose: "one-time-code",
    };
}

// The fields of one account screen, in the order a person meets them. The keys
// are the ones `Accounts.submit` reads from a form body.
export function accountFields(table: AccountsSpec, flow: AccountFlow): readonly FieldDefinition[] {
    switch (flow) {
        case "sign_in":
            return [identifier(table), password("password", table, "current-password")];
        case "register": {
            const fields: FieldDefinition[] = [];
            for (const spec of table.identifiers) {
                const max = kIdentifierMax[spec.kind];
                fields.push({
                    key: spec.kind,
                    type: text(max),
                    required: spec.required,
                    maxCodePoints: max,
                    purpose: kIdentifierPurpose[spec.kind],
                });
            }
            for (const spec of table.profile) {
                fields.push({
                    key: spec.key,
                    type: { ...text(spec.maxCodePoints), flags: { ...text(spec.maxCodePoints).flags, multiLine: spec.lineBreaks } },
                    required: spec.required,
                    maxCodePoints: spec.maxCodePoints,
                });
            }
            fields.push(password("password", table, "new-password"));
            return fields;
        }
        case "verify":
            return [contactBox(table), code(table)];
        case "reset_request":
            return [identifier(table)];
        case "reset_confirm":
            return [identifier(table), code(table), password("password", table, "new-password")];
        case "change":
            return [
                identifier(table),
                password("current_password", table, "current-password"),
                password("new_password", table, "new-password"),
            ];
    }
}
