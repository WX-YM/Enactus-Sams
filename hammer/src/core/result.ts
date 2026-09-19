// Failure is in the return type.
//
// `throw` is reserved for programmer error — a violated precondition, a
// misconfigured client — and never for a network failure or a server error,
// both of which are certainties rather than exceptions. An exception thrown for
// an expected condition is a `catch` somebody forgets to write, and the shape of
// that bug is a promise that rejects into a handler nobody attached.

export type Ok<T> = { readonly ok: true; readonly value: T };
export type Fail<E> = { readonly ok: false; readonly error: E };

// A discriminated union rather than a class: narrowing on `.ok` is something the
// compiler does for free, and a class would need an instanceof check that a
// structured clone across a worker boundary would not survive.
export type Result<T, E> = Ok<T> | Fail<E>;

export function ok(): Ok<void>;
export function ok<T>(value: T): Ok<T>;
export function ok<T>(value?: T): Ok<T | void> {
    return { ok: true, value: value as T };
}

export function fail<E>(error: E): Fail<E> {
    return { ok: false, error };
}

export function isOk<T, E>(result: Result<T, E>): result is Ok<T> {
    return result.ok;
}

export function isFail<T, E>(result: Result<T, E>): result is Fail<E> {
    return !result.ok;
}

// Unwraps, or throws — which is the one legitimate use of a throw here: a caller
// that has already narrowed and wants the value without re-narrowing. It throws
// a plain Error because reaching it IS a programmer error; there is no code for
// "the programmer did not check".
export function expect<T, E>(result: Result<T, E>, why: string): T {
    if (!result.ok) {
        throw new Error(why);
    }
    return result.value;
}
