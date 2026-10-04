// This application's Argon2 worker: the entry its bundler turns into the URL
// `Platform.prehashWorker` constructs. Two lines, as `hammer/prehash-worker`
// says, and the only file here that runs off the main thread.
//
// `self` is typed as a Window under the DOM library this project compiles
// against; in a dedicated worker it is the worker's global scope, whose
// `postMessage` takes a transfer list and no target origin.

import type { WorkerScope } from "hammer/state";
import { serveArgon2Pool } from "hammer/prehash-worker";

serveArgon2Pool(self as unknown as WorkerScope);
