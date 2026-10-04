#pragma once

// The reference application's notification topics and message templates.
//
// This is what every application built on anvil writes, and it is compiled by
// every build of the test suite — so the worked example in docs/01-seams.md §7 is
// a file that must keep compiling rather than a snippet that can rot.
//
// Two tables and one numbering each. Both are STORED: a topic code is written on
// every notification row and is a bit position inside every client's preference
// masks, and a template id is written beside it. Neither may ever be renumbered
// or reused, for the reason permission bits and locale indices may not.

#include <array>
#include <cstddef>
#include <cstdint>

#include "anvil/core/perm_set.h"
#include "anvil/notifications/template_spec.h"
#include "anvil/notifications/topic_spec.h"
#include "perms.h"

namespace testapp {

namespace n = anvil::notifications;
using n::ClientType;
using n::FanOut;

// `Scope` is deliberately NOT imported. capabilities.h declares an unrelated
// `testapp::Scope` for its capability scopes, and a `using` at namespace scope
// here made the two collide the moment one translation unit included both —
// which nothing did until the descriptor emitter needed every table at once.
// The topic scope is spelled `n::Scope` below; the capability one keeps the
// short name it already had.

// --- topics -----------------------------------------------------------------

enum class Topic : n::TopicCode {
    ContentPublished = 0,
    FormSubmitted    = 1,
    FormAccepted     = 2,
    SessionNewDevice = 3,
    SystemAnnouncement = 4,
    StaffAssigned    = 5,
    ChatMessage      = 6,
};

inline constexpr std::array<n::TopicSpec, 7> kTopics{{
    // Broadcast: ONE row whatever the audience. This is the whole reason the
    // read-merge exists, and it is why a topic's fan-out is compile-time.
    // Webhook is in the mask because an ungated broadcast is the one topic shape
    // an external integration can legitimately subscribe to with no account
    // behind it: a permission-gated topic delivered to an ownerless endpoint has
    // nobody to check the permission against, and fails closed.
    {"content.published", anvil::PermSet{}, 90, 0, 0, FanOut::Read,
     n::channels(ClientType::InApp, ClientType::WebPush, ClientType::Webhook),
     n::Scope::Global, false, true},

    // Staff-facing, coalesced, and STEALTH on denial: a form under load produces
    // one row per ten minutes with a count rather than one row per submission,
    // and the existence of the topic is itself what the permission protects.
    {"form.submitted", anvil::perm_mask(Perm::FormRead), 90, 600, 1, FanOut::Read,
     n::channels(ClientType::InApp, ClientType::Email, ClientType::Webhook),
     n::Scope::Resource, true, true},

    // Targeted at one applicant, so fan-out on write is correct here even though
    // its sibling above is not.
    {"form.accepted", anvil::PermSet{}, 180, 0, 2, FanOut::Write,
     n::channels(ClientType::InApp, ClientType::Email), n::Scope::Resource, false, true},

    // Security, and therefore NOT user_optional: an account that can silence its
    // own "new sign-in" alert has no alert, which is precisely the state an
    // attacker wants it in. The storm breaker will not shed it either.
    //
    // ACCOUNT-scoped: the subject IS the account signed into. That is what makes
    // the transport walk find only that account's own endpoints — an unscoped
    // subject here would match every client subscribed to the kind and mail one
    // person's sign-in to everybody.
    {"session.new_device", anvil::PermSet{}, 365, 0, 3, FanOut::Write,
     n::channels(ClientType::InApp, ClientType::Email), n::Scope::Account, false, false},

    {"system.announcement", anvil::perm_mask(Perm::SystemAnnounce), 365, 0, 4, FanOut::Read,
     n::channels(ClientType::InApp, ClientType::WebPush, ClientType::Email),
     n::Scope::Global, false, true},

    // The one combination the other five do not cover: TARGETED and
    // PERMISSION-GATED at once. It exists because that is the combination where
    // getting the fan-out wrong is a disclosure rather than noise — every
    // subscriber to this topic gets a row of their own, so a subscriber who has
    // lost the bit since subscribing receives a staff notification about a form
    // they may no longer read.
    //
    // The permission is therefore rechecked at DISPATCH, against the recipient,
    // and not trusted from whenever the subscription was written.
    {"staff.assigned", anvil::perm_mask(Perm::StaffManage), 90, 0, 5, FanOut::Write,
     n::channels(ClientType::InApp, ClientType::Email), n::Scope::Resource, true, true},

    // Chat's push nudges (anvil/chat/push.h). Nothing is ever published to it:
    // it exists so a push says what it is to the service worker, and so a
    // device's preferences can turn chat pushes off. WebPush only, because the
    // chat list is chat's inbox and a nudge writes no row.
    {"chat.message", anvil::PermSet{}, 30, 0, 6, FanOut::Write,
     n::channels(ClientType::WebPush), n::Scope::Account, false, true},
}};

static_assert(n::topic_table_is_well_formed(kTopics),
              "empty key, a duplicate key or code, a code past the preference masks, a topic "
              "with no default channel, or a zero retention");
static_assert(n::topics_are_dense_from_zero(kTopics),
              "the lookup is a direct index; a sparse table turns it into a scan per publish");
static_assert(kTopics.size() == 7,
              "adding a topic is a deliberate act: the code is stored on every row and is a "
              "bit position in every client's preferences");

// The enum and the table are two spellings of one numbering, so they are checked
// against each other rather than kept in step by hand.
static_assert(n::topic_spec(kTopics, static_cast<n::TopicCode>(Topic::SessionNewDevice))->key ==
                  "session.new_device");
static_assert(!n::topic_spec(kTopics,
                             static_cast<n::TopicCode>(Topic::SessionNewDevice))->user_optional,
              "a security topic the reader can mute is a security topic with no alert");

// --- templates --------------------------------------------------------------
//
// One message per locale, and the placeholder SET must match across all of them.
// The Arabic strings are not decoration: they are what proves the UTF-8 check and
// the order-insensitive placeholder comparison actually run, because Arabic
// phrasing legitimately puts `{t}` somewhere English does not.

enum class Template : n::TemplateId {
    ContentPublished = 0,
    FormSubmitted    = 1,
    FormAccepted     = 2,
    SessionNewDevice = 3,
    Announcement     = 4,
    ChatPreview      = 5,
    ChatPlain        = 6,
};

inline constexpr std::array<n::TemplateSpec, 7> kTemplates{{
    {{{"New post", "منشور جديد"}}, {{"{t} was published", "تم نشر {t}"}}, 0, 1},

    // Coalesced, so the count is part of the sentence rather than an afterthought:
    // "3 new submissions" is the whole reason coalescing exists.
    {{{"New form submissions", "ردود جديدة على النموذج"}},
     {{"{n} new submissions on {t}", "{n} ردود جديدة على {t}"}}, 1, 2},

    {{{"Application accepted", "تم قبول الطلب"}},
     {{"Your application to {t} was accepted", "تم قبول طلبك في {t}"}}, 2, 1},

    // Security. `{d}` is a COARSE label built server-side, never the raw
    // User-Agent: echoing that back is a stored-XSS vector in a mail client, and
    // it tells an attacker exactly what is being fingerprinted.
    {{{"New sign-in", "تسجيل دخول جديد"}},
     {{"A new sign-in from {d}", "تسجيل دخول جديد من {d}"}}, 3, 1},

    // The announcement's own text IS the parameter, so an operator can say
    // anything without a deploy — and it is escaped at render like every other
    // untrusted string.
    {{{"Announcement", "إعلان"}}, {{"{b}", "{b}"}}, 4, 1},

    // A chat nudge (anvil/chat/push.h). `{t}` is the group's title, or the
    // sender's name in a direct conversation; `{b}` is the newest message's
    // text and is bound only for a reader who keeps previews on. The plain one
    // is what a reader who turned them off is sent, so it has no `{b}` at all.
    {{{"{t}", "{t}"}}, {{"{s}: {b}", "{s}: {b}"}}, 5, 3},
    {{{"{t}", "{t}"}}, {{"{n} new messages", "{n} رسائل جديدة"}}, 6, 2},
}};

static_assert(n::template_table_is_well_formed(kTemplates),
              "a template must declare param_count equal to its placeholder set, must use the "
              "same placeholders in every locale, and must carry a non-empty valid-UTF-8 "
              "string for each");
static_assert(n::templates_are_dense_from_zero(kTemplates));
static_assert(kTemplates.size() == 7,
              "a template id is stored on rows that outlive several deploys");

}  // namespace testapp
