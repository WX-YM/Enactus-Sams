#include "anvil/chat/routes.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <trantor/utils/Logger.h>

#include "anvil/accesscontrol/access_filter.h"
#include "anvil/accesscontrol/route_registration.h"
#include "anvil/accesscontrol/stealth.h"
#include "anvil/core/thread_pools.h"
#include "anvil/core/uuid.h"
#include "anvil/crypto/base64url.h"
#include "anvil/db/mongo_pool.h"
#include "anvil/http/client_address.h"
#include "anvil/http/errors.h"
#include "anvil/http/json_writer.h"
#include "anvil/http/origin_check.h"
#include "anvil/http/request_scope.h"
#include "anvil/http/retry_after.h"
#include "anvil/input/arena.h"
#include "anvil/input/json.h"
#include "render.h"

namespace anvil::chat {
namespace {

using drogon::HttpRequestPtr;
using drogon::HttpResponse;
using drogon::HttpResponsePtr;
using Responder = std::function<void(const HttpResponsePtr&)>;
namespace ac = accesscontrol;
namespace hj = http;
using detail::append_device_page;
using detail::append_message;
using detail::append_published_device;
using detail::append_optional_time;
using detail::append_time;
using detail::role_name;

// --- answers ----------------------------------------------------------------------

[[nodiscard]] HttpResponsePtr json(int status, std::string body) {
    HttpResponsePtr response = HttpResponse::newHttpResponse();
    response->setStatusCode(static_cast<drogon::HttpStatusCode>(status));
    response->setContentTypeString(std::string{hj::kJsonContentType});
    response->addHeader("Cache-Control", "private, no-store");
    // A page that shows a conversation carries grants in its links, and a
    // Referer would carry one to whatever the page links out to.
    response->addHeader("Referrer-Policy", "no-referrer");
    response->setBody(std::move(body));
    return response;
}

[[nodiscard]] HttpResponsePtr no_content() {
    HttpResponsePtr response = HttpResponse::newHttpResponse();
    response->setStatusCode(drogon::k204NoContent);
    response->addHeader("Cache-Control", "private, no-store");
    return response;
}

// A reason carried in Failure::detail back to the closed enum. A value outside
// it is a programming error somewhere upstream, and BadFormat is the honest
// thing to say about a field nobody can name the fault of.
[[nodiscard]] input::Reason reason_of(const Failure& failed) noexcept {
    if (failed.detail == 0 ||
        failed.detail > static_cast<std::uint16_t>(input::Reason::Breached)) {
        return input::Reason::BadFormat;
    }
    return static_cast<input::Reason>(failed.detail);
}

// What a refusal whose code alone does not say what to do next carries beside
// "error", in the one object the error body is: a reason a client switches on,
// and the field it is about. Both compile-time constants.
void append_explanation(std::string& body, std::string_view reason, std::string_view field) {
    body.pop_back();
    body += ',';
    hj::append_json_key(body, "reason");
    hj::append_json_string(body, reason);
    if (!field.empty()) {
        body += ',';
        hj::append_json_key(body, "field");
        hj::append_json_string(body, field);
    }
    body += '}';
}

// The reason a stale first-device registration carries (docs/22 §7.3.1).
inline constexpr std::string_view kFreshAuthenticationReason = "chat.fresh_authentication";

[[nodiscard]] HttpResponsePtr failure(const HttpRequestPtr& req, const Failure& failed) {
    // Not a member, never a member, no such conversation: one filter, one
    // answer, byte-identical to an unmatched route.
    if (failed.code == ErrorCode::NotFound) { return ac::not_found_response(); }
    std::string body;
    body.reserve(160);
    if (failed.code == ErrorCode::CapabilityRequired &&
        failed.field == device_inputs::kAuthenticatedAt) {
        // 428 and named, because the two things a client could otherwise take
        // it for both cost the person: a 401 is refreshed, replayed, refused
        // again and ends every tab's session; a bare 403 tells it nothing it
        // can do. What it can do is ask for a password or a passkey and retry.
        hj::append_error_body(body, failed.code, hj::request_id_of(req));
        append_explanation(body, kFreshAuthenticationReason, failed.field);
        return json(hj::http_status(failed.code), std::move(body));
    }
    if (failed.code == ErrorCode::ValidationFailed) {
        const std::array<input::FieldError, 1> fields{
            input::FieldError{failed.field, reason_of(failed)}};
        hj::append_error_body(body, failed.code, hj::request_id_of(req), fields);
    } else {
        hj::append_error_body(body, failed.code, hj::request_id_of(req));
    }
    HttpResponsePtr response = json(hj::http_status(failed.code), std::move(body));
    if (failed.code == ErrorCode::ServiceUnavailable) {
        hj::apply_retry_after(*response, hj::kShedRetryAfterSeconds);
    }
    return response;
}

[[nodiscard]] Failure invalid(std::string_view field, input::Reason reason) noexcept {
    return Failure{ErrorCode::ValidationFailed, field, static_cast<std::uint16_t>(reason)};
}

// --- parsing --------------------------------------------------------------------

// A path segment that is not a well-formed id names nothing, and answers as
// nothing does.
[[nodiscard]] std::optional<Uuid> path_id(std::string_view segment) {
    return uuid::parse(segment);
}

[[nodiscard]] std::optional<std::int64_t> integer(std::string_view text) noexcept {
    if (text.empty() || text.size() > 19) { return std::nullopt; }
    std::int64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value < 0) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::optional<std::int64_t> query_integer(const HttpRequestPtr& req,
                                                        const std::string& key) {
    const std::string& raw = req->getParameter(key);
    if (raw.empty()) { return std::nullopt; }
    return integer(raw);
}

// A `limit` query parameter, bounded while it is still 64 bits wide: a
// narrowing cast first would turn 2^32 + 1 into 1 and 2^31 into a negative.
[[nodiscard]] std::int32_t page_limit(const HttpRequestPtr& req, std::int32_t fallback,
                                      std::int32_t ceiling) {
    const std::optional<std::int64_t> asked = query_integer(req, "limit");
    if (!asked.has_value()) { return fallback; }
    return static_cast<std::int32_t>(std::clamp<std::int64_t>(*asked, 1, ceiling));
}

// --- writing ----------------------------------------------------------------------

[[nodiscard]] std::optional<Role> role_from(std::string_view name) noexcept {
    if (name == "member") { return Role::Member; }
    if (name == "admin") { return Role::Admin; }
    if (name == "owner") { return Role::Owner; }
    return std::nullopt;
}

void append_conversation(std::string& out, const ChatService& service,
                         const ConversationRecord& row) {
    out += '{';
    hj::append_json_key(out, "id");
    hj::append_json_uuid(out, row.id);
    out += ',';
    hj::append_json_key(out, "kind");
    hj::append_json_string(out, service.kind_of(row.kind).key);
    out += ',';
    hj::append_json_key(out, "encrypted");
    out += row.encrypted ? "true" : "false";
    out += ',';
    hj::append_json_key(out, "title");
    hj::append_json_string(out, row.title);
    out += ',';
    hj::append_json_key(out, "description");
    hj::append_json_string(out, row.description);
    out += ',';
    hj::append_json_key(out, "head");
    hj::append_json_int(out, row.seq);
    out += ',';
    hj::append_json_key(out, "timer");
    hj::append_json_int(out, row.timer_s);
    out += ',';
    // The fence an encrypted send names (docs/22 §7.4). A client reads it here,
    // then the device lists, never the other way round.
    hj::append_json_key(out, "dsv");
    hj::append_json_int(out, row.device_set_version);
    out += ',';
    // How many edits, revokes and reaction changes there have been (§4.5): a
    // client whose mutation cursor is behind this catches up with
    // `changed_after=`, as one whose seq cursor is behind `head` does with
    // `after=`.
    hj::append_json_key(out, "mutations");
    hj::append_json_int(out, row.mutations);
    out += ',';
    hj::append_json_key(out, "created_at");
    append_time(out, row.created_at);
    out += '}';
}

void append_membership(std::string& out, const MemberRecord& row) {
    out += '{';
    hj::append_json_key(out, "user");
    hj::append_json_uuid(out, row.user);
    out += ',';
    hj::append_json_key(out, "role");
    hj::append_json_string(out, role_name(row.role));
    out += ',';
    hj::append_json_key(out, "current");
    out += row.current() ? "true" : "false";
    out += ',';
    hj::append_json_key(out, "delivered");
    hj::append_json_int(out, row.delivered);
    out += ',';
    hj::append_json_key(out, "read");
    hj::append_json_int(out, row.read);
    out += ',';
    hj::append_json_key(out, "muted_until");
    append_optional_time(out, row.muted_until);
    out += ',';
    hj::append_json_key(out, "pinned");
    out += row.pinned ? "true" : "false";
    out += ',';
    hj::append_json_key(out, "archived");
    out += row.archived ? "true" : "false";
    out += '}';
}

[[nodiscard]] std::int64_t now_unix() noexcept {
    return std::chrono::duration_cast<std::chrono::seconds>(
               db::now_ms().time_since_epoch())
        .count();
}

// --- body binding ---------------------------------------------------------------

[[nodiscard]] std::optional<std::string_view> text_field(const input::JsonValue& body,
                                                         std::string_view key) {
    const input::JsonValue* value = body.find(key);
    if (value == nullptr) { return std::nullopt; }
    return value->as_string();
}

[[nodiscard]] Result<std::optional<std::int64_t>> integer_field(const input::JsonValue& body,
                                                                std::string_view key) {
    const input::JsonValue* value = body.find(key);
    if (value == nullptr || value->is_null()) { return std::optional<std::int64_t>{}; }
    const std::optional<std::int64_t> number = value->as_int64();
    if (!number.has_value() || *number < 0) { return invalid(key, input::Reason::BadFormat); }
    return std::optional<std::int64_t>{*number};
}

[[nodiscard]] Result<Uuid> uuid_field(const input::JsonValue& value, std::string_view field) {
    const std::optional<std::string_view> text = value.as_string();
    if (!text.has_value()) { return invalid(field, input::Reason::BadFormat); }
    const std::optional<Uuid> parsed = uuid::parse(*text);
    if (!parsed.has_value()) { return invalid(field, input::Reason::BadFormat); }
    return *parsed;
}

// Mentions bind the same way on a send and on an edit. An edit carries no
// client id, so it cannot borrow the send's binder whole.
[[nodiscard]] Result<std::vector<MentionSpan>> bind_mentions(const input::JsonValue& body) {
    std::vector<MentionSpan> out;
    const input::JsonValue* mentions = body.find(kMentionsField);
    if (mentions == nullptr || mentions->is_null()) { return out; }
    if (!mentions->is_array() || mentions->elements().size() > kMaxMentions) {
        return invalid(kMentionsField, input::Reason::BadFormat);
    }
    out.reserve(mentions->elements().size());
    for (const input::JsonValue& entry : mentions->elements()) {
        const input::JsonValue* user = entry.find("user");
        const input::JsonValue* offset = entry.find("offset");
        const input::JsonValue* length = entry.find("length");
        if (user == nullptr || offset == nullptr || length == nullptr) {
            return invalid(kMentionsField, input::Reason::BadFormat);
        }
        const Result<Uuid> who = uuid_field(*user, kMentionsField);
        const std::optional<std::int64_t> at = offset->as_int64();
        const std::optional<std::int64_t> span = length->as_int64();
        if (!who || !at.has_value() || !span.has_value() || *at < 0 || *span < 0 ||
            *at > UINT32_MAX || *span > UINT32_MAX) {
            return invalid(kMentionsField, input::Reason::BadFormat);
        }
        out.push_back(MentionSpan{who.value(), static_cast<std::uint32_t>(*at),
                                  static_cast<std::uint32_t>(*span)});
    }
    return out;
}

// Attachments bind the same way on a plaintext and an encrypted send.
[[nodiscard]] Result<std::vector<OutgoingAttachment>> bind_attachments(
    const input::JsonValue& body);

// The send body. Every view in it borrows from the parsed document, which lives
// for the duration of the work that uses it.
struct BoundSend final {
    SendMessage                     message;
    std::vector<MentionSpan>        mentions;
    std::vector<OutgoingAttachment> attachments;
};

[[nodiscard]] Result<BoundSend> bind_send(const input::JsonValue& body) {
    BoundSend out{};
    const std::optional<std::string_view> cid = text_field(body, kClientIdField);
    std::array<std::uint8_t, 16> client_id{};
    if (!cid.has_value() ||
        crypto::base64url_decode_into(*cid, client_id) != std::optional<std::size_t>{16U}) {
        return invalid(kClientIdField, input::Reason::BadFormat);
    }
    out.message.client_id = client_id;
    out.message.body = text_field(body, kBodyField).value_or(std::string_view{});

    Result<std::vector<MentionSpan>> mentions = bind_mentions(body);
    if (!mentions) { return mentions.error(); }
    out.mentions = std::move(mentions).value();

    if (const input::JsonValue* preview = body.find("preview");
        preview != nullptr && !preview->is_null()) {
        if (!preview->is_object()) { return invalid("preview", input::Reason::BadFormat); }
        out.message.preview = LinkPreview{text_field(*preview, "url").value_or(""),
                                          text_field(*preview, "title").value_or(""),
                                          text_field(*preview, "description").value_or("")};
    }

    const Result<std::optional<std::int64_t>> reply = integer_field(body, kReplyField);
    if (!reply) { return reply.error(); }
    out.message.reply_to = reply.value();

    Result<std::vector<OutgoingAttachment>> attachments = bind_attachments(body);
    if (!attachments) { return attachments.error(); }
    out.attachments = std::move(attachments).value();

    if (const input::JsonValue* card = body.find(kCardField);
        card != nullptr && !card->is_null()) {
        const std::optional<std::string_view> kind = text_field(*card, "kind");
        const input::JsonValue* card_body = card->find("body");
        if (!kind.has_value() || card_body == nullptr) {
            return invalid(kCardField, input::Reason::BadFormat);
        }
        out.message.card = SendMessage::Card{*kind, card_body};
    }
    return out;
}

// One ciphertext from unpadded base64url, bounded BEFORE it is decoded, so an
// oversized one costs a length compare and no allocation.
[[nodiscard]] Result<std::vector<std::uint8_t>> ciphertext_of(const input::JsonValue& value,
                                                              std::string_view field) {
    const std::optional<std::string_view> text = value.as_string();
    if (!text.has_value()) { return invalid(field, input::Reason::BadFormat); }
    const std::optional<std::size_t> size = crypto::base64url_decoded_size(text->size());
    if (!size.has_value()) { return invalid(field, input::Reason::BadFormat); }
    if (*size > kMaxCiphertextBytes) { return invalid(field, input::Reason::TooLong); }
    std::vector<std::uint8_t> out(*size);
    if (crypto::base64url_decode_into(*text, out) != std::optional<std::size_t>{*size}) {
        return invalid(field, input::Reason::BadFormat);
    }
    return out;
}

// The encrypted send body. The decoded ciphertexts are owned here and the
// message's spans point into them, so the spans are set after the last move.
struct BoundEncrypted final {
    EncryptedMessage                       message;
    std::vector<std::uint8_t>              common;
    std::vector<std::vector<std::uint8_t>> per_device;
    std::vector<DeviceCiphertext>          devices;
    std::vector<OutgoingAttachment>        attachments;

    void point() {
        message.ciphertext = common;
        for (std::size_t i = 0; i < devices.size(); ++i) { devices[i].ciphertext = per_device[i]; }
        message.devices = devices;
        message.attachments = attachments;
    }
};

[[nodiscard]] Result<BoundEncrypted> bind_encrypted(const input::JsonValue& body) {
    BoundEncrypted out{};
    const std::optional<std::string_view> cid = text_field(body, kClientIdField);
    if (!cid.has_value() ||
        crypto::base64url_decode_into(*cid, out.message.client_id) !=
            std::optional<std::size_t>{16U}) {
        return invalid(kClientIdField, input::Reason::BadFormat);
    }
    const input::JsonValue* device = body.find(kDeviceField);
    if (device == nullptr) { return invalid(kDeviceField, input::Reason::Required); }
    const Result<Uuid> sending = uuid_field(*device, kDeviceField);
    if (!sending) { return sending.error(); }
    out.message.device = sending.value();
    const Result<std::optional<std::int64_t>> version = integer_field(body, kDeviceSetField);
    if (!version) { return version.error(); }
    if (!version.value().has_value()) { return invalid(kDeviceSetField, input::Reason::Required); }
    out.message.device_set_version = *version.value();

    if (const input::JsonValue* common = body.find(kCiphertextField);
        common != nullptr && !common->is_null()) {
        Result<std::vector<std::uint8_t>> bytes = ciphertext_of(*common, kCiphertextField);
        if (!bytes) { return bytes.error(); }
        out.common = std::move(bytes).value();
    }
    if (const input::JsonValue* devices = body.find(kDevicesField);
        devices != nullptr && !devices->is_null()) {
        if (!devices->is_array()) { return invalid(kDevicesField, input::Reason::BadFormat); }
        if (devices->elements().size() > kMaxDeviceCiphertexts) {
            return invalid(kDevicesField, input::Reason::TooLong);
        }
        out.devices.reserve(devices->elements().size());
        out.per_device.reserve(devices->elements().size());
        for (const input::JsonValue& entry : devices->elements()) {
            const input::JsonValue* id = entry.find(kDeviceField);
            const input::JsonValue* ciphertext = entry.find(kCiphertextField);
            if (id == nullptr || ciphertext == nullptr) {
                return invalid(kDevicesField, input::Reason::BadFormat);
            }
            const Result<Uuid> target = uuid_field(*id, kDevicesField);
            if (!target) { return target.error(); }
            Result<std::vector<std::uint8_t>> bytes = ciphertext_of(*ciphertext, kDevicesField);
            if (!bytes) { return bytes.error(); }
            out.per_device.push_back(std::move(bytes).value());
            out.devices.push_back(DeviceCiphertext{{}, target.value()});
        }
    }
    if (const input::JsonValue* page = body.find(kPageField); page != nullptr) {
        const std::optional<bool> paged = page->as_bool();
        if (!paged.has_value()) { return invalid(kPageField, input::Reason::BadFormat); }
        out.message.page = *paged;
    }
    Result<std::vector<OutgoingAttachment>> attachments = bind_attachments(body);
    if (!attachments) { return attachments.error(); }
    out.attachments = std::move(attachments).value();
    return out;
}

[[nodiscard]] Result<std::vector<OutgoingAttachment>> bind_attachments(
    const input::JsonValue& body) {
    std::vector<OutgoingAttachment> out;
    if (const input::JsonValue* attachments = body.find(kAttachmentsField);
        attachments != nullptr) {
        if (!attachments->is_array() || attachments->elements().size() > kMaxAttachments) {
            return invalid(kAttachmentsField, input::Reason::BadFormat);
        }
        for (const input::JsonValue& entry : attachments->elements()) {
            if (!entry.is_object()) {
                return invalid(kAttachmentsField, input::Reason::BadFormat);
            }
            OutgoingAttachment item{};
            item.handle = text_field(entry, "handle").value_or("");
            if (const input::JsonValue* forward = entry.find("forward"); forward != nullptr) {
                const input::JsonValue* conversation = forward->find("conversation");
                const input::JsonValue* seq = forward->find("seq");
                const input::JsonValue* index = forward->find("index");
                if (conversation == nullptr || seq == nullptr || index == nullptr) {
                    return invalid(kAttachmentsField, input::Reason::BadFormat);
                }
                const Result<Uuid> from = uuid_field(*conversation, kAttachmentsField);
                const std::optional<std::int64_t> at = seq->as_int64();
                const std::optional<std::int64_t> which = index->as_int64();
                if (!from || !at.has_value() || !which.has_value() || *at < 0 || *which < 0) {
                    return invalid(kAttachmentsField, input::Reason::BadFormat);
                }
                item.forward = OutgoingAttachment::Forward{from.value(), *at,
                                                           static_cast<std::size_t>(*which)};
            }
            item.name = text_field(entry, "name").value_or("");
            const auto bounded = [&](std::string_view key, std::int64_t max) -> std::int64_t {
                const input::JsonValue* value = entry.find(key);
                if (value == nullptr) { return 0; }
                const std::optional<std::int64_t> number = value->as_int64();
                return number.has_value() && *number >= 0 && *number <= max ? *number : -1;
            };
            const std::int64_t width = bounded("width", 65535);
            const std::int64_t height = bounded("height", 65535);
            const std::int64_t duration = bounded("duration_ms", UINT32_MAX);
            if (width < 0 || height < 0 || duration < 0) {
                return invalid(kAttachmentsField, input::Reason::OutOfRange);
            }
            item.width = static_cast<std::uint16_t>(width);
            item.height = static_cast<std::uint16_t>(height);
            item.duration_ms = static_cast<std::uint32_t>(duration);
            out.push_back(item);
        }
    }
    return out;
}

// --- the shape of every handler ----------------------------------------------------

// The rule by VALUE. It is 32 trivially copyable bytes whose bucket is a
// literal with static storage (http/rate_limit.h), and a pointer to it would
// need an owner outliving every handler: the first version pointed into a
// shared_ptr only a boot-time lambda held, and every send read a freed rule.
struct Budget final {
    http::RateLimiter*  limiter;
    http::RateLimitRule rule;
};

// What a handler is given. Every view in it borrows from the request or from
// the arena on the stack of the pool task that parsed the body, and both
// outlive the handler, which returns before the task does.
struct Call final {
    mongocxx::client&       client;
    const ChatService&      chat;
    const HttpRequestPtr&   req;
    const Actor&            actor;
    // Null for a route that takes no body.
    const input::JsonValue* body;
    // What the path named, as far as its pattern has placeholders for.
    Uuid                    conversation;
    Uuid                    user;
    std::int64_t            seq;
    // The per-TARGET claim budget, for the one route that has one; the
    // per-caller budget was spent before the body was read.
    const Budget*           target_budget;
};

// A plain function pointer, so the route table below is constexpr and every
// handler is a named function a reader can find, rather than the fourth lambda
// down inside the installer.
using Handler = HttpResponsePtr (*)(const Call&);

// What a pattern's placeholders name, in order.
enum class Path : std::uint8_t { None, User, Conversation, ConversationUser, ConversationSeq };

struct Segments final {
    Uuid         conversation{};
    Uuid         user{};
    std::int64_t seq{0};
};


// The order every handler answers in: the caller, then the origin, then — on
// db_pool — the budget, the body and the work, and exactly one answer.
void run(const HttpRequestPtr& req, Responder&& callback, const ChatService& chat, bool write,
         bool has_body, std::optional<Budget> budget, std::optional<Budget> target_budget,
         Segments segments, Handler handler) {
    const std::shared_ptr<const UserContext> ctx = ac::user_context(req);
    if (ctx == nullptr) {
        callback(ac::not_found_response());
        return;
    }
    // Before the body is read: a forged write costs a header compare.
    if (write && hj::is_rejection(hj::check_request_origin(req))) {
        callback(failure(req, fail(ErrorCode::Forbidden)));
        return;
    }
    const Actor actor{ctx->user_id, ctx->permissions, ctx->session_id};
    const ChatService* service = &chat;
    auto shared_callback = std::make_shared<Responder>(std::move(callback));
    const bool posted = Pools::db().try_post(anvil::guarded(
        "chat", [req, actor, service, has_body, budget, target_budget, segments, handler,
                 shared_callback]() {
            // Answered exactly once, whatever the work does: an exception that
            // escaped here would otherwise leave the client waiting forever.
            try {
                // The budget is a Redis round trip, so it is spent here and not
                // on the loop thread that accepted the request (CLAUDE.md §4).
                // Still before the body is parsed, so a refused request costs
                // no parse and no query.
                if (budget.has_value()) {
                    const http::RateLimitVerdict verdict = budget->limiter->check_account(
                        uuid::to_string(actor.user), budget->rule);
                    if (!verdict.allowed) {
                        HttpResponsePtr refused = failure(req, fail(ErrorCode::RateLimited));
                        hj::apply_retry_after(*refused,
                                              hj::retry_after_seconds(verdict, budget->rule));
                        (*shared_callback)(refused);
                        return;
                    }
                }
                auto entry = db::MongoPool::instance().acquire();
                input::BodyArena arena;
                const input::JsonValue* body = nullptr;
                std::optional<input::JsonDocument> document;
                if (has_body) {
                    document = input::parse_json(req->body(), arena);
                    if (!document->ok() || !document->root().is_object()) {
                        (*shared_callback)(
                            failure(req, invalid("body", input::Reason::BadFormat)));
                        return;
                    }
                    body = &document->root();
                }
                (*shared_callback)(handler(Call{*entry, *service, req, actor, body,
                                                segments.conversation, segments.user,
                                                segments.seq,
                                                target_budget.has_value() ? &*target_budget
                                                                          : nullptr}));
            } catch (const std::exception& e) {
                LOG_ERROR << "chat handler failed: " << e.what();
                (*shared_callback)(failure(req, fail(ErrorCode::Internal)));
            }
        }));
    if (!posted) {
        (*shared_callback)(failure(req, fail(ErrorCode::ServiceUnavailable)));
    }
}

// Status → 204, or the failure.
[[nodiscard]] HttpResponsePtr answer(const HttpRequestPtr& req, const Status& status) {
    return status ? no_content() : failure(req, status.error());
}

[[nodiscard]] HttpResponsePtr state_body(const ChatService& service,
                                         const ConversationState& state, int status) {
    std::string body;
    body.reserve(512);
    body += '{';
    hj::append_json_key(body, "conversation");
    append_conversation(body, service, state.conversation);
    body += ',';
    hj::append_json_key(body, "membership");
    append_membership(body, state.membership);
    body += '}';
    return json(status, std::move(body));
}

// The people a body names under `members`, all or nothing.
[[nodiscard]] Result<std::vector<Uuid>> people(const input::JsonValue& body, bool required) {
    std::vector<Uuid> out;
    const input::JsonValue* list = body.find(kMembersField);
    if (list == nullptr && !required) { return out; }
    if (list == nullptr || !list->is_array() ||
        list->elements().size() > kMaxMembersPerRequest) {
        return invalid(kMembersField, input::Reason::BadFormat);
    }
    out.reserve(list->elements().size());
    for (const input::JsonValue& entry : list->elements()) {
        const Result<Uuid> id = uuid_field(entry, kMembersField);
        if (!id) { return id.error(); }
        out.push_back(id.value());
    }
    return out;
}

[[nodiscard]] bool flag(const input::JsonValue& body, std::string_view key) {
    const input::JsonValue* value = body.find(key);
    return value != nullptr && value->as_bool().value_or(false);
}

// --- conversations ------------------------------------------------------------------

[[nodiscard]] HttpResponsePtr create(const Call& call) {
    // Required here, where a client is the caller: without it a lost answer
    // retried is a second group.
    std::array<std::uint8_t, 16> client_id{};
    const std::optional<std::string_view> cid = text_field(*call.body, kClientIdField);
    if (!cid.has_value() ||
        crypto::base64url_decode_into(*cid, client_id) != std::optional<std::size_t>{16U} ||
        std::all_of(client_id.begin(), client_id.end(), [](std::uint8_t b) { return b == 0; })) {
        return failure(call.req, invalid(kClientIdField, input::Reason::Required));
    }
    const Result<std::vector<Uuid>> members = people(*call.body, false);
    if (!members) { return failure(call.req, members.error()); }
    const CreateConversation request{text_field(*call.body, kKindField).value_or(""),
                                     text_field(*call.body, kTitleField).value_or(""),
                                     text_field(*call.body, kDescriptionField).value_or(""),
                                     members.value(), flag(*call.body, "encrypted"), client_id};
    const Result<CreatedConversation> made = call.chat.create(call.client, call.actor, request);
    if (!made) { return failure(call.req, made.error()); }
    // 201 for a new conversation, 200 for a retry answered with the first, as
    // a send answers.
    return state_body(call.chat,
                      ConversationState{made.value().conversation, made.value().membership},
                      made.value().created ? 201 : 200);
}

[[nodiscard]] HttpResponsePtr open_direct(const Call& call) {
    const Result<ConversationState> opened = call.chat.open_direct(
        call.client, call.actor, text_field(*call.body, kKindField).value_or(""), call.user,
        flag(*call.body, "encrypted"));
    if (!opened) { return failure(call.req, opened.error()); }
    return state_body(call.chat, opened.value(), 200);
}

void append_list_items(std::string& out, const ChatService& chat, std::string_view key,
                       const std::vector<ChatListItem>& rows) {
    hj::append_json_key(out, key);
    out += '[';
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (i != 0) { out += ','; }
        out += '{';
        hj::append_json_key(out, "conversation");
        append_conversation(out, chat, rows[i].conversation);
        out += ',';
        hj::append_json_key(out, "membership");
        append_membership(out, rows[i].membership);
        out += ',';
        hj::append_json_key(out, "unread");
        hj::append_json_int(out, rows[i].unread);
        out += '}';
    }
    out += ']';
}

[[nodiscard]] HttpResponsePtr list(const Call& call) {
    const bool archived = call.req->getParameter("archived") == "true";
    std::optional<std::pair<db::TimeMs, Uuid>> after;
    const std::optional<std::int64_t> at = query_integer(call.req, "after_at");
    const std::optional<Uuid> id = path_id(call.req->getParameter("after_id"));
    if (at.has_value() && id.has_value()) {
        after = std::pair{db::TimeMs{std::chrono::milliseconds{*at}}, *id};
    }
    const Result<ChatList> page = call.chat.chat_list(call.client, call.actor, archived, after,
                                                      page_limit(call.req, 30, 100));
    if (!page) { return failure(call.req, page.error()); }
    const ChatList& rows = page.value();
    std::string body;
    body.reserve(4096);
    body += '{';
    append_list_items(body, call.chat, "pinned", rows.pinned);
    body += ',';
    append_list_items(body, call.chat, "items", rows.items);
    body += ',';
    hj::append_json_key(body, "next");
    if (rows.next.has_value()) {
        body += '{';
        hj::append_json_key(body, "after_at");
        hj::append_json_int(body, rows.next->first.time_since_epoch().count());
        body += ',';
        hj::append_json_key(body, "after_id");
        hj::append_json_uuid(body, rows.next->second);
        body += '}';
    } else {
        body += "null";
    }
    body += '}';
    return json(200, std::move(body));
}

[[nodiscard]] HttpResponsePtr get(const Call& call) {
    const Result<ConversationState> state = call.chat.state(call.client, call.actor,
                                                            call.conversation);
    if (!state) { return failure(call.req, state.error()); }
    return state_body(call.chat, state.value(), 200);
}

[[nodiscard]] HttpResponsePtr update(const Call& call) {
    return answer(call.req, call.chat.update_info(call.client, call.actor, call.conversation,
                                                  text_field(*call.body, kTitleField),
                                                  text_field(*call.body, kDescriptionField)));
}

[[nodiscard]] HttpResponsePtr set_timer(const Call& call) {
    const Result<std::optional<std::int64_t>> seconds = integer_field(*call.body, kTimerField);
    if (!seconds) { return failure(call.req, seconds.error()); }
    if (!seconds.value().has_value() || *seconds.value() > UINT32_MAX) {
        return failure(call.req, invalid(kTimerField, input::Reason::Required));
    }
    return answer(call.req, call.chat.set_timer(call.client, call.actor, call.conversation,
                                                static_cast<std::uint32_t>(*seconds.value())));
}

// --- membership ---------------------------------------------------------------------

[[nodiscard]] HttpResponsePtr members(const Call& call) {
    const Result<std::vector<MemberRecord>> rows =
        call.chat.members(call.client, call.actor, call.conversation,
                          path_id(call.req->getParameter("after")),
                          page_limit(call.req, kMaxMemberPage, kMaxMemberPage));
    if (!rows) { return failure(call.req, rows.error()); }
    std::string body;
    body.reserve(rows.value().size() * 72 + 16);
    body += '{';
    hj::append_json_key(body, "members");
    body += '[';
    for (std::size_t i = 0; i < rows.value().size(); ++i) {
        if (i != 0) { body += ','; }
        body += '{';
        hj::append_json_key(body, "user");
        hj::append_json_uuid(body, rows.value()[i].user);
        body += ',';
        hj::append_json_key(body, "role");
        hj::append_json_string(body, role_name(rows.value()[i].role));
        body += '}';
    }
    body += "]}";
    return json(200, std::move(body));
}

[[nodiscard]] HttpResponsePtr add_members(const Call& call) {
    const Result<std::vector<Uuid>> added = people(*call.body, true);
    if (!added) { return failure(call.req, added.error()); }
    return answer(call.req, call.chat.add_members(call.client, call.actor, call.conversation,
                                                  added.value()));
}

[[nodiscard]] HttpResponsePtr update_member(const Call& call) {
    const std::optional<Role> role = role_from(text_field(*call.body, kRoleField).value_or(""));
    if (!role.has_value()) {
        return failure(call.req, invalid(kRoleField, input::Reason::NotAllowed));
    }
    return answer(call.req, call.chat.set_role(call.client, call.actor, call.conversation,
                                               call.user, *role));
}

[[nodiscard]] HttpResponsePtr remove_member(const Call& call) {
    return answer(call.req, call.chat.remove_member(call.client, call.actor, call.conversation,
                                                    call.user));
}

// --- messages -------------------------------------------------------------------------

// A stale encrypted send (docs/22 §7.6): the 409 every failure is, plus the
// first page of the device lists it was stale against, so the client can
// re-encrypt without a second round trip in the common case of a direct
// conversation or a small group.
[[nodiscard]] HttpResponsePtr stale(const Call& call) {
    const Result<ConversationDevices> page = call.chat.conversation_devices(
        call.client, call.actor, call.conversation, std::nullopt, kMaxDevicePage);
    if (!page) { return failure(call.req, page.error()); }
    std::string body;
    body.reserve(4096);
    hj::append_error_body(body, ErrorCode::Conflict, hj::request_id_of(call.req));
    // The page rides beside "error" in the one object the error body is.
    append_explanation(body, "chat.devices_stale", {});
    body.pop_back();
    body += ',';
    hj::append_json_key(body, "devices");
    append_device_page(body, page.value());
    body += '}';
    return json(409, std::move(body));
}

[[nodiscard]] HttpResponsePtr send(const Call& call) {
    // An encrypted send names the version it encrypted against; nothing else
    // does. The service refuses each kind of body in the other mode.
    Result<SentMessage> sent = fail(ErrorCode::Internal);
    if (call.body->find(kDeviceSetField) != nullptr) {
        Result<BoundEncrypted> bound = bind_encrypted(*call.body);
        if (!bound) { return failure(call.req, bound.error()); }
        BoundEncrypted message = std::move(bound).value();
        message.point();
        sent = call.chat.send_encrypted(call.client, call.actor, call.conversation,
                                        message.message);
        if (!sent && sent.code() == ErrorCode::Conflict && sent.error().field == kDevicesField) {
            return stale(call);
        }
    } else {
        Result<BoundSend> bound = bind_send(*call.body);
        if (!bound) { return failure(call.req, bound.error()); }
        // The spans are set only after the move: they would otherwise point
        // into the vectors of the Result.
        BoundSend message = std::move(bound).value();
        message.message.mentions = message.mentions;
        message.message.attachments = message.attachments;
        sent = call.chat.send(call.client, call.actor, call.conversation, message.message);
    }
    if (!sent) { return failure(call.req, sent.error()); }
    std::string out;
    out.reserve(96);
    out += '{';
    hj::append_json_key(out, "seq");
    hj::append_json_int(out, sent.value().seq);
    out += ',';
    hj::append_json_key(out, "sent_at");
    append_time(out, sent.value().sent_at);
    out += '}';
    // 201 for a new message, 200 for a retry answered with the first: both
    // success, and a client treats them alike.
    return json(sent.value().created ? 201 : 200, std::move(out));
}

void append_tallies(std::string& out,
                    const std::vector<ChatRepository::ReactionTally>& tallies) {
    hj::append_json_key(out, "reactions");
    out += '[';
    for (std::size_t i = 0; i < tallies.size(); ++i) {
        if (i != 0) { out += ','; }
        out += '{';
        hj::append_json_key(out, "seq");
        hj::append_json_int(out, tallies[i].seq);
        out += ',';
        hj::append_json_key(out, "reaction");
        hj::append_json_string(out, tallies[i].reaction);
        out += ',';
        hj::append_json_key(out, "count");
        hj::append_json_int(out, tallies[i].count);
        out += ',';
        hj::append_json_key(out, "mine");
        out += tallies[i].mine ? "true" : "false";
        out += '}';
    }
    out += ']';
}

// `after=` is a device's catch-up from its cursor, oldest first;
// `changed_after=` its catch-up from its MUTATION cursor, the messages edited,
// revoked or reacted to since, oldest change first (docs/22 §4.5); otherwise a
// page backwards from `before=` or the head.
[[nodiscard]] HttpResponsePtr history(const Call& call) {
    const std::int32_t limit = page_limit(call.req, 50, kMaxHistoryPage);
    std::vector<MessageRecord> messages;
    std::optional<std::int64_t> older;
    if (const std::optional<std::int64_t> changed = query_integer(call.req, "changed_after")) {
        Result<std::vector<MessageRecord>> found =
            call.chat.changes(call.client, call.actor, call.conversation, *changed, limit);
        if (!found) { return failure(call.req, found.error()); }
        messages = std::move(found).value();
    } else if (const std::optional<std::int64_t> after = query_integer(call.req, "after")) {
        Result<std::vector<MessageRecord>> caught =
            call.chat.catch_up(call.client, call.actor, call.conversation, *after, limit);
        if (!caught) { return failure(call.req, caught.error()); }
        messages = std::move(caught).value();
    } else {
        Result<HistoryPage> page = call.chat.history(call.client, call.actor, call.conversation,
                                                     query_integer(call.req, "before"), limit);
        if (!page) { return failure(call.req, page.error()); }
        HistoryPage taken = std::move(page).value();
        older = taken.older;
        messages = std::move(taken.messages);
    }
    std::vector<std::int64_t> seqs;
    seqs.reserve(messages.size());
    for (const MessageRecord& m : messages) { seqs.push_back(m.seq); }
    const Result<std::vector<ChatRepository::ReactionTally>> tallies =
        call.chat.reactions(call.client, call.actor, call.conversation, seqs);
    if (!tallies) { return failure(call.req, tallies.error()); }

    const std::int64_t now = now_unix();
    std::string body;
    body.reserve(messages.size() * 400 + 128);
    body += '{';
    hj::append_json_key(body, "messages");
    body += '[';
    for (std::size_t i = 0; i < messages.size(); ++i) {
        if (i != 0) { body += ','; }
        append_message(body, call.chat, messages[i], now);
    }
    body += "],";
    append_tallies(body, tallies.value());
    body += ',';
    hj::append_json_key(body, "older");
    if (older.has_value()) {
        hj::append_json_int(body, *older);
    } else {
        body += "null";
    }
    body += '}';
    return json(200, std::move(body));
}

[[nodiscard]] HttpResponsePtr edit(const Call& call) {
    const Result<std::vector<MentionSpan>> mentions = bind_mentions(*call.body);
    if (!mentions) { return failure(call.req, mentions.error()); }
    return answer(call.req,
                  call.chat.edit(call.client, call.actor, call.conversation, call.seq,
                                 text_field(*call.body, kBodyField).value_or(""),
                                 mentions.value()));
}

[[nodiscard]] HttpResponsePtr revoke(const Call& call) {
    return answer(call.req,
                  call.chat.revoke(call.client, call.actor, call.conversation, call.seq));
}

// `null` takes the caller's reaction back.
[[nodiscard]] HttpResponsePtr react(const Call& call) {
    const input::JsonValue* reaction = call.body->find(kReactionField);
    std::optional<std::string_view> wanted;
    if (reaction != nullptr && !reaction->is_null()) {
        wanted = reaction->as_string();
        if (!wanted.has_value()) {
            return failure(call.req, invalid(kReactionField, input::Reason::BadFormat));
        }
    }
    return answer(call.req, call.chat.react(call.client, call.actor, call.conversation,
                                            call.seq, wanted));
}

[[nodiscard]] HttpResponsePtr read_by(const Call& call) {
    const Result<Readers> readers =
        call.chat.readers(call.client, call.actor, call.conversation, call.seq);
    if (!readers) { return failure(call.req, readers.error()); }
    const auto append_ids = [](std::string& out, const std::vector<Uuid>& ids) {
        out += '[';
        for (std::size_t i = 0; i < ids.size(); ++i) {
            if (i != 0) { out += ','; }
            hj::append_json_uuid(out, ids[i]);
        }
        out += ']';
    };
    std::string body;
    body.reserve(readers.value().delivered_to.size() * 80 + 48);
    body += '{';
    // Null, not empty, for a kind that does not show reads: an empty list
    // would say nobody read it.
    hj::append_json_key(body, "read_by");
    if (readers.value().read_by.has_value()) {
        append_ids(body, *readers.value().read_by);
    } else {
        body += "null";
    }
    body += ',';
    hj::append_json_key(body, "delivered_to");
    append_ids(body, readers.value().delivered_to);
    body += '}';
    return json(200, std::move(body));
}

// --- receipts, preferences, invites, follow ---------------------------------------

[[nodiscard]] HttpResponsePtr receipts(const Call& call) {
    const Result<std::optional<std::int64_t>> delivered =
        integer_field(*call.body, "delivered");
    if (!delivered) { return failure(call.req, delivered.error()); }
    const Result<std::optional<std::int64_t>> read = integer_field(*call.body, "read");
    if (!read) { return failure(call.req, read.error()); }
    const Result<MemberRecord> after =
        call.chat.receipts(call.client, call.actor, call.conversation,
                           delivered.value().value_or(0), read.value().value_or(0));
    if (!after) { return failure(call.req, after.error()); }
    std::string out;
    out.reserve(256);
    append_membership(out, after.value());
    return json(200, std::move(out));
}

[[nodiscard]] HttpResponsePtr preferences(const Call& call) {
    const auto optional_flag = [&](std::string_view key) -> std::optional<bool> {
        const input::JsonValue* value = call.body->find(key);
        return value == nullptr ? std::nullopt : value->as_bool();
    };
    MemberPreferences prefs{};
    prefs.pinned = optional_flag("pinned");
    prefs.archived = optional_flag("archived");
    prefs.read_private = optional_flag("read_private");
    prefs.clear = optional_flag("clear").value_or(false);
    if (const input::JsonValue* duration = call.body->find(kMuteForField); duration != nullptr) {
        const std::optional<std::int64_t> seconds = duration->as_int64();
        if (!seconds.has_value() || *seconds < 0) {
            return failure(call.req, invalid(kMuteForField, input::Reason::BadFormat));
        }
        // Bounded while 64 bits wide; the service refuses past its ceiling.
        prefs.mute_for = std::chrono::seconds{
            std::min<std::int64_t>(*seconds, kMaxMuteDuration.count() + 1)};
    }
    if (const input::JsonValue* forever = call.body->find("mute_indefinitely");
        forever != nullptr) {
        const std::optional<bool> on = forever->as_bool();
        if (!on.has_value()) {
            return failure(call.req, invalid("mute_indefinitely", input::Reason::BadFormat));
        }
        prefs.mute_indefinitely = *on;
    }
    if (const input::JsonValue* muted = call.body->find("muted_until"); muted != nullptr) {
        const std::optional<std::int64_t> ms = muted->as_int64();
        if (muted->is_null()) {
            prefs.muted_until = std::optional<db::TimeMs>{};
        } else if (ms.has_value() && *ms >= 0) {
            prefs.muted_until =
                std::optional<db::TimeMs>{db::TimeMs{std::chrono::milliseconds{*ms}}};
        } else {
            return failure(call.req, invalid("muted_until", input::Reason::BadFormat));
        }
    }
    const Result<MemberRecord> after =
        call.chat.preferences(call.client, call.actor, call.conversation, prefs);
    if (!after) { return failure(call.req, after.error()); }
    // The membership after, as receipts answer: a mute given as a duration is
    // an instant only the server could compute, and this is where the client
    // learns it.
    std::string out;
    out.reserve(256);
    append_membership(out, after.value());
    return json(200, std::move(out));
}

[[nodiscard]] HttpResponsePtr create_invite(const Call& call) {
    const Result<std::optional<std::int64_t>> uses = integer_field(*call.body, "uses");
    if (!uses) { return failure(call.req, uses.error()); }
    const Result<std::optional<std::int64_t>> seconds = integer_field(*call.body, "lifetime_s");
    if (!seconds) { return failure(call.req, seconds.error()); }
    // Clamped while 64 bits wide; the service bounds both again, and refuses
    // past kMaxInviteUses and kMaxInviteLifetime rather than clamping.
    const Result<std::string> token = call.chat.create_invite(
        call.client, call.actor, call.conversation,
        static_cast<std::int32_t>(std::min<std::int64_t>(uses.value().value_or(1), INT32_MAX)),
        std::chrono::seconds{std::min<std::int64_t>(seconds.value().value_or(86'400),
                                                    kMaxInviteLifetime.count() + 1)});
    if (!token) { return failure(call.req, token.error()); }
    std::string out;
    out.reserve(96);
    out += '{';
    hj::append_json_key(out, "token");
    hj::append_json_string(out, token.value());
    out += '}';
    return json(201, std::move(out));
}

[[nodiscard]] HttpResponsePtr revoke_invite(const Call& call) {
    const Result<bool> revoked = call.chat.revoke_invite(
        call.client, call.actor, call.conversation,
        text_field(*call.body, "token").value_or(""));
    if (!revoked) { return failure(call.req, revoked.error()); }
    return no_content();
}

[[nodiscard]] HttpResponsePtr join(const Call& call) {
    const Result<ConversationState> joined =
        call.chat.join(call.client, call.actor, text_field(*call.body, "token").value_or(""));
    if (!joined) { return failure(call.req, joined.error()); }
    return state_body(call.chat, joined.value(), 200);
}

[[nodiscard]] HttpResponsePtr follow(const Call& call) {
    const Result<ConversationState> followed =
        call.chat.follow(call.client, call.actor, call.conversation);
    if (!followed) { return failure(call.req, followed.error()); }
    return state_body(call.chat, followed.value(), 200);
}

// --- blocks ---------------------------------------------------------------------------

[[nodiscard]] HttpResponsePtr block(const Call& call) {
    return answer(call.req, call.chat.block(call.client, call.actor, call.user));
}

[[nodiscard]] HttpResponsePtr unblock(const Call& call) {
    return answer(call.req, call.chat.unblock(call.client, call.actor, call.user));
}

// --- presence -------------------------------------------------------------------------

// `{"online":…,"last_seen":…}`. Withheld reads exactly like never seen: offline
// and null, so a viewer the subject hides from learns nothing by asking.
[[nodiscard]] HttpResponsePtr presence(const Call& call) {
    const Result<PresenceView> seen = call.chat.presence(call.client, call.actor, call.user);
    if (!seen) { return failure(call.req, seen.error()); }
    std::string body;
    body.reserve(64);
    body += '{';
    hj::append_json_key(body, "online");
    body += seen.value().online ? "true" : "false";
    body += ',';
    hj::append_json_key(body, "last_seen");
    append_optional_time(body, seen.value().last_seen);
    body += '}';
    return json(200, std::move(body));
}

// `?users=<id>,<id>,…`: the accounts a chat-list page shows, in one request
// rather than one each. Answered in their order, duplicates once; every entry is
// `{"user","online","last_seen"}`, and withheld reads exactly as never seen.
[[nodiscard]] HttpResponsePtr presence_many(const Call& call) {
    const std::string& raw = call.req->getParameter("users");
    if (raw.empty()) { return failure(call.req, invalid("users", input::Reason::Required)); }
    std::vector<Uuid> subjects;
    std::string_view rest{raw};
    while (!rest.empty()) {
        const std::size_t comma = rest.find(',');
        const std::string_view one = rest.substr(0, comma);
        const std::optional<Uuid> id = path_id(one);
        if (!id.has_value()) { return failure(call.req, invalid("users", input::Reason::BadFormat)); }
        if (std::find(subjects.begin(), subjects.end(), *id) == subjects.end()) {
            // Refused as it grows, so a query of a million ids is never held.
            if (subjects.size() == kMaxPresenceBatch) {
                return failure(call.req, invalid("users", input::Reason::TooLong));
            }
            subjects.push_back(*id);
        }
        if (comma == std::string_view::npos) { break; }
        rest.remove_prefix(comma + 1);
    }
    const Result<std::vector<PresenceView>> seen =
        call.chat.presence(call.client, call.actor, subjects);
    if (!seen) { return failure(call.req, seen.error()); }
    std::string body;
    body.reserve(subjects.size() * 96 + 16);
    body += '{';
    hj::append_json_key(body, "presence");
    body += '[';
    for (std::size_t i = 0; i < subjects.size(); ++i) {
        if (i != 0) { body += ','; }
        body += '{';
        hj::append_json_key(body, "user");
        hj::append_json_uuid(body, subjects[i]);
        body += ',';
        hj::append_json_key(body, "online");
        body += seen.value()[i].online ? "true" : "false";
        body += ',';
        hj::append_json_key(body, "last_seen");
        append_optional_time(body, seen.value()[i].last_seen);
        body += '}';
    }
    body += "]}";
    return json(200, std::move(body));
}

// --- devices, keys and the queue (docs/22 §7.3, §7.5, §7.6) ------------------------

namespace din = device_inputs;

// One fixed-length key or signature from unpadded base64url. A wrong length is
// a malformed field, named, before any key is looked at.
template <std::size_t N>
[[nodiscard]] Status fixed_bytes(const input::JsonValue& body, std::string_view key,
                                 std::array<std::uint8_t, N>& out) {
    const std::optional<std::string_view> text = text_field(body, key);
    if (!text.has_value()) { return invalid(key, input::Reason::Required); }
    if (crypto::base64url_decode_into(*text, out) != std::optional<std::size_t>{N}) {
        return invalid(key, input::Reason::BadFormat);
    }
    return ok();
}

// A device's public bundle as its client uploads it. The keys are checked as
// keys by the directory; this only gets the bytes out.
[[nodiscard]] Result<NewDevice> bind_device(const input::JsonValue& body) {
    NewDevice out{};
    const input::JsonValue* id = body.find(din::kDeviceId);
    if (id == nullptr) { return invalid(din::kDeviceId, input::Reason::Required); }
    const Result<Uuid> device = uuid_field(*id, din::kDeviceId);
    if (!device) { return device.error(); }
    out.id = device.value();
    const Result<std::optional<std::int64_t>> suite = integer_field(body, din::kSuite);
    if (!suite) { return suite.error(); }
    if (!suite.value().has_value() || *suite.value() < 1 ||
        *suite.value() > static_cast<std::int64_t>(kMaxSuite)) {
        return invalid(din::kSuite, input::Reason::NotAllowed);
    }
    out.keys.suite = static_cast<Suite>(*suite.value());
    for (const Status read :
         {fixed_bytes(body, din::kAgreementKey, out.keys.agreement),
          fixed_bytes(body, din::kSigningKey, out.keys.signing),
          fixed_bytes(body, din::kSignedPrekey, out.keys.signed_prekey),
          fixed_bytes(body, din::kSignedPrekeySignature, out.keys.signed_prekey_signature),
          fixed_bytes(body, din::kLastResortKey, out.keys.last_resort),
          fixed_bytes(body, din::kLastResortSignature, out.keys.last_resort_signature)}) {
        if (!read) { return read.error(); }
    }
    return out;
}

// The device a query string names, which must parse: a device route without
// one is a malformed request, not a request about nothing.
[[nodiscard]] Result<Uuid> query_device(const HttpRequestPtr& req) {
    const std::optional<Uuid> device = path_id(req->getParameter(std::string{kDeviceField}));
    if (!device.has_value()) { return invalid(kDeviceField, input::Reason::Required); }
    return *device;
}

[[nodiscard]] HttpResponsePtr my_devices(const Call& call) {
    const Result<std::optional<IdentityRecord>> mine = call.chat.my_devices(call.client, call.actor);
    if (!mine) { return failure(call.req, mine.error()); }
    std::string body;
    body.reserve(1024);
    body += '{';
    hj::append_json_key(body, "dv");
    hj::append_json_int(body, mine.value().has_value() ? mine.value()->device_set_version : 0);
    body += ',';
    hj::append_json_key(body, "devices");
    body += '[';
    if (mine.value().has_value()) {
        bool first = true;
        for (const DeviceRecord& device : mine.value()->devices) {
            if (!first) { body += ','; }
            first = false;
            // The published bundle, then what only the owner is shown: when
            // each device was last seen, and whether it should upload keys.
            // Never the session that registered it.
            append_published_device(body, device.published);
            body.pop_back();
            body += ',';
            hj::append_json_key(body, "last_seen");
            append_time(body, device.last_seen);
            body += ',';
            hj::append_json_key(body, "low");
            body += device.low ? "true" : "false";
            body += ',';
            // Whether THIS session registered it: a client whose key store was
            // evicted finds the device it orphaned and unlinks it, rather than
            // every sender encrypting to it until the idle sweeper's thirty
            // days are up. A boolean, so the session itself never leaves.
            hj::append_json_key(body, "mine");
            body += device.session == call.actor.session ? "true" : "false";
            body += '}';
        }
    }
    body += "]}";
    return json(200, std::move(body));
}

[[nodiscard]] HttpResponsePtr register_device(const Call& call) {
    Result<NewDevice> device = bind_device(*call.body);
    if (!device) { return failure(call.req, device.error()); }
    const Status made = call.chat.register_device(call.client, call.actor, device.value());
    return made ? no_content() : failure(call.req, made.error());
}

[[nodiscard]] HttpResponsePtr link_device(const Call& call) {
    Result<NewDevice> device = bind_device(*call.body);
    if (!device) { return failure(call.req, device.error()); }
    const input::JsonValue* approver = call.body->find(din::kApprover);
    if (approver == nullptr) { return failure(call.req, invalid(din::kApprover, input::Reason::Required)); }
    const Result<Uuid> approving = uuid_field(*approver, din::kApprover);
    if (!approving) { return failure(call.req, approving.error()); }
    const Result<std::optional<std::int64_t>> timestamp = integer_field(*call.body, din::kTimestamp);
    if (!timestamp) { return failure(call.req, timestamp.error()); }
    if (!timestamp.value().has_value()) {
        return failure(call.req, invalid(din::kTimestamp, input::Reason::Required));
    }
    crypto::Ed25519Signature signature{};
    if (const Status read = fixed_bytes(*call.body, din::kLinkSignature, signature); !read) {
        return failure(call.req, read.error());
    }
    const Status linked = call.chat.link_device(
        call.client, call.actor, approving.value(), device.value(),
        static_cast<std::uint64_t>(*timestamp.value()), signature);
    return linked ? no_content() : failure(call.req, linked.error());
}

// `{signed_prekey, signed_prekey_signature}`, `{last_resort_key,
// last_resort_signature}` or both, each pair whole. The path names the device.
[[nodiscard]] HttpResponsePtr rotate_prekeys(const Call& call) {
    const auto pair = [&](std::string_view key_field, std::string_view signature_field)
        -> Result<std::optional<SignedPrekey>> {
        const bool has_key = call.body->find(key_field) != nullptr;
        const bool has_signature = call.body->find(signature_field) != nullptr;
        if (!has_key && !has_signature) { return std::optional<SignedPrekey>{}; }
        SignedPrekey out{};
        if (const Status read = fixed_bytes(*call.body, key_field, out.key); !read) {
            return read.error();
        }
        if (const Status read = fixed_bytes(*call.body, signature_field, out.signature); !read) {
            return read.error();
        }
        return std::optional<SignedPrekey>{out};
    };
    const Result<std::optional<SignedPrekey>> signed_prekey =
        pair(din::kSignedPrekey, din::kSignedPrekeySignature);
    if (!signed_prekey) { return failure(call.req, signed_prekey.error()); }
    const Result<std::optional<SignedPrekey>> last_resort =
        pair(din::kLastResortKey, din::kLastResortSignature);
    if (!last_resort) { return failure(call.req, last_resort.error()); }
    return answer(call.req, call.chat.rotate_prekeys(call.client, call.actor, call.user,
                                                     signed_prekey.value(), last_resort.value()));
}

// --- staff review and reports (docs/22 §9.2) -------------------------------------------

// Forbidden from a review route says why, because the reviewer already holds
// the permission and is owed the reason a conversation is closed to them.
[[nodiscard]] HttpResponsePtr review_failure(const HttpRequestPtr& req, const Failure& failed) {
    if (failed.code == ErrorCode::Forbidden && failed.field == "conversation") {
        std::string body;
        body.reserve(160);
        hj::append_error_body(body, failed.code, hj::request_id_of(req));
        append_explanation(body, "chat.not_reviewable", {});
        return json(403, std::move(body));
    }
    return failure(req, failed);
}

// `{"conversation":{…},"members":[{user, role, current}],"next":…|null}`.
[[nodiscard]] HttpResponsePtr review_conversation(const Call& call) {
    const Result<Review> seen =
        call.chat.review(call.client, call.actor, http::client_address(call.req),
                         call.conversation, path_id(call.req->getParameter("after")),
                         page_limit(call.req, kMaxMemberPage, kMaxMemberPage));
    if (!seen) { return review_failure(call.req, seen.error()); }
    std::string body;
    body.reserve(seen.value().members.size() * 80 + 512);
    body += '{';
    hj::append_json_key(body, "conversation");
    append_conversation(body, call.chat, seen.value().conversation);
    body += ',';
    hj::append_json_key(body, "members");
    body += '[';
    for (std::size_t i = 0; i < seen.value().members.size(); ++i) {
        if (i != 0) { body += ','; }
        body += '{';
        hj::append_json_key(body, "user");
        hj::append_json_uuid(body, seen.value().members[i].user);
        body += ',';
        hj::append_json_key(body, "role");
        hj::append_json_string(body, role_name(seen.value().members[i].role));
        body += '}';
    }
    body += "],";
    hj::append_json_key(body, "next");
    if (seen.value().next.has_value()) {
        hj::append_json_uuid(body, *seen.value().next);
    } else {
        body += "null";
    }
    body += '}';
    return json(200, std::move(body));
}

// The history route's shape, `{messages, reactions, older}`, over the whole
// retained log, attachments as grants as a member's are. Reactions are
// tallied with `mine` always false: the reviewer is nobody in it.
[[nodiscard]] HttpResponsePtr review_history(const Call& call) {
    Result<HistoryPage> page = call.chat.review_history(
        call.client, call.actor, http::client_address(call.req), call.conversation,
        query_integer(call.req, "before"), page_limit(call.req, 50, kMaxHistoryPage));
    if (!page) { return review_failure(call.req, page.error()); }
    std::vector<std::int64_t> seqs;
    seqs.reserve(page.value().messages.size());
    for (const MessageRecord& m : page.value().messages) { seqs.push_back(m.seq); }
    const Result<std::vector<ChatRepository::ReactionTally>> tallies =
        call.chat.review_reactions(call.client, call.conversation, seqs);
    if (!tallies) { return failure(call.req, tallies.error()); }
    const std::int64_t now = now_unix();
    std::string body;
    body.reserve(page.value().messages.size() * 400 + 128);
    body += '{';
    hj::append_json_key(body, "messages");
    body += '[';
    for (std::size_t i = 0; i < page.value().messages.size(); ++i) {
        if (i != 0) { body += ','; }
        append_message(body, call.chat, page.value().messages[i], now);
    }
    body += "],";
    append_tallies(body, tallies.value());
    body += ',';
    hj::append_json_key(body, "older");
    if (page.value().older.has_value()) {
        hj::append_json_int(body, *page.value().older);
    } else {
        body += "null";
    }
    body += '}';
    return json(200, std::move(body));
}

// `{from, to, note?}` → 201 `{"id"}`, or 200 for a retry over the same range.
[[nodiscard]] HttpResponsePtr report(const Call& call) {
    const Result<std::optional<std::int64_t>> from = integer_field(*call.body, "from");
    if (!from) { return failure(call.req, from.error()); }
    const Result<std::optional<std::int64_t>> to = integer_field(*call.body, "to");
    if (!to) { return failure(call.req, to.error()); }
    if (!from.value().has_value()) {
        return failure(call.req, invalid("from", input::Reason::Required));
    }
    const Result<FiledReport> filed = call.chat.report(
        call.client, call.actor, call.conversation, *from.value(),
        to.value().value_or(*from.value()), text_field(*call.body, "note").value_or(""));
    if (!filed) { return review_failure(call.req, filed.error()); }
    std::string body;
    body.reserve(64);
    body += '{';
    hj::append_json_key(body, "id");
    hj::append_json_uuid(body, filed.value().report.id);
    body += '}';
    return json(filed.value().created ? 201 : 200, std::move(body));
}

// `{"reports":[{id, conversation, reporter, from, to, note, at}],"next":…|null}`,
// newest first.
[[nodiscard]] HttpResponsePtr reports(const Call& call) {
    const std::int32_t limit = page_limit(call.req, 50, kMaxReportPage);
    const Result<std::vector<ReportRecord>> rows =
        call.chat.reports(call.client, path_id(call.req->getParameter("after")), limit);
    if (!rows) { return failure(call.req, rows.error()); }
    std::string body;
    body.reserve(rows.value().size() * 256 + 32);
    body += '{';
    hj::append_json_key(body, "reports");
    body += '[';
    for (std::size_t i = 0; i < rows.value().size(); ++i) {
        const ReportRecord& row = rows.value()[i];
        if (i != 0) { body += ','; }
        body += '{';
        hj::append_json_key(body, "id");
        hj::append_json_uuid(body, row.id);
        body += ',';
        hj::append_json_key(body, "conversation");
        hj::append_json_uuid(body, row.conversation);
        body += ',';
        hj::append_json_key(body, "reporter");
        hj::append_json_uuid(body, row.reporter);
        body += ',';
        hj::append_json_key(body, "from");
        hj::append_json_int(body, row.from);
        body += ',';
        hj::append_json_key(body, "to");
        hj::append_json_int(body, row.to);
        body += ',';
        hj::append_json_key(body, "note");
        hj::append_json_string(body, row.note);
        body += ',';
        hj::append_json_key(body, "at");
        append_time(body, row.at);
        body += '}';
    }
    body += "],";
    hj::append_json_key(body, "next");
    if (rows.value().size() == static_cast<std::size_t>(limit)) {
        hj::append_json_uuid(body, rows.value().back().id);
    } else {
        body += "null";
    }
    body += '}';
    return json(200, std::move(body));
}

// --- the link relay (docs/22 §7.3.1) -------------------------------------------------

// The keys a link signs over, as every relay answer writes them.
void append_link_keys(std::string& body, const LinkRequest& request) {
    hj::append_json_key(body, "device_id");
    hj::append_json_uuid(body, request.device);
    body += ',';
    hj::append_json_key(body, "agreement_key");
    hj::append_json_string(body, crypto::base64url_encode(request.agreement));
    body += ',';
    hj::append_json_key(body, "signing_key");
    hj::append_json_string(body, crypto::base64url_encode(request.signing));
}

// `{device_id, agreement_key, signing_key}` → 201 `{"token","expires_in_s"}`.
[[nodiscard]] HttpResponsePtr request_link(const Call& call) {
    const input::JsonValue* id = call.body->find(din::kDeviceId);
    if (id == nullptr) { return failure(call.req, invalid(din::kDeviceId, input::Reason::Required)); }
    const Result<Uuid> device = uuid_field(*id, din::kDeviceId);
    if (!device) { return failure(call.req, device.error()); }
    crypto::X25519PublicKey agreement{};
    crypto::Ed25519PublicKey signing{};
    if (const Status read = fixed_bytes(*call.body, din::kAgreementKey, agreement); !read) {
        return failure(call.req, read.error());
    }
    if (const Status read = fixed_bytes(*call.body, din::kSigningKey, signing); !read) {
        return failure(call.req, read.error());
    }
    const Result<std::string> token =
        call.chat.request_link(call.client, call.actor, device.value(), agreement, signing);
    if (!token) { return failure(call.req, token.error()); }
    std::string body;
    body.reserve(96);
    body += '{';
    hj::append_json_key(body, "token");
    hj::append_json_string(body, token.value());
    body += ',';
    hj::append_json_key(body, "expires_in_s");
    hj::append_json_int(body, kLinkRequestLifetime.count());
    body += '}';
    return json(201, std::move(body));
}

// `{token}` → `{device_id, agreement_key, signing_key, expires_in_s}`: what the
// approver shows its person and signs. A duration, never an instant: the
// approver's clock is a device's.
[[nodiscard]] HttpResponsePtr read_link_request(const Call& call) {
    const Result<LinkRequest> request = call.chat.read_link_request(
        call.client, call.actor, text_field(*call.body, "token").value_or(""));
    if (!request) { return failure(call.req, request.error()); }
    std::string body;
    body.reserve(256);
    body += '{';
    append_link_keys(body, request.value());
    body += ',';
    hj::append_json_key(body, "expires_in_s");
    const auto left = std::chrono::duration_cast<std::chrono::seconds>(
        request.value().expires_at - db::now_ms());
    hj::append_json_int(body, std::max<std::int64_t>(left.count(), 0));
    body += '}';
    return json(200, std::move(body));
}

// `{token, approver, timestamp, link_signature}` → 204.
[[nodiscard]] HttpResponsePtr approve_link_request(const Call& call) {
    const input::JsonValue* approver = call.body->find(din::kApprover);
    if (approver == nullptr) {
        return failure(call.req, invalid(din::kApprover, input::Reason::Required));
    }
    const Result<Uuid> approving = uuid_field(*approver, din::kApprover);
    if (!approving) { return failure(call.req, approving.error()); }
    const Result<std::optional<std::int64_t>> timestamp = integer_field(*call.body, din::kTimestamp);
    if (!timestamp) { return failure(call.req, timestamp.error()); }
    if (!timestamp.value().has_value()) {
        return failure(call.req, invalid(din::kTimestamp, input::Reason::Required));
    }
    crypto::Ed25519Signature signature{};
    if (const Status read = fixed_bytes(*call.body, din::kLinkSignature, signature); !read) {
        return failure(call.req, read.error());
    }
    return answer(call.req, call.chat.approve_link_request(
                                call.client, call.actor, text_field(*call.body, "token").value_or(""),
                                approving.value(), static_cast<std::uint64_t>(*timestamp.value()),
                                signature));
}

// `{token}` → `{"approval":null}` while pending, then once
// `{"approval":{approver, timestamp, link_signature}}`, then 404.
[[nodiscard]] HttpResponsePtr collect_link_approval(const Call& call) {
    const Result<std::optional<LinkProof>> collected = call.chat.collect_link_approval(
        call.client, call.actor, text_field(*call.body, "token").value_or(""));
    if (!collected) { return failure(call.req, collected.error()); }
    std::string body;
    body.reserve(192);
    body += '{';
    hj::append_json_key(body, "approval");
    if (collected.value().has_value()) {
        const LinkProof& proof = *collected.value();
        body += '{';
        hj::append_json_key(body, din::kApprover);
        hj::append_json_uuid(body, proof.approver);
        body += ',';
        hj::append_json_key(body, din::kTimestamp);
        hj::append_json_int(body, static_cast<std::int64_t>(proof.timestamp_s));
        body += ',';
        hj::append_json_key(body, din::kLinkSignature);
        hj::append_json_string(body, crypto::base64url_encode(proof.signature));
        body += '}';
    } else {
        body += "null";
    }
    body += '}';
    return json(200, std::move(body));
}

// The path names a device, which the segment carries in `user`.
[[nodiscard]] HttpResponsePtr unlink_device(const Call& call) {
    return answer(call.req, call.chat.unlink_device(call.client, call.actor, call.user));
}

[[nodiscard]] HttpResponsePtr upload_prekeys(const Call& call) {
    const input::JsonValue* device = call.body->find(prekey_inputs::kDevice);
    if (device == nullptr) {
        return failure(call.req, invalid(prekey_inputs::kDevice, input::Reason::Required));
    }
    const Result<Uuid> target = uuid_field(*device, prekey_inputs::kDevice);
    if (!target) { return failure(call.req, target.error()); }
    const input::JsonValue* list = call.body->find(prekey_inputs::kPrekeys);
    if (list == nullptr || !list->is_array()) {
        return failure(call.req, invalid(prekey_inputs::kPrekeys, input::Reason::BadFormat));
    }
    if (list->elements().size() > kMaxPrekeyBatch) {
        return failure(call.req, invalid(prekey_inputs::kPrekeys, input::Reason::TooLong));
    }
    std::vector<OneTimePrekey> keys;
    keys.reserve(list->elements().size());
    for (const input::JsonValue& entry : list->elements()) {
        OneTimePrekey key{};
        const Result<std::optional<std::int64_t>> id = integer_field(entry, prekey_inputs::kKeyId);
        if (!id) { return failure(call.req, id.error()); }
        if (!id.value().has_value() || *id.value() > UINT32_MAX) {
            return failure(call.req, invalid(prekey_inputs::kKeyId, input::Reason::OutOfRange));
        }
        key.id = static_cast<std::uint32_t>(*id.value());
        if (const Status read = fixed_bytes(entry, prekey_inputs::kPrekey, key.key); !read) {
            return failure(call.req, read.error());
        }
        keys.push_back(key);
    }
    return answer(call.req, call.chat.upload_prekeys(call.client, call.actor, target.value(), keys));
}

void append_bundle(std::string& body, const ClaimedBundle& bundle) {
    body += '{';
    hj::append_json_key(body, "device");
    hj::append_json_uuid(body, bundle.device);
    body += ',';
    hj::append_json_key(body, "suite");
    hj::append_json_int(body, static_cast<std::int64_t>(bundle.keys.suite));
    for (const auto& [key, bytes] :
         {std::pair<std::string_view, std::span<const std::uint8_t>>{"agreement_key",
                                                                     bundle.keys.agreement},
          {"signing_key", bundle.keys.signing},
          {"signed_prekey", bundle.keys.signed_prekey},
          {"signed_prekey_signature", bundle.keys.signed_prekey_signature},
          {"last_resort_key", bundle.keys.last_resort},
          {"last_resort_signature", bundle.keys.last_resort_signature}}) {
        body += ',';
        hj::append_json_key(body, key);
        hj::append_json_string(body, crypto::base64url_encode(bytes));
    }
    body += ',';
    // Null when the pool was empty: the sender uses the last-resort key.
    hj::append_json_key(body, "one_time");
    if (bundle.one_time.has_value()) {
        body += '{';
        hj::append_json_key(body, "kid");
        hj::append_json_int(body, bundle.one_time->id);
        body += ',';
        hj::append_json_key(body, "key");
        hj::append_json_string(body, crypto::base64url_encode(bundle.one_time->key));
        body += '}';
    } else {
        body += "null";
    }
    body += '}';
}

// `{"users":[…]}`, up to kMaxClaimBatch accounts: a group's first encrypted
// send claims a page of members per request rather than one request each.
// Answered per account, because keys claimed for the others are spent:
// `{"claims":[{"user","bundles":[…]|null,"refused":null|"NOT_FOUND"|
// "RATE_LIMITED","retry_after":null|seconds}]}`.
[[nodiscard]] HttpResponsePtr claim_prekeys(const Call& call) {
    const input::JsonValue* list = call.body->find(din::kUsers);
    if (list == nullptr || !list->is_array()) {
        return failure(call.req, invalid(din::kUsers, input::Reason::Required));
    }
    // Refused before a single id is parsed past the bound, so a list of a
    // million costs a length compare.
    if (list->elements().size() > kMaxClaimBatch * 2) {
        return failure(call.req, invalid(din::kUsers, input::Reason::TooLong));
    }
    std::vector<Uuid> targets;
    targets.reserve(list->elements().size());
    for (const input::JsonValue& entry : list->elements()) {
        const Result<Uuid> id = uuid_field(entry, din::kUsers);
        if (!id) { return failure(call.req, id.error()); }
        targets.push_back(id.value());
    }
    // The per-target budget, spent by the service only for a target that is a
    // current member, and before any of its keys leaves its pool: a target
    // whose keys are being drained is refused to every member for the window,
    // which costs a sender a retry and costs the attacker the drain.
    std::vector<std::pair<Uuid, std::uint32_t>> retry_after;
    const auto admit = [&](const Uuid& target) -> Status {
        if (call.target_budget == nullptr) { return ok(); }
        const http::RateLimitVerdict verdict = call.target_budget->limiter->check_account(
            uuid::to_string(target), call.target_budget->rule);
        if (verdict.allowed) { return ok(); }
        retry_after.emplace_back(target, hj::retry_after_seconds(verdict, call.target_budget->rule));
        return fail(ErrorCode::RateLimited);
    };
    const Result<std::vector<AccountClaim>> claims =
        call.chat.claim_bundles(call.client, call.actor, call.conversation, targets, admit);
    if (!claims) { return failure(call.req, claims.error()); }
    std::string body;
    body.reserve(claims.value().size() * 3200 + 32);
    body += '{';
    hj::append_json_key(body, "claims");
    body += '[';
    for (std::size_t i = 0; i < claims.value().size(); ++i) {
        const AccountClaim& claim = claims.value()[i];
        if (i != 0) { body += ','; }
        body += '{';
        hj::append_json_key(body, "user");
        hj::append_json_uuid(body, claim.user);
        body += ',';
        hj::append_json_key(body, "bundles");
        if (claim.refused.has_value()) {
            body += "null";
        } else {
            body += '[';
            for (std::size_t j = 0; j < claim.bundles.size(); ++j) {
                if (j != 0) { body += ','; }
                append_bundle(body, claim.bundles[j]);
            }
            body += ']';
        }
        body += ',';
        hj::append_json_key(body, "refused");
        if (claim.refused.has_value()) {
            hj::append_json_string(body, hj::wire_name(*claim.refused));
        } else {
            body += "null";
        }
        body += ',';
        hj::append_json_key(body, "retry_after");
        const auto waited = std::find_if(retry_after.begin(), retry_after.end(),
                                         [&](const auto& r) { return r.first == claim.user; });
        if (claim.refused == ErrorCode::RateLimited && waited != retry_after.end()) {
            hj::append_json_int(body, waited->second);
        } else {
            body += "null";
        }
        body += '}';
    }
    body += "]}";
    return json(200, std::move(body));
}

[[nodiscard]] HttpResponsePtr conversation_devices(const Call& call) {
    const Result<ConversationDevices> page = call.chat.conversation_devices(
        call.client, call.actor, call.conversation, path_id(call.req->getParameter("after")),
        page_limit(call.req, kMaxDevicePage, kMaxDevicePage));
    if (!page) { return failure(call.req, page.error()); }
    std::string body;
    body.reserve(4096);
    append_device_page(body, page.value());
    return json(200, std::move(body));
}

[[nodiscard]] HttpResponsePtr device_queue(const Call& call) {
    const Result<Uuid> device = query_device(call.req);
    if (!device) { return failure(call.req, device.error()); }
    const Result<std::vector<QueuedCiphertext>> rows =
        call.chat.device_queue(call.client, call.actor, device.value(),
                               path_id(call.req->getParameter("after")),
                               page_limit(call.req, kMaxQueuePage, kMaxQueuePage));
    if (!rows) { return failure(call.req, rows.error()); }
    std::string body;
    body.reserve(rows.value().size() * 256 + 32);
    body += '{';
    hj::append_json_key(body, "rows");
    body += '[';
    for (std::size_t i = 0; i < rows.value().size(); ++i) {
        const QueuedCiphertext& row = rows.value()[i];
        if (i != 0) { body += ','; }
        body += '{';
        hj::append_json_key(body, "id");
        hj::append_json_uuid(body, row.id);
        body += ',';
        hj::append_json_key(body, "conversation");
        hj::append_json_uuid(body, row.conversation);
        body += ',';
        hj::append_json_key(body, "seq");
        hj::append_json_int(body, row.seq);
        body += ',';
        hj::append_json_key(body, "sender");
        hj::append_json_uuid(body, row.sender);
        body += ',';
        hj::append_json_key(body, "sender_device");
        hj::append_json_uuid(body, row.sender_device);
        body += ',';
        hj::append_json_key(body, "ciphertext");
        hj::append_json_string(body, crypto::base64url_encode(row.ciphertext));
        body += '}';
    }
    body += "]}";
    return json(200, std::move(body));
}

[[nodiscard]] HttpResponsePtr acknowledge_queue(const Call& call) {
    const Result<Uuid> device = query_device(call.req);
    if (!device) { return failure(call.req, device.error()); }
    const std::optional<Uuid> through = path_id(call.req->getParameter("through"));
    if (!through.has_value()) {
        return failure(call.req, invalid("through", input::Reason::Required));
    }
    const Result<std::int64_t> deleted =
        call.chat.acknowledge_queue(call.client, call.actor, device.value(), *through);
    if (!deleted) { return failure(call.req, deleted.error()); }
    return no_content();
}

// --- the table --------------------------------------------------------------------------

// Which budget a route spends. Sending is the volume; what changes a
// conversation is rare, so a flood of sends cannot spend the budget for
// leaving a group. Reads spend none: they are bounded by the page ceilings.
enum class Spend : std::uint8_t { Nothing, Send, Write, Claim };

struct Route final {
    std::string_view ChatRouteIds::* id;
    Handler                          handler;
    drogon::HttpMethod               method;
    Path                             path;
    Spend                            spend;
    // Checks Origin. Every route that is not a GET.
    bool                             write;
    bool                             has_body;
    // One of the staff review routes (docs/22 §9.2), which an application may
    // leave out — all four or none — when it reviews nothing.
    bool                             review{false};
};

using drogon::Delete;
using drogon::Get;
using drogon::Patch;
using drogon::Post;
using drogon::Put;

constexpr std::array<Route, 44> kChatRoutes{{
    {&ChatRouteIds::create, &create, Post, Path::None, Spend::Write, true, true},
    {&ChatRouteIds::open_direct, &open_direct, Put, Path::User, Spend::Write, true, true},
    {&ChatRouteIds::list, &list, Get, Path::None, Spend::Nothing, false, false},
    {&ChatRouteIds::get, &get, Get, Path::Conversation, Spend::Nothing, false, false},
    {&ChatRouteIds::update, &update, Patch, Path::Conversation, Spend::Write, true, true},
    {&ChatRouteIds::set_timer, &set_timer, Put, Path::Conversation, Spend::Write, true, true},
    {&ChatRouteIds::members, &members, Get, Path::Conversation, Spend::Nothing, false, false},
    {&ChatRouteIds::add_members, &add_members, Post, Path::Conversation, Spend::Write, true,
     true},
    {&ChatRouteIds::update_member, &update_member, Patch, Path::ConversationUser, Spend::Write,
     true, true},
    {&ChatRouteIds::remove_member, &remove_member, Delete, Path::ConversationUser, Spend::Write,
     true, false},
    {&ChatRouteIds::send, &send, Post, Path::Conversation, Spend::Send, true, true},
    {&ChatRouteIds::history, &history, Get, Path::Conversation, Spend::Nothing, false, false},
    {&ChatRouteIds::edit, &edit, Patch, Path::ConversationSeq, Spend::Send, true, true},
    {&ChatRouteIds::revoke, &revoke, Delete, Path::ConversationSeq, Spend::Write, true, false},
    {&ChatRouteIds::react, &react, Put, Path::ConversationSeq, Spend::Send, true, true},
    {&ChatRouteIds::read_by, &read_by, Get, Path::ConversationSeq, Spend::Nothing, false,
     false},
    // Receipts ride with sends: a busy reader makes them at the same rate.
    {&ChatRouteIds::receipts, &receipts, Post, Path::Conversation, Spend::Send, true, true},
    {&ChatRouteIds::preferences, &preferences, Patch, Path::Conversation, Spend::Write, true,
     true},
    {&ChatRouteIds::create_invite, &create_invite, Post, Path::Conversation, Spend::Write, true,
     true},
    // The token in the body, never the path: a path is in every access log.
    {&ChatRouteIds::revoke_invite, &revoke_invite, Delete, Path::Conversation, Spend::Write,
     true, true},
    {&ChatRouteIds::join, &join, Post, Path::None, Spend::Write, true, true},
    {&ChatRouteIds::follow, &follow, Post, Path::Conversation, Spend::Write, true, false},
    {&ChatRouteIds::block, &block, Put, Path::User, Spend::Write, true, false},
    {&ChatRouteIds::unblock, &unblock, Delete, Path::User, Spend::Write, true, false},
    {&ChatRouteIds::presence, &presence, Get, Path::User, Spend::Nothing, false, false},
    {&ChatRouteIds::my_devices, &my_devices, Get, Path::None, Spend::Nothing, false, false},
    {&ChatRouteIds::register_device, &register_device, Put, Path::None, Spend::Write, true,
     true},
    {&ChatRouteIds::link_device, &link_device, Post, Path::None, Spend::Write, true, true},
    // The device id rides the User segment: a uuid in the path, parsed alike.
    {&ChatRouteIds::unlink_device, &unlink_device, Delete, Path::User, Spend::Write, true,
     false},
    {&ChatRouteIds::upload_prekeys, &upload_prekeys, Post, Path::None, Spend::Write, true, true},
    {&ChatRouteIds::claim_prekeys, &claim_prekeys, Post, Path::Conversation, Spend::Claim, true,
     true},
    {&ChatRouteIds::conversation_devices, &conversation_devices, Get, Path::Conversation,
     Spend::Nothing, false, false},
    // Reads with a cursor at a device's pace; bounded by the page.
    {&ChatRouteIds::device_queue, &device_queue, Get, Path::None, Spend::Nothing, false, false},
    // Acknowledgements ride with sends: a busy device makes them at that rate.
    {&ChatRouteIds::acknowledge_queue, &acknowledge_queue, Delete, Path::None, Spend::Send, true,
     false},
    // Presence for a page of accounts at once: one ask per account of the
    // hook, one MGET, at most one $in. Bounded by the query, not the budget.
    {&ChatRouteIds::presence_many, &presence_many, Get, Path::None, Spend::Nothing, false,
     false},
    // A rotation is rare, a few a month per device; it rides the write budget.
    {&ChatRouteIds::rotate_prekeys, &rotate_prekeys, Put, Path::User, Spend::Write, true, true},
    {&ChatRouteIds::request_link, &request_link, Post, Path::None, Spend::Write, true, true},
    // A read, at a person's pace; the send budget is the volume one.
    {&ChatRouteIds::read_link_request, &read_link_request, Post, Path::None, Spend::Send, true,
     true},
    {&ChatRouteIds::approve_link_request, &approve_link_request, Post, Path::None, Spend::Write,
     true, true},
    // Polled by the waiting device until the approval arrives.
    {&ChatRouteIds::collect_link_approval, &collect_link_approval, Post, Path::None, Spend::Send,
     true, true},
    // Staff review (docs/22 §9.2): installed all four or none, behind a
    // permission the application names in its route table.
    {&ChatRouteIds::review_conversation, &review_conversation, Get, Path::Conversation,
     Spend::Nothing, false, false, true},
    {&ChatRouteIds::review_history, &review_history, Get, Path::Conversation, Spend::Nothing,
     false, false, true},
    // A member's report rides the write budget: rare, and a flood of them is
    // its own abuse.
    {&ChatRouteIds::report, &report, Post, Path::Conversation, Spend::Write, true, true, true},
    {&ChatRouteIds::reports, &reports, Get, Path::None, Spend::Nothing, false, false, true},
}};

// Every id in ChatRouteIds has exactly one row, and a GET never writes: a row
// added to the struct and forgotten here is a route nobody installs.
static_assert([] {
    for (std::size_t i = 0; i < kChatRoutes.size(); ++i) {
        if ((kChatRoutes[i].method == Get) == kChatRoutes[i].write) { return false; }
        for (std::size_t j = 0; j < i; ++j) {
            if (kChatRoutes[i].id == kChatRoutes[j].id) { return false; }
        }
    }
    return true;
}());
static_assert(sizeof(ChatRouteIds) == kChatRoutes.size() * sizeof(std::string_view));

// --- installation -----------------------------------------------------------------------

[[nodiscard]] constexpr std::size_t arity(Path path) noexcept {
    switch (path) {
        case Path::None: return 0;
        case Path::User:
        case Path::Conversation: return 1;
        case Path::ConversationUser:
        case Path::ConversationSeq: return 2;
    }
    return 0;
}

[[nodiscard]] std::string pattern_of(std::span<const descriptor::RouteDescription> descriptions,
                                     std::string_view id, std::size_t placeholders) {
    for (const descriptor::RouteDescription& candidate : descriptions) {
        if (candidate.id != id) { continue; }
        const auto count = static_cast<std::size_t>(
            std::count(candidate.pattern.begin(), candidate.pattern.end(), '{'));
        if (count != placeholders) {
            throw std::invalid_argument{"chat route '" + std::string{id} + "' must carry " +
                                        std::to_string(placeholders) +
                                        " path placeholder(s)"};
        }
        return std::string{candidate.pattern};
    }
    throw std::invalid_argument{"chat route id '" + std::string{id} +
                                "' is not in the route descriptions"};
}

}  // namespace

void install_chat_routes(const ChatService& service, http::RateLimiter& limiter,
                         std::span<const accesscontrol::RoutePolicy> routes,
                         std::span<const descriptor::RouteDescription> descriptions,
                         const ChatRoutes& config) {
    // Pointers, captured by value: the handlers run long after this frame, and
    // the service and the limiter outlive serving as every handler's do. The
    // rules are copied, so the application's config need not outlive boot.
    const ChatService* chat = &service;
    http::RateLimiter* const budgets = &limiter;
    // The review routes are installed all together or not at all: a report
    // nobody can list is a promise to a member that nothing keeps.
    std::size_t review_named = 0;
    std::size_t review_total = 0;
    for (const Route& route : kChatRoutes) {
        if (!route.review) { continue; }
        ++review_total;
        if (!(config.ids.*route.id).empty()) { ++review_named; }
    }
    if (review_named != 0 && review_named != review_total) {
        throw std::invalid_argument{"chat review routes: name all four ids or none"};
    }
    for (const Route& route : kChatRoutes) {
        if (route.review && review_named == 0) { continue; }
        std::optional<Budget> budget;
        if (route.spend == Spend::Send) { budget = Budget{budgets, config.send_budget}; }
        if (route.spend == Spend::Write) { budget = Budget{budgets, config.write_budget}; }
        std::optional<Budget> target_budget;
        if (route.spend == Spend::Claim) {
            budget = Budget{budgets, config.claim_budget};
            target_budget = Budget{budgets, config.claim_target_budget};
        }
        const auto go = [chat, route, budget, target_budget](const HttpRequestPtr& req,
                                                             Responder&& cb, Segments segments) {
            run(req, std::move(cb), *chat, route.write, route.has_body, budget, target_budget,
                segments, route.handler);
        };
        std::string pattern = pattern_of(descriptions, config.ids.*route.id, arity(route.path));

        // A segment that does not parse names nothing, and answers as nothing
        // does: the stealth 404, before any work.
        switch (route.path) {
            case Path::None:
                ac::register_route(routes, std::move(pattern), route.method,
                                   [go](const HttpRequestPtr& req, Responder&& cb) {
                                       go(req, std::move(cb), Segments{});
                                   });
                break;
            case Path::User:
                ac::register_route(
                    routes, std::move(pattern), route.method,
                    [go](const HttpRequestPtr& req, Responder&& cb, const std::string& u) {
                        const std::optional<Uuid> user = path_id(u);
                        if (!user.has_value()) { return cb(ac::not_found_response()); }
                        go(req, std::move(cb), Segments{.user = *user});
                    });
                break;
            case Path::Conversation:
                ac::register_route(
                    routes, std::move(pattern), route.method,
                    [go](const HttpRequestPtr& req, Responder&& cb, const std::string& c) {
                        const std::optional<Uuid> conversation = path_id(c);
                        if (!conversation.has_value()) { return cb(ac::not_found_response()); }
                        go(req, std::move(cb), Segments{.conversation = *conversation});
                    });
                break;
            case Path::ConversationUser:
                ac::register_route(routes, std::move(pattern), route.method,
                                   [go](const HttpRequestPtr& req, Responder&& cb,
                                        const std::string& c, const std::string& u) {
                                       const std::optional<Uuid> conversation = path_id(c);
                                       const std::optional<Uuid> user = path_id(u);
                                       if (!conversation.has_value() || !user.has_value()) {
                                           return cb(ac::not_found_response());
                                       }
                                       go(req, std::move(cb),
                                          Segments{.conversation = *conversation,
                                                   .user = *user});
                                   });
                break;
            case Path::ConversationSeq:
                ac::register_route(routes, std::move(pattern), route.method,
                                   [go](const HttpRequestPtr& req, Responder&& cb,
                                        const std::string& c, const std::string& s) {
                                       const std::optional<Uuid> conversation = path_id(c);
                                       const std::optional<std::int64_t> seq = integer(s);
                                       if (!conversation.has_value() || !seq.has_value()) {
                                           return cb(ac::not_found_response());
                                       }
                                       go(req, std::move(cb),
                                          Segments{.conversation = *conversation,
                                                   .seq = *seq});
                                   });
                break;
        }
    }
}

}  // namespace anvil::chat
