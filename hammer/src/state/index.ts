// The `hammer/state` entry point: stores, caches and the mechanisms that keep
// them honest across tabs and across a session.
//
// Exports are named and explicit, for the reason the other entry points give: a
// star re-export puts every module in every bundle that touches any of them,
// because a bundler cannot drop what it was never told is side-effect free at
// the granularity it needs.
//
// Nothing here renders. This layer names no document and no element — a script
// fails the build if it does (`tools/check-layering.sh`) — so a component that
// drives one of these stores is `hammer/dom`'s, and the store is testable with
// no document at all.

export type { Readable, Unsubscribe } from "./store.js";
export { Store } from "./store.js";
export type { CountSink, StateCount } from "./counts.js";
export { kNoCounts } from "./counts.js";
export type {
    CacheClasses,
    CacheConfig,
    CacheEntry,
    CacheKey,
    Identity,
} from "./cache.js";
export { Lru, ResourceCache, cacheKey } from "./cache.js";
export type {
    Resource,
    ResourceBody,
    ResourceFailure,
    ResourceState,
    ResourceStoreConfig,
} from "./resource.js";
export { ResourceStore, kResourceLoading } from "./resource.js";
export type { Invalidatable, InvalidatorConfig } from "./invalidate.js";
export { Invalidator } from "./invalidate.js";
export type { SessionRead, SessionState, SessionStoreConfig } from "./session.js";
export { SessionStore } from "./session.js";
export type { Page, PagedRoute, PagerConfig, PagerError, PagerState } from "./paginate.js";
export { Pager } from "./paginate.js";
export type { VersionedOutcome, VersionedWrite } from "./versioned.js";
export { writeVersioned } from "./versioned.js";
export type { OptimisticOutcome, OptimisticUpdate } from "./optimistic.js";
export { optimistic } from "./optimistic.js";
export type {
    FieldDefinition,
    FieldState,
    FieldTypeSpec,
    FieldValue,
    FormReasons,
    FormState,
} from "./forms.js";
export { Form, definitionsFrom } from "./forms.js";
export type { SectionStage, SectionsConfig } from "./sections.js";
export { Sections, labelAt } from "./sections.js";
export type { InboxConfig, InboxState, Notification } from "./inbox.js";
export { Inbox, kInboxHeld, kInboxRingSlots } from "./inbox.js";
export type {
    ImageSources,
    MediaConfig,
    MediaError,
    MediaWidths,
    UploadImageError,
    UploadImageOptions,
} from "./media.js";
export { imageSource, imageSources, uploadImage } from "./media.js";
export type {
    AnalyticsConfig,
    Consent,
    Dimensions,
    EventSpec,
    ReportedEvent,
} from "./analytics.js";
export { AnalyticsSink } from "./analytics.js";
export type {
    PoolConfig,
    PoolError,
    WorkerFactory,
    WorkerLike,
    WorkerScope,
} from "./workers/pool.js";
export { WorkerPool, servePool } from "./workers/pool.js";
export type { DownscaleRequest, DownscaleResult } from "./workers/image.js";
export { ImagePool, kImageWorkers, serveImagePool } from "./workers/image.js";
export type { DecodeTask } from "./workers/decode.js";
export { DecodePool, decodeWorkers, serveDecodePool } from "./workers/decode.js";
