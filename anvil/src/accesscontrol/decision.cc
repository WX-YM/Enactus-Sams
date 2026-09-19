#include "anvil/accesscontrol/decision.h"

#include "anvil/accesscontrol/cookies.h"

namespace anvil::accesscontrol {
namespace {

[[nodiscard]] Evaluation deny(ErrorCode code) noexcept {
    // A zeroed context, not a partially populated one. Nothing downstream reads
    // it on a denial, and leaving real ids in a struct that travels alongside a
    // denial is how a "harmless" refactor later logs the identity of a request
    // that was refused.
    return Evaluation{.ctx = {}, .code = code, .step = Step::Deny, .token_epoch = 0};
}

[[nodiscard]] ErrorCode code_for(auth::TokenError error) noexcept {
    // Every token failure is Unauthenticated. The distinctions — malformed,
    // unknown kid, bad signature, expired — matter to the audit log and are
    // preserved in the TokenError the caller logs; telling the CLIENT which one
    // it was would confirm, for instance, that a forged token's kid names a
    // real key.
    (void)error;
    return ErrorCode::Unauthenticated;
}

}  // namespace

Evaluation evaluate(std::string_view cookie_header, const RoutePolicy& policy,
                    const auth::TokenKeys& keys, const EpochResolver& epochs,
                    std::int64_t now_unix) noexcept {
    return evaluate_token(read_cookie(cookie_header, kAccessCookieName), policy, keys, epochs,
                          now_unix);
}

Evaluation evaluate_token(std::string_view token, const RoutePolicy& policy,
                          const auth::TokenKeys& keys, const EpochResolver& epochs,
                          std::int64_t now_unix) noexcept {
    if (policy.access == RouteAccess::Public) {
        // A public route still gets a context when a valid token happens to be
        // present — a signed-in visitor reading a public page should be
        // recognised — but a missing or bad token is not a failure here.
        if (token.empty()) {
            return Evaluation{.ctx = {}, .code = ErrorCode::Ok, .step = Step::Allow,
                              .token_epoch = 0};
        }
        const auth::TokenResult decoded = auth::decode(token, keys, now_unix);
        if (!decoded.ok()) {
            return Evaluation{.ctx = {}, .code = ErrorCode::Ok, .step = Step::Allow,
                              .token_epoch = 0};
        }
        // The epoch is deliberately NOT consulted on a public route. Doing so
        // would put a Redis GET behind unauthenticated traffic, which is a
        // denial-of-service lever with no security benefit: the route grants
        // nothing that a revoked permission could have unlocked.
        return Evaluation{.ctx = auth::to_context(decoded.claims),
                          .code = ErrorCode::Ok,
                          .step = Step::Allow,
                          .token_epoch = decoded.claims.perm_epoch};
    }

    if (token.empty()) { return deny(ErrorCode::Unauthenticated); }

    // Length, then tag, then expiry, then fields. A 64 KB cookie is rejected on
    // the length compare inside decode() before any base64url work happens.
    const auth::TokenResult decoded = auth::decode(token, keys, now_unix);
    if (!decoded.ok()) { return deny(code_for(decoded.error)); }

    const UserContext ctx = auth::to_context(decoded.claims);
    // Evaluated BEFORE the epoch is consulted, because on a stealth route the
    // answer decides whether this request is permitted to cost a round trip at
    // all. See the Unknown arm.
    const bool satisfied = satisfies(ctx.permissions, ctx.user_type, policy.required);

    switch (epochs.check_cached(ctx.user_id, decoded.claims.perm_epoch)) {
        case EpochVerdict::Match:
            break;
        case EpochVerdict::Mismatch:
            // The token was minted before a permission or role change. Deny;
            // the client's refresh endpoint re-mints with fresh permissions, so
            // the recovery is one round trip rather than a re-login.
            return deny(ErrorCode::Unauthenticated);
        case EpochVerdict::Unknown:
            if (policy.access == RouteAccess::Stealth && !satisfied) {
                // A DENIAL on a stealth route answers here rather than deferring,
                // and this is the stealth timing property rather than an
                // optimisation (docs/04-access-control.md §3). Deferring made a
                // denied stealth route cost a Redis GET while a NONEXISTENT route
                // cost nothing, so the two were separable with a stopwatch — which
                // is the existence oracle the whole feature exists to close.
                //
                // Answering now cannot change the outcome, only its label:
                // resume_after_epoch re-checks the mask carried by THIS token, so
                // the authority can turn an allow into a denial and never a denial
                // into an allow. The client sees the same 404 for either label, so
                // the round trip would have bought nothing it could observe.
                //
                // The audit row therefore reads Forbidden where a resolved epoch
                // might have said Unauthenticated. Forbidden is the stronger
                // statement of the two: it records what was actually checked.
                return deny(ErrorCode::Forbidden);
            }
            // Every other route kind still defers. The 401/403 distinction is
            // visible there, and a permission that was just GRANTED reaches the
            // client as 401 -> refresh -> retry; collapsing it to 403 would strand
            // the user behind a stale token until it expired.
            return Evaluation{.ctx = ctx,
                              .code = ErrorCode::Ok,
                              .step = Step::ResolveEpoch,
                              .token_epoch = decoded.claims.perm_epoch};
    }

    if (!satisfied) {
        return deny(ErrorCode::Forbidden);
    }
    return Evaluation{.ctx = ctx,
                      .code = ErrorCode::Ok,
                      .step = Step::Allow,
                      .token_epoch = decoded.claims.perm_epoch};
}

ConnectionVerdict still_authorized(const UserContext& ctx, std::uint32_t expires_at_unix,
                                   const RoutePolicy& policy, const EpochResolver& epochs,
                                   std::int64_t now_unix) noexcept {
    // The same comparison `auth::decode` makes, tolerance included. Written as
    // the same expression rather than as "now > expires_at", because a re-check
    // that were one minute stricter than the verification would close a
    // connection whose token a fresh request would still accept — and the client
    // would reconnect, hand over that same token, and be let in.
    if (static_cast<std::int64_t>(expires_at_unix) + auth::kClockSkewToleranceSeconds <
        now_unix) {
        return ConnectionVerdict::Close;
    }

    // Public grants nothing a revoked permission could have unlocked, so the
    // epoch is not consulted here either — the same refusal to put a Redis GET
    // behind unauthenticated traffic that `evaluate_token` makes.
    if (policy.access == RouteAccess::Public) { return ConnectionVerdict::Keep; }

    const bool satisfied = satisfies(ctx.permissions, ctx.user_type, policy.required);

    switch (epochs.check_cached(ctx.user_id, ctx.perm_epoch)) {
        case EpochVerdict::Match:
            return satisfied ? ConnectionVerdict::Keep : ConnectionVerdict::Close;
        case EpochVerdict::Mismatch:
            return ConnectionVerdict::Close;
        case EpochVerdict::Unknown:
            // The same early answer `evaluate_token` gives, for a DIFFERENT
            // reason that lands in the same place. There is no timing oracle on
            // an established connection — it already exists, and its existence
            // was disclosed at the handshake. What this mirrors is the rule
            // above it: the two paths must not be two paths. It also spares a
            // round trip that could only ever confirm the answer, because
            // `resume_connection_after_epoch` re-checks THIS context's mask and
            // so can turn a keep into a close and never the reverse.
            if (policy.access == RouteAccess::Stealth && !satisfied) {
                return ConnectionVerdict::Close;
            }
            return ConnectionVerdict::ResolveEpoch;
    }
    // Unreachable for any value the enum declares, and closed rather than kept:
    // a verdict this function does not understand is not a reason to hold a
    // connection open.
    return ConnectionVerdict::Close;
}

ConnectionVerdict resume_connection_after_epoch(const UserContext& ctx,
                                                const RoutePolicy& policy,
                                                std::uint64_t authoritative_epoch) noexcept {
    if (ctx.perm_epoch != authoritative_epoch) { return ConnectionVerdict::Close; }
    if (!satisfies(ctx.permissions, ctx.user_type, policy.required)) {
        return ConnectionVerdict::Close;
    }
    return ConnectionVerdict::Keep;
}

Evaluation resume_after_epoch(const Evaluation& pending, const RoutePolicy& policy,
                              std::uint64_t authoritative_epoch) noexcept {
    if (pending.token_epoch != authoritative_epoch) {
        return deny(ErrorCode::Unauthenticated);
    }
    if (!satisfies(pending.ctx.permissions, pending.ctx.user_type, policy.required)) {
        return deny(ErrorCode::Forbidden);
    }
    return Evaluation{.ctx = pending.ctx,
                      .code = ErrorCode::Ok,
                      .step = Step::Allow,
                      .token_epoch = pending.token_epoch};
}

}  // namespace anvil::accesscontrol
