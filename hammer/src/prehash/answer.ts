// The salt route's answer, decoded in exactly one place (`CLAUDE.md` §7).
//
//   {"algorithm":"argon2id","version":19,"salt":"<22 base64url>",
//    "memory_kib":M,"iterations":T,"parallelism":P,"hash_bytes":32}
//
// anvil writes it with `append_prehash_salt_answer` (anvil `docs/05` §12), and
// every field is checked rather than trusted into shape. The algorithm and the
// lengths are not parameters: a version-1 client that met a different one would
// derive a credential no version-1 server can verify, so it refuses instead.

import type { PrehashError } from "../core/errors.js";
import type { Result } from "../core/result.js";
import { fail, ok } from "../core/result.js";

import { decodeBase64Url } from "../core/base64url.js";

export const kPrehashSaltBytes = 16;
export const kPrehashKeyBytes = 32;

export type SaltAnswer = {
    readonly salt: Uint8Array;
    readonly memoryKib: number;
    readonly iterations: number;
    readonly parallelism: number;
};

const kBadAnswer: PrehashError = { kind: "prehash", cause: "bad-answer" };

function positiveInteger(value: unknown): number | null {
    return typeof value === "number" && Number.isInteger(value) && value >= 1 && value <= 0xffffffff
        ? value
        : null;
}

export function decodeSaltAnswer(body: unknown): Result<SaltAnswer, PrehashError> {
    if (typeof body !== "object" || body === null || Array.isArray(body)) {
        return fail(kBadAnswer);
    }
    const answer = body as Readonly<Record<string, unknown>>;

    if (answer["algorithm"] !== "argon2id" || answer["version"] !== 19 || answer["hash_bytes"] !== kPrehashKeyBytes) {
        return fail(kBadAnswer);
    }
    const saltText = answer["salt"];
    const salt = typeof saltText === "string" ? decodeBase64Url(saltText, kPrehashSaltBytes) : null;
    const memoryKib = positiveInteger(answer["memory_kib"]);
    const iterations = positiveInteger(answer["iterations"]);
    const parallelism = positiveInteger(answer["parallelism"]);
    if (salt === null || memoryKib === null || iterations === null || parallelism === null) {
        return fail(kBadAnswer);
    }
    return ok({ salt, memoryKib, iterations, parallelism });
}
