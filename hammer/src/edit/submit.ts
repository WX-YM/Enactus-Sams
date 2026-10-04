// The two calls an editor makes: submit an edit, and read one back to reopen it.
//
// Neither binds anything. Swapping a document's reference from the old image to
// the new one is the application's own versioned write — the document is the
// application's, and so is the version it read (`CLAUDE.md` §6).
//
// Both routes are the application's, handed in as the generated route constants:
// hammer does not know which ids an application gave them, and a library that
// looked `media.edit` up by name would be holding an application's route table
// (`CLAUDE.md` §1).

import type { HammerError } from "../core/errors.js";
import type { Result } from "../core/result.js";
import { fail, ok } from "../core/result.js";
import type { ApiTypes, CallOptions, CallableRoute, Client } from "../wire/client.js";

import type { EditLimits, EncodedRecipe, Recipe, RecipeError, SourceSize } from "./recipe.js";
import { decodeRecipe } from "./recipe.js";

export type MediaSubject = {
    readonly ns: string;
    readonly id: string;
};

// One request to an edit route: its path parameters, a body or none, a signal.
export type EditCall = (
    route: CallableRoute,
    params: MediaSubject,
    body: Readonly<Record<string, unknown>> | null,
    signal: AbortSignal,
) => Promise<Result<unknown, HammerError>>;

// A client's `call`, for the two edit routes. Asserted once here rather than at
// every call site: both routes take exactly `{ns, id}` and neither declares a
// capability, so the options are exactly those, a body and a signal.
export function editCall<A extends ApiTypes>(client: Client<A>): EditCall {
    return async (route, params, body, signal) =>
        await client.call(route, (body === null ? { params, signal } : { params, body, signal }) as unknown as CallOptions<A, typeof route>);
}

// The server answered 2xx with a body that is not the shape its own route
// description declares. Surfaced rather than guessed around.
export type EditAnswerError = {
    readonly kind: "edit-answer";
};

export type EditedMedia = {
    // The new object, or — when this exact edit of this source already existed —
    // that one. Either is success, and nothing here says which: a retry after a
    // lost response is SUPPOSED to land on the object its first attempt made.
    readonly id: string;
    readonly widthPx: number;
    readonly heightPx: number;
};

export type ReopenedEdit = {
    // The object an editor opens on. For an edit this is its SOURCE, never the
    // edit itself: a re-edit is the source plus the whole recipe, because anvil
    // refuses an edit of an edit.
    readonly source: string;
    readonly size: SourceSize;
    // Null when the object was not an edit.
    readonly recipe: Recipe | null;
};

const kBadAnswer: EditAnswerError = { kind: "edit-answer" };

function isObject(value: unknown): value is Readonly<Record<string, unknown>> {
    return typeof value === "object" && value !== null && !Array.isArray(value);
}

function isPixels(value: unknown): value is number {
    return typeof value === "number" && Number.isInteger(value) && value > 0;
}

export type SubmitEditOptions = {
    readonly route: CallableRoute;
    readonly source: MediaSubject;
    readonly recipe: EncodedRecipe;
    // Required, never defaulted. A detached edit can never be reopened and holds
    // nothing on its source, which is what lets the source be collected: it
    // exists for redaction (anvil `docs/21-image-edits.md` §7). A caller must
    // decide which one it is making rather than reach one by omission.
    readonly detach: boolean;
    readonly signal: AbortSignal;
};

// Submits an edit. The client retries it by its ordinary policy with an
// idempotency key, and anvil also resolves the same recipe on the same source to
// one object by the recipe's hash — so a retry whose first answer was lost comes
// back with the object that first attempt made.
export async function submitEdit(
    call: EditCall,
    options: SubmitEditOptions,
): Promise<Result<EditedMedia, HammerError | EditAnswerError>> {
    const answered = await call(
        options.route,
        options.source,
        { recipe: options.recipe.text, detach: options.detach },
        options.signal,
    );
    if (!answered.ok) {
        return answered;
    }
    const body = answered.value;
    if (!isObject(body) || typeof body["id"] !== "string" || !isPixels(body["width"]) || !isPixels(body["height"])) {
        return fail(kBadAnswer);
    }
    return ok({ id: body["id"], widthPx: body["width"], heightPx: body["height"] });
}

export type ReopenEditOptions = {
    readonly route: CallableRoute;
    readonly subject: MediaSubject;
    readonly limits: EditLimits;
    readonly signal: AbortSignal;
};

// What an editor opens with, for any stored image: the source, its size, and the
// recipe if it is an edit.
//
// A stored recipe that this client cannot decode is refused rather than dropped.
// Opening the source with no recipe would look like an edit that lost its work,
// and saving from there would make a new object that silently discards it.
export async function reopenEdit(
    call: EditCall,
    options: ReopenEditOptions,
): Promise<Result<ReopenedEdit, HammerError | EditAnswerError | RecipeError>> {
    const answered = await call(options.route, options.subject, null, options.signal);
    if (!answered.ok) {
        return answered;
    }
    const body = answered.value;
    if (
        !isObject(body) ||
        typeof body["source"] !== "string" ||
        !isPixels(body["width"]) ||
        !isPixels(body["height"]) ||
        !(body["recipe"] === null || typeof body["recipe"] === "string")
    ) {
        return fail(kBadAnswer);
    }
    const size: SourceSize = { widthPx: body["width"], heightPx: body["height"] };
    const text = body["recipe"];
    if (text === null) {
        return ok({ source: body["source"], size, recipe: null });
    }
    const recipe = decodeRecipe(text, options.limits);
    if (!recipe.ok) {
        return recipe;
    }
    return ok({ source: body["source"], size, recipe: recipe.value });
}
