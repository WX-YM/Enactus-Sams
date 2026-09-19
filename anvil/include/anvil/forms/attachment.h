#pragma once

// How an `Attachment` field resolves, as three hooks the application supplies.
//
// anvil cannot answer any of the three questions itself, and each one is a
// different kind of not-knowing:
//
//   MAY THIS SUBMITTER BIND THIS OBJECT?  Which storage namespace a form's
//   attachments live in is the application's table, and what "owned by" means for
//   a submitter with no account at all is its policy. Getting this wrong is an
//   IDOR with a confidentiality impact: a submitter references somebody else's
//   private image by id, and an authenticated staff member's review screen
//   renders it. It must therefore answer with the SAME stealth response for "does
//   not exist" and "is not yours" — which media ids exist is not something a
//   failed submission confirms.
//
//   BIND / RELEASE.  Reference counting belongs to whatever subsystem owns the
//   object. Both take the caller's session because the count must move inside the
//   SAME transaction as the row that owns it: a count that commits while the
//   submission aborts is a leak no sweep can distinguish from a live reference,
//   and the reverse is a file deleted out from under a live row.
//
// All three empty means the application declares no attachment field type, and a
// submission carrying one is refused. That is the correct direction to fail —
// accepting an id nothing will ever reference is how a file becomes unreachable
// and uncollectable at the same time.
//
// std::function rather than a function pointer, because a real implementation
// closes over a MediaService and a namespace. The allocation is one per service
// at construction, never on a request path.

#include <functional>
#include <optional>

#include <mongocxx/client.hpp>
#include <mongocxx/client_session.hpp>

#include "anvil/core/result.h"
#include "anvil/core/types.h"

namespace anvil::forms {

// BLOCKING: db_pool. `submitter` is absent for a public, unauthenticated
// submission. False means "refuse", and the caller turns that into the same
// not-found every other attachment failure produces.
using AttachmentResolver =
    std::function<Result<bool>(mongocxx::client& client, const Uuid& attachment,
                               const std::optional<Uuid>& submitter)>;

// +1 and -1, inside the caller's transaction.
using AttachmentBinder = std::function<Status(mongocxx::client& client,
                                              mongocxx::client_session& session,
                                              const Uuid& attachment)>;

struct AttachmentHooks final {
    AttachmentResolver may_bind;
    AttachmentBinder   bind;
    AttachmentBinder   release;

    // All three or none. A half-supplied set is a configuration mistake that
    // would otherwise surface as a submission that binds an object nothing ever
    // releases, and the asymmetry is invisible until the sweeper does not run.
    [[nodiscard]] bool complete() const noexcept {
        return static_cast<bool>(may_bind) && static_cast<bool>(bind) &&
               static_cast<bool>(release);
    }
    [[nodiscard]] bool empty() const noexcept {
        return !may_bind && !bind && !release;
    }
};

}  // namespace anvil::forms
