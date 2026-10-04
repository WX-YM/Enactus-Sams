// The one send that survives the page going away.
//
// A `fetch` from an unloading document is cancelled by the browser without an
// error. That is how a funnel loses precisely its last step — the one every
// drop-off analysis is about — and it is silent, so the data looks complete and
// is wrong in one direction only (`docs/01-seams.md` §10).
//
// `sendBeacon` is the platform's answer: the request is handed to the browser,
// which sends it after the document is gone. It comes with two properties a
// caller has to know rather than discover:
//
//   IT CAN SAY NO. The return is false when the payload is over the user agent's
//   queue limit, which is 64 KB in every engine that documents one. A caller
//   that ignored it would drop a batch and believe it had sent one.
//
//   IT CARRIES NO HEADERS. No `Idempotency-Key`, no `Capability`, no `Accept`.
//   That is the whole reason it is confined to this one use: an analytics batch
//   is the only thing this library sends that needs none of them.
//
// It is here rather than beside the sink that uses it because `sendBeacon` may
// be named in `src/wire/` and nowhere else (`tools/check-wire-discipline.sh`) —
// one transport, one place for the credential posture to be right.

// The slice of `navigator` this needs, injected rather than read, for the reason
// `wire/leader.ts` gives about the lock manager: a global the platform imposes
// arrives as a parameter, so a test supplies its own instead of racing every
// other test in the file (`CLAUDE.md` §3.3).
export type BeaconSender = {
    readonly sendBeacon: (url: string, data: BodyInit) => boolean;
};

// Returns false when the browser would not take it.
export type Beacon = (body: string) => boolean;

// The URL is closed over rather than passed per call, so a caller cannot send a
// beacon somewhere this deployment did not name.
export function beaconFrom(sender: BeaconSender, url: string): Beacon {
    return (body) => {
        try {
            // A `Blob` with an explicit type rather than a bare string: a string
            // is sent as `text/plain;charset=UTF-8`, which a server reading JSON
            // either rejects or has to be configured to ignore — and configuring
            // a server to ignore a content type is how it ends up ignoring one
            // that mattered.
            return sender.sendBeacon(url, new Blob([body], { type: "application/json" }));
        } catch {
            // `sendBeacon` throws where a Content Security Policy refuses the
            // destination. A refusal to send analytics must never be a refusal
            // to unload the page.
            return false;
        }
    };
}

// For a platform with no `sendBeacon`, and for a test that wants the sink's
// behaviour when the browser says no. Refusing is one honest answer; pretending
// to have sent is not.
export const noBeacon: Beacon = () => false;
