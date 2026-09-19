// Which resources a write makes stale.
//
// Partial rather than total, and deliberately the one of the three tables that
// is: a read invalidates nothing, so an entry for every route would be a table of
// empty arrays whose only effect is to hide the routes that matter. Both sides
// are still real route ids, so a mutation naming a resource the server retired
// stops compiling rather than silently ceasing to invalidate anything.

import type { Invalidations } from "hammer";

import type { RouteId } from "../api/hammer.generated.js";

// #region invalidations
// src/app/invalidate.ts
export const invalidations = {
    "content.delete": ["content.get"],
    "media.delete": ["media.list"],
} as const satisfies Invalidations<RouteId>;
// #endregion
