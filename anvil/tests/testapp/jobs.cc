// The reference application's job bodies.
//
// Declared in anvil_app_config.h and defined here, which is the shape a real
// application uses: the table stays constexpr because it only takes addresses of
// declared functions, and the bodies stay out of the low layer because the linker
// binds them rather than the header including them.
//
// Both are trivially idempotent, which is not incidental — every queue in this
// system is at-least-once, a crashed worker's claim is RECLAIMED rather than
// lost, and a handler that is not idempotent will eventually run twice on the
// same envelope.

#include "anvil_app_jobs.h"
#include "chat_push.h"

namespace anvil::config {

timer::JobOutcome sweep_handler(const timer::JobRunContext& ctx) noexcept {
    static_cast<void>(ctx);
    return timer::JobOutcome::Done;
}

timer::JobOutcome fanout_handler(const timer::JobRunContext& ctx) noexcept {
    // An empty argument blob is a malformed envelope, and four more attempts will
    // not make it parse. Permanent failure, not retry: retrying a malformed job is
    // a denial of service against our own database.
    if (ctx.args.empty()) { return timer::JobOutcome::Failed; }
    return timer::JobOutcome::Done;
}

timer::JobOutcome chat_push_handler(const timer::JobRunContext& ctx) noexcept {
    const chat::ChatPush* push = testapp::installed_chat_push().load(std::memory_order_acquire);
    // Claimed before boot finished building the push: not a malformed job, and
    // in a moment it will run.
    if (push == nullptr) { return timer::JobOutcome::Retry; }
    return push->run(ctx);
}

}  // namespace anvil::config
