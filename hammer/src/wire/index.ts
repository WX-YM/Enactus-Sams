// The `hammer/wire` entry point.
//
// Exports are named and explicit, for the reason the core entry point gives: a
// star re-export puts every module in every bundle that touches any of them.

export type {
    HttpMethod,
    PermissionBits,
    RouteTarget,
    SessionDecodeError,
    SessionView,
} from "./session_view.js";
export { decodeSessionView, isRoutePath, parseRouteTarget, staleClient } from "./session_view.js";
export type {
    BuiltRoute,
    PathParams,
    QueryScalar,
    QueryValue,
    RouteBuildError,
    RouteQuery,
    RouteRequest,
} from "./route.js";
export { buildRoute, requestPath } from "./route.js";
export type { Beacon, BeaconSender } from "./beacon.js";
export { beaconFrom, noBeacon } from "./beacon.js";
export type { CacheControl } from "./cache_control.js";
export { kNoDirectives, parseCacheControl } from "./cache_control.js";
export type { ErrorVocabulary, ResponseFacts } from "./envelope.js";
export { decodeEnvelope, errorVocabulary } from "./envelope.js";
export type { ResolvableRoute, ResolveError, SessionSource } from "./resolve.js";
export { resolveRoute } from "./resolve.js";
export type { AffordableRoute } from "./affordance.js";
export { affordsRoute, holdsAll } from "./affordance.js";
export type { ApiOrigin, ApiOriginError, ApiOriginRequest } from "./origin.js";
export { defineApiOrigin, requestUrl } from "./origin.js";
// `mintCapability` and `capabilityToken` are deliberately absent, the way
// `cursorFromServer` is from the core entry point: a consumer that can mint one
// can satisfy a destructive route with a value no server issued, and a consumer
// that can read the token can put it in a log line. The only public producer is
// the client's mint call.
export type { Capability, CapabilityError } from "./capability.js";
// The key type is public because a call's observability record names it; the
// mint is not, for the reason above.
export type { IdempotencyKey } from "./idempotency.js";
export type { BucketSpec, RateLimitTable } from "./rate_limit.js";
export { RateLimiter } from "./rate_limit.js";
export type { BreakerConfig, BreakerState } from "./breaker.js";
export { CircuitBreaker, kDefaultBreaker } from "./breaker.js";
export type { RetryAction, RetryFacts, RetryPolicy, UnitRandom } from "./retry.js";
export { fullJitter, kDefaultRetryPolicy, parseRetryAfter, randomUnit, retryAction } from "./retry.js";
export type { Sleep } from "./schedule.js";
export { sleep } from "./schedule.js";
export type { MonotonicClock, QueueConfig, QueuePressure } from "./queue.js";
export { RequestQueue, kDefaultQueue } from "./queue.js";
export type { ExclusiveLocks, FanOut, Leadership, LockGrant } from "./leader.js";
export { channelFanOut, leadership, locksFrom, noFanOut } from "./leader.js";
export type { CredentialCounts, CredentialsConfig, RefreshOutcome } from "./credentials.js";
export { Credentials, credentials, kDefaultLeaderWaitMs, kRefreshLock } from "./credentials.js";
export type {
    ApiTables,
    ApiTypes,
    CallOptions,
    CallResult,
    CallableRoute,
    ClientConfig,
    ClientCount,
    FetchLike,
    RequestBody,
    RequestRecord,
    SendBody,
    SendOptions,
    Telemetry,
} from "./client.js";
export { Client, createClient } from "./client.js";
export type { Stream, StreamClosed, StreamConfig, StreamEvent } from "./sse.js";
export { kStreamRingSlots, openStream } from "./sse.js";
export type { UploadFile, UploadLimits, UploadOptions, UploadProgress } from "./upload.js";
export { checkUpload, supportsRequestStreams } from "./upload.js";
