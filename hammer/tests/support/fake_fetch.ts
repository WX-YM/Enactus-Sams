// A server, as a function, with a script.
//
// Not a mocking framework (docs/16-test-plan.md): a mock configured to return
// what the test expects asserts that the test knows what it expects. This
// answers with real `Response` objects, so the decode, the header reads and the
// body parse under test are the ones that run in a browser — and it records what
// it was ASKED, because half of what the client has to get right is in the
// request rather than in the answer.

import type { FetchLike } from "../../src/wire/client.js";

export type Recorded = {
    readonly url: string;
    readonly method: string;
    readonly headers: Readonly<Record<string, string>>;
    readonly body: BodyInit | null;

    // How many bytes the body turned out to be, for a streamed one, and null
    // for every other kind. A stand-in that only recorded the object was blind
    // to the one thing a streamed body does that no other body does — it is
    // CONSUMED by the send — so a retried upload handing the same stream over
    // twice looked identical to one handing over two.
    readonly bodyBytes: number | null;

    // The stream had already been read. The platform throws on this, so this
    // does too, and the attempt is recorded before the throw because it is an
    // attempt the client made.
    readonly bodyDisturbed: boolean;

    readonly credentials: RequestCredentials | undefined;
    readonly redirect: RequestRedirect | undefined;
};

export type Answer = {
    readonly status?: number;
    readonly body?: unknown;
    readonly headers?: Readonly<Record<string, string>>;

    // A failure with no response at all: a dropped connection, a refused
    // redirect, a DNS failure. `fetch` rejects for every one of them.
    readonly transport?: boolean;

    // Text that is not JSON, for a proxy's HTML error page.
    readonly text?: string;

    // Held until the test releases it, so "what is in flight right now" is an
    // assertion rather than a race.
    readonly until?: Promise<void>;
};

// Every byte of a stream body, or null when the stream was already locked or
// already read. `getReader()` is what throws in both cases, and both cases mean
// the same thing here: somebody sent this body twice.
async function drain(stream: ReadableStream<Uint8Array>): Promise<number | null> {
    let reader: ReadableStreamDefaultReader<Uint8Array>;
    try {
        reader = stream.getReader();
    } catch {
        return null;
    }
    let bytes = 0;
    for (;;) {
        const chunk = await reader.read();
        if (chunk.done) {
            return bytes;
        }
        bytes += chunk.value.byteLength;
    }
}

export class FakeServer {
    readonly requests: Recorded[] = [];

    private readonly script: Answer[] = [];
    private fallback: Answer = { status: 200, body: {} };

    // Each call is answered by the next scripted answer, then by the fallback.
    reply(...answers: readonly Answer[]): this {
        this.script.push(...answers);
        return this;
    }

    always(answer: Answer): this {
        this.fallback = answer;
        return this;
    }

    get calls(): number {
        return this.requests.length;
    }

    header(at: number, name: string): string | undefined {
        return this.requests[at]?.headers[name];
    }

    readonly fetch: FetchLike = async (url, init) => {
        const headers: Record<string, string> = {};
        for (const [name, value] of Object.entries(
            (init.headers ?? {}) as Record<string, string>,
        )) {
            headers[name] = value;
        }

        // A server READS the request body, and reading is what makes a stream
        // body one-shot. Draining it here is what lets a test see the
        // difference between an upload that was retried with a second body and
        // one that was retried with the empty remains of the first.
        const sent = (init.body ?? null) as BodyInit | null;
        let bodyBytes: number | null = null;
        let bodyDisturbed = false;
        if (sent instanceof ReadableStream) {
            const drained = await drain(sent as ReadableStream<Uint8Array>);
            bodyBytes = drained;
            bodyDisturbed = drained === null;
        }

        this.requests.push({
            url,
            method: init.method ?? "GET",
            headers,
            body: sent,
            bodyBytes,
            bodyDisturbed,
            credentials: init.credentials,
            redirect: init.redirect,
        });

        if (bodyDisturbed) {
            // What a browser does with a body stream a previous attempt already
            // drank: `fetch` rejects with a TypeError, which is indistinguishable
            // from a dropped connection by the time it reaches the client.
            throw new TypeError("the request body stream has already been read");
        }

        const answer = this.script.shift() ?? this.fallback;
        if (answer.until !== undefined) {
            await answer.until;
        }

        if (init.signal?.aborted === true) {
            throw new DOMException("aborted", "AbortError");
        }
        if (answer.transport === true) {
            throw new TypeError("failed to fetch");
        }

        const status = answer.status ?? 200;
        const body =
            answer.text !== undefined
                ? answer.text
                : answer.body === undefined
                  ? null
                  : JSON.stringify(answer.body);

        return new Response(status === 204 || status === 304 ? null : body, {
            status,
            headers: { "Content-Type": "application/json", ...answer.headers },
        });
    };
}

// A promise a test opens by hand, for an answer that must still be in flight
// while something else is asserted.
export function gate(): { readonly until: Promise<void>; readonly open: () => void } {
    let open = (): void => {};
    const until = new Promise<void>((resolve) => {
        open = resolve;
    });
    return { until, open: () => open() };
}
