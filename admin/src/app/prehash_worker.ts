// The Argon2 worker the sign-in and enrolment prehash run in, off the main
// thread (hammer/prehash-worker). The bundler turns this file into the URL
// platform.ts constructs.

import type { WorkerScope } from "hammer/state";
import { serveArgon2Pool } from "hammer/prehash-worker";

serveArgon2Pool(self as unknown as WorkerScope);
