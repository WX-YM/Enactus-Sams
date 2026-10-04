// What every screen shares: the words for a failure, a load hook over hammer's
// call, and the signed-in session.

import { createContext, useCallback, useContext, useEffect, useRef, useState } from "react";
import type { ReactNode } from "react";
import type { HammerError } from "hammer";
import type { AffordableRoute } from "hammer/wire";

import type { ErrorCode, ValidationReason } from "../api/hammer.generated";
import { routeIdentityMe } from "../api/hammer.generated";
import type { Me } from "./responses";
import { platform } from "./platform";

// Total over the server's codes: a code anvil adds is a compile error here until
// it has words. NOT_FOUND says only that — a stealth refusal is a 404 on purpose.
const kErrorWords: Record<ErrorCode, string> = {
    UNAUTHENTICATED: "Your session has ended. Please sign in again.",
    FORBIDDEN: "Your account does not have access to this.",
    NOT_FOUND: "That could not be found. It may have been removed.",
    CAPABILITY_REQUIRED: "This action needs confirmation.",
    CAPABILITY_INVALID: "The confirmation expired. Try again.",
    VALIDATION_FAILED: "Some of the details were not accepted.",
    CONFLICT: "That conflicts with something that already exists.",
    VERSION_MISMATCH: "Someone else changed this while you were editing. Reload and try again.",
    RATE_LIMITED: "Too many attempts. Wait a moment and try again.",
    PAYLOAD_TOO_LARGE: "That is too large.",
    UNSUPPORTED_MEDIA: "That file type is not supported. Use a JPEG or PNG image.",
    SERVICE_UNAVAILABLE: "The server is busy. Try again in a moment.",
    INTERNAL: "Something went wrong on the server.",
    INSUFFICIENT_STORAGE: "The server is out of storage space.",
    Unknown: "Something went wrong.",
};

const kReasonWords: Record<ValidationReason, string> = {
    REQUIRED: "is required",
    TOO_SHORT: "is too short",
    TOO_LONG: "is too long",
    BAD_FORMAT: "has the wrong format",
    BAD_CHARSET: "contains characters that are not allowed",
    OUT_OF_RANGE: "is out of range",
    NOT_ALLOWED: "is not an allowed value",
    BAD_CHECKSUM: "has a wrong check digit",
    WEAK: "is too easy to guess",
    BREACHED: "has appeared in a data breach",
    Unknown: "was not accepted",
};

type AnyFailure = HammerError | { readonly kind: string; readonly cause?: string };

export function describe(error: AnyFailure): string {
    if (error.kind === "server") {
        const server = error as HammerError & { kind: "server" };
        const code = (server.code in kErrorWords ? server.code : "Unknown") as ErrorCode;
        if (server.fields !== null && server.fields.size > 0) {
            const parts: string[] = [];
            server.fields.forEach((reason, field) => {
                const words = kReasonWords[(reason in kReasonWords ? reason : "Unknown") as ValidationReason];
                parts.push(field === "" ? `A field ${words}.` : `${field.replace(/_/g, " ")} ${words}.`);
            });
            return parts.join(" ");
        }
        return kErrorWords[code];
    }
    if (error.kind === "transport") { return "The server could not be reached. Check your connection."; }
    if (error.kind === "account") {
        return error.cause === "secret-too-short"
            ? "The password must be at least 12 characters."
            : "The password must be at most 128 characters.";
    }
    if (error.kind === "prehash") { return "This browser could not prepare the sign-in. Try another browser."; }
    if (error.kind === "stale-client") { return "The admin panel was updated. Reload the page."; }
    return "Something went wrong.";
}

// The server's code for a failure, or null when no server answered.
export function codeOf(error: AnyFailure): string | null {
    return error.kind === "server" ? (error as HammerError & { kind: "server" }).code : null;
}

export type Load<T> = {
    readonly data: T | null;
    readonly error: string | null;
    readonly loading: boolean;
    readonly reload: () => void;
};

export type CallResult<T> = { readonly ok: true; readonly value: T } | { readonly ok: false; readonly error: AnyFailure };

// Runs `fetcher` on mount and whenever `deps` change, aborting the previous
// request. The fetcher returns hammer's Result; the hook never throws.
export function useLoad<T>(fetcher: (signal: AbortSignal) => Promise<CallResult<T>>, deps: readonly unknown[]): Load<T> {
    const [data, setData] = useState<T | null>(null);
    const [error, setError] = useState<string | null>(null);
    const [loading, setLoading] = useState(true);
    const [tick, setTick] = useState(0);
    const latest = useRef(fetcher);
    latest.current = fetcher;

    useEffect(() => {
        const controller = new AbortController();
        setLoading(true);
        latest.current(controller.signal).then((result) => {
            if (controller.signal.aborted) { return; }
            if (result.ok) {
                setData(result.value);
                setError(null);
            } else {
                setError(describe(result.error));
            }
            setLoading(false);
        }, () => {
            if (!controller.signal.aborted) {
                setError("Something went wrong.");
                setLoading(false);
            }
        });
        return () => controller.abort();
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, [...deps, tick]);

    const reload = useCallback(() => setTick((n) => n + 1), []);
    return { data, error, loading, reload };
}

// A one-off action with its own abort signal, for button handlers.
export async function run<T>(action: (signal: AbortSignal) => Promise<CallResult<T>>): Promise<CallResult<T>> {
    const controller = new AbortController();
    return await action(controller.signal);
}

// --- the signed-in session ----------------------------------------------------

export type SessionInfo = {
    readonly me: Me;
    // Whether the server's route table for this holder includes the route. The
    // table is anvil's own answer, built with the same check its filter runs,
    // so it cannot disagree with what a call will do. An affordance, never a
    // refusal: the server decides.
    readonly affords: (route: AffordableRoute) => boolean;
    readonly superadmin: boolean;
    readonly signOut: () => void;
    readonly refresh: () => void;
};

const SessionContext = createContext<SessionInfo | null>(null);

export function useSession(): SessionInfo {
    const session = useContext(SessionContext);
    if (session === null) { throw new Error("useSession outside the signed-in shell"); }
    return session;
}

export type SessionState =
    | { readonly status: "loading" }
    | { readonly status: "signed-out"; readonly message: string }
    | { readonly status: "signed-in"; readonly me: Me };

// Loads the session (the holder's route table) and the identity behind it.
export async function openSession(signal: AbortSignal): Promise<Me | null> {
    const view = await platform.session.load(signal);
    if (view === null) { return null; }
    const me = await platform.api.call(routeIdentityMe, { signal });
    return me.ok ? me.value : null;
}

export function SessionProvider(props: {
    readonly me: Me;
    readonly onSignedOut: (message: string) => void;
    readonly onRefreshed: (me: Me) => void;
    readonly children: ReactNode;
}): ReactNode {
    const { me, onSignedOut, onRefreshed } = props;
    const info: SessionInfo = {
        me,
        affords: (route) => platform.session.affords(route),
        superadmin: me.type === "superadmin",
        signOut: () => {
            void platform.signOut().finally(() => onSignedOut(""));
        },
        refresh: () => {
            const controller = new AbortController();
            void openSession(controller.signal).then((next) => {
                if (next === null) { onSignedOut("Your session has ended. Please sign in again."); }
                else { onRefreshed(next); }
            });
        },
    };
    return <SessionContext.Provider value={info}>{props.children}</SessionContext.Provider>;
}

export function ErrorBanner({ message }: { readonly message: string | null }): ReactNode {
    if (message === null || message === "") { return null; }
    return (
        <div role="alert" style={{ background: "#FFF0F0", border: "2.5px solid #E53935", padding: "12px 16px", color: "#E53935", fontSize: "14px", fontWeight: 700 }}>
            {message}
        </div>
    );
}

export function formatDate(ms: number): string {
    return new Date(ms).toLocaleString(undefined, { year: "numeric", month: "short", day: "numeric", hour: "2-digit", minute: "2-digit" });
}
