// A worker pool's factory, as an application writes it: the platform's own
// `Worker`, handed over with no cast.
//
// `WorkerLike` promises that the platform's `Worker` satisfies it
// structurally. For as long as nothing wrote this, the promise was false — the
// transfer list was declared `readonly` and optional, the platform declares it
// mutable and required, and every application would have needed a cast.
// Type-checking this file is what keeps the promise.
//
// The URL is the application's: only its bundler can turn a worker entry into
// one, which is why hammer takes a factory rather than a path.

import type { WorkerFactory } from "hammer/state";

export function moduleWorker(url: URL): WorkerFactory {
    return () => new Worker(url, { type: "module" });
}
