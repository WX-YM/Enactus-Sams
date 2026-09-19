#pragma once

// Outbound delivery: who gets a transport, in what language, and what a failure
// costs the endpoint.
//
// One job per notification reaches here, never one per subscriber — a broadcast
// to 20 000 endpoints must not become 20 000 queue entries. The audience is paged
// inside this call instead.
//
// --- every transport is best-effort, and that is a DESIGN POSITION -----------
//
// The inbox row was written before any of this ran. Nothing here can fail in a
// way that loses a notification, which is what lets a delivery be abandoned the
// moment it stops being worth attempting: a push endpoint that answers 410 is
// disabled on the spot, and the reader still has the notification.
//
// --- a failure disables the ENDPOINT, not the notification -------------------
//
//   Delivered   the streak resets. A transient failure that recovers must not
//               accumulate across weeks into a disable.
//   Gone        410 or 404 from a push service, 410 from a webhook. The endpoint
//               told us it is finished; there is nothing to retry into and a
//               streak would only delay believing it.
//   Rejected    an unusable address, a missing key, an SSRF refusal. Retrying
//               cannot fix a configuration, so it is not retried.
//   Transient   a timeout, a 5xx, a network error. Counted, and at
//               kMaxSoftFailures the endpoint is disabled — a permanently broken
//               webhook that retries forever is a self-inflicted outbound flood
//               aimed at somebody else's infrastructure.
//
// --- idempotent, because the queue is at-least-once --------------------------
//
// A redelivered job re-sends. That is accepted rather than prevented: the
// alternative is a per-endpoint completion record, which is a write per endpoint
// per notification — more storage and more write load than the duplicate it
// avoids. Web Push and email are both already at-least-once to the client, and a
// webhook receiver is told to be idempotent because every webhook receiver has to
// be anyway.
//
// --- threading --------------------------------------------------------------
//
// BLOCKING throughout: MongoDB, then a network round trip per endpoint. This runs
// on a job worker, never on a Trantor loop thread and never on db_pool — a
// mailbox that takes thirty seconds to answer would otherwise hold a database
// connection for thirty seconds.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <mongocxx/client.hpp>

#include "anvil/core/locale.h"
#include "anvil/core/perm_set.h"
#include "anvil/core/result.h"
#include "anvil/core/types.h"
#include "anvil/db/codec.h"
#include "anvil/notifications/publish.h"
#include "anvil/notifications/record.h"
#include "anvil/notifications/repository.h"
#include "anvil/notifications/template_spec.h"
#include "anvil/notifications/topic_spec.h"

namespace anvil::notifications {

// One attempt on one endpoint, with everything the transport needs and nothing
// else.
//
// `content` is rendered per recipient rather than once per notification, because
// two subscribers to the same topic do not necessarily read the same language.
// That is a render per endpoint — a table lookup and a bounded substitution, no
// allocation beyond the two strings, and nowhere near the cost of the network
// round trip it precedes.
struct Delivery final {
    Rendered                      content;
    // The push endpoint, mailbox or webhook URL. Borrowed from the client row,
    // which outlives the call.
    std::string_view              address;
    // Web-push keys or the webhook signing secret, SEALED exactly as stored. A
    // transport unseals what it needs; this struct never holds plaintext key
    // material, so a Delivery in a core dump or a log line discloses nothing.
    std::span<const std::uint8_t> keys;
    Uuid                          client;
    Uuid                          notification;
    std::int32_t                  count;
    TopicCode                     kind;
    ClientType                    type;
};

// One attempt. BLOCKING — it is a network round trip.
//
// Returning a Failure rather than a verdict means the transport could not form an
// attempt at all (a missing configuration, an exception caught at the boundary);
// it is recorded as Rejected, because a request that was never made cannot be
// blamed on the endpoint but also will not succeed on a retry.
using Transport = std::function<Result<DeliveryVerdict>(const Delivery& delivery)>;

// One per ClientType that leaves the process. InApp has none: its delivery IS the
// inbox row, which was written before this ran.
//
// An absent transport means the deployment does not do that channel. Its
// endpoints are SKIPPED rather than failed — a deployment with no SMTP configured
// has not got a broken mail endpoint, it has no mail.
struct Transports final {
    Transport web_push;
    Transport email;
    Transport webhook;
};

// The reader's language, for rendering. anvil does not know where an application
// keeps it — it is on the account row, which is the application's.
//
// Absent means every delivery renders in the default locale, which is correct for
// a single-locale deployment and wrong in a way somebody will notice immediately
// in any other.
//
// BLOCKING: db_pool. Called once per endpoint, so an implementation that queries
// per call is an N+1 across the audience; cache it.
using ReaderLocale =
    std::function<Locale(mongocxx::client& client, const std::optional<Uuid>& owner)>;

struct OutboundHooks final {
    ReaderLocale   locale_of;
    // The same question the publish path asks, asked again here for the same
    // reason: a subscriber who lost the topic's permission between the fan-out
    // and the send must not be mailed about it. Separate probes rather than a
    // shared one, because the two run on different pools at different times.
    RecipientProbe may_receive;
};

struct OutboundSummary final {
    std::int32_t attempted;
    std::int32_t delivered;
    std::int32_t disabled;
    std::int32_t skipped;
};

// How many endpoints one send will attempt before giving up on the rest.
//
// A ceiling and not a page size: the audience is paged at kSubscriberPage
// regardless, and this bounds the TOTAL so one notification cannot occupy a job
// worker indefinitely. Past it the send stops and reports what it did; the row
// stays dispatched, because the inbox already has it and the transports are
// best-effort by construction.
inline constexpr std::int32_t kMaxDeliveriesPerSend = 5000;

class OutboundSender final {
public:
    OutboundSender(const NotificationRepository& repository,
                   std::span<const TemplateSpec> templates, Transports transports,
                   OutboundHooks hooks) noexcept
        : repository_{repository},
          templates_{templates},
          transports_{std::move(transports)},
          hooks_{std::move(hooks)} {}

    // Deliver one notification to every endpoint that should receive it.
    //
    // `channels` is the mask read off the row by the dispatch that enqueued this,
    // so a publish that narrowed its channels is still narrowed here — the job
    // carries the decision rather than recomputing it from the topic.
    //
    // A notification that has expired is not an error and not a delivery:
    // e-mailing somebody about something their inbox no longer shows is worse
    // than not mailing them.
    [[nodiscard]] Result<OutboundSummary> send(mongocxx::client& client,
                                               const Uuid& notification, ChannelMask channels,
                                               db::TimeMs now) const;

private:
    // One endpoint, from the decision through the attempt to the bookkeeping.
    // Returns false when the endpoint was skipped before any attempt was made.
    [[nodiscard]] Result<bool> attempt(mongocxx::client& client, const NotificationRow& row,
                                       const TopicSpec& spec, const ClientTarget& target,
                                       ChannelMask channels, db::TimeMs now,
                                       OutboundSummary& summary) const;

    [[nodiscard]] const Transport* transport_for(ClientType type) const noexcept;

    // What the verdict costs the endpoint. Separated because it is the half that
    // has to be right when the transport half is a stub in a test.
    [[nodiscard]] Status record(mongocxx::client& client, const ClientTarget& target,
                                DeliveryVerdict verdict, db::TimeMs now,
                                OutboundSummary& summary) const;

    const NotificationRepository& repository_;
    std::span<const TemplateSpec> templates_;
    Transports                    transports_;
    OutboundHooks                 hooks_;
};

}  // namespace anvil::notifications
