#include "anvil/descriptor/descriptor.h"

#include <array>
#include <cstddef>
#include <utility>

#include "anvil/core/locale.h"
#include "anvil/core/types.h"
#include "anvil/core/version.h"
#include "anvil/crypto/digest.h"
#include "anvil/fs/namespace.h"
#include "anvil/fs/namespace_spec.h"
#include "anvil/http/errors.h"
#include "anvil/http/json_writer.h"
#include "anvil/http/response_spec.h"
#include "anvil/input/fields.h"

namespace anvil::descriptor {

namespace {

// Every ErrorCode and every Reason, listed once, in numeric order.
//
// A range-for over an enum is not a thing C++ has, and the alternative — casting
// 0..kMax and hoping every value in between is an enumerator — emits a name for
// a value that does not exist the moment the enum grows a gap. The lists are
// asserted against their bounds below, so adding a code without adding it here
// is a build failure rather than a client with a hole in its vocabulary.
constexpr std::array<ErrorCode, 15> kAllErrorCodes{
    ErrorCode::Ok,
    ErrorCode::Unauthenticated,
    ErrorCode::Forbidden,
    ErrorCode::NotFound,
    ErrorCode::CapabilityRequired,
    ErrorCode::CapabilityInvalid,
    ErrorCode::ValidationFailed,
    ErrorCode::Conflict,
    ErrorCode::VersionMismatch,
    ErrorCode::RateLimited,
    ErrorCode::PayloadTooLarge,
    ErrorCode::UnsupportedMedia,
    ErrorCode::ServiceUnavailable,
    ErrorCode::Internal,
    ErrorCode::InsufficientStorage,
};

constexpr std::array<input::Reason, 11> kAllReasons{
    input::Reason::Ok,          input::Reason::Required,   input::Reason::TooShort,
    input::Reason::TooLong,     input::Reason::BadFormat,  input::Reason::BadCharset,
    input::Reason::OutOfRange,  input::Reason::NotAllowed, input::Reason::BadChecksum,
    input::Reason::Weak,        input::Reason::Breached,
};

static_assert(kAllErrorCodes.back() == kMaxErrorCode,
              "an ErrorCode was appended without being listed here, so a generated "
              "client would have no name for it");
static_assert(kAllReasons.back() == http::kMaxReason,
              "a validation Reason was appended without being listed here");

void append_error_codes(std::string& out) {
    http::append_json_key(out, "error_codes");
    out += '{';
    http::append_json_key(out, "max");
    http::append_json_int(out, static_cast<std::int64_t>(kMaxErrorCode));
    out += ',';
    http::append_json_key(out, "codes");
    out += '[';
    bool first = true;
    for (const ErrorCode code : kAllErrorCodes) {
        if (!first) { out += ','; }
        first = false;
        out += '{';
        http::append_json_key(out, "name");
        http::append_json_string(out, http::wire_name(code));
        out += ',';
        http::append_json_key(out, "value");
        http::append_json_int(out, static_cast<std::int64_t>(code));
        out += ',';
        http::append_json_key(out, "http");
        http::append_json_int(out, http::http_status(code));
        out += ',';
        // A client must know which codes the filter rewrites to the stealth 404,
        // because on those routes it may not report a denial as a denial.
        http::append_json_key(out, "stealth_hidden");
        out += http::is_stealth_hidden(code) ? "true" : "false";
        out += '}';
    }
    out += "]}";
}

void append_validation_reasons(std::string& out) {
    http::append_json_key(out, "validation_reasons");
    out += '[';
    bool first = true;
    for (const input::Reason reason : kAllReasons) {
        if (!first) { out += ','; }
        first = false;
        out += '{';
        http::append_json_key(out, "name");
        http::append_json_string(out, http::wire_name(reason));
        out += ',';
        http::append_json_key(out, "value");
        http::append_json_int(out, static_cast<std::int64_t>(reason));
        out += '}';
    }
    out += ']';
}

void append_permissions(std::string& out, std::span<const PermName> permissions) {
    http::append_json_key(out, "permissions");
    out += '[';
    bool first = true;
    for (const PermName& p : permissions) {
        if (!first) { out += ','; }
        first = false;
        out += '{';
        http::append_json_key(out, "name");
        http::append_json_string(out, p.name);
        out += ',';
        http::append_json_key(out, "bit");
        http::append_json_int(out, p.bit);
        out += '}';
    }
    out += ']';
}

// The locale table, in the server's order, which is the order the stored
// one-byte index means. A client that sorts this table renumbers every locale
// every document already carries.
void append_locales(std::string& out) {
    http::append_json_key(out, "locales");
    out += '[';
    for (std::size_t i = 0; i < kLocaleCount; ++i) {
        if (i != 0) { out += ','; }
        const LocaleSpec& spec = config::kLocales[i];
        out += '{';
        http::append_json_key(out, "tag");
        http::append_json_string(out, spec.tag);
        out += ',';
        http::append_json_key(out, "collation");
        http::append_json_string(out, spec.collation);
        out += ',';
        http::append_json_key(out, "rtl");
        out += spec.rtl ? "true" : "false";
        out += '}';
    }
    out += ']';
}

// The permission names a mask contains, in BIT order rather than table order, so
// two emissions of the same mask are byte-identical whatever order the name
// table happens to be written in. The hash depends on it.
void append_permission_names(std::string& out, const PermCatalogue& catalogue,
                             const PermSet& mask) {
    out += '[';
    bool first = true;
    catalogue.for_each_name(mask, [&](std::string_view name) {
        if (!first) { out += ','; }
        first = false;
        http::append_json_string(out, name);
    });
    out += ']';
}

// The declared shape of one route's success body.
//
// Emitted as `{"kind":…,"fields":[…]}` rather than as a bare array, because
// "one object of this shape" and "a list of them" are two different types in
// every client and a wrapper is what lets the second exist without a second key.
void append_response(std::string& out, const RouteDescription& d) {
    if (d.response.empty()) {
        out += "null";
        return;
    }
    out += '{';
    http::append_json_key(out, "kind");
    http::append_json_string(out, d.response_is_array ? "array" : "object");
    out += ',';
    http::append_json_key(out, "fields");
    out += '[';
    for (std::size_t i = 0; i < d.response.size(); ++i) {
        if (i != 0) { out += ','; }
        const http::ResponseField& field = d.response[i];
        out += '{';
        http::append_json_key(out, "name");
        http::append_json_string(out, field.name);
        out += ',';
        http::append_json_key(out, "type");
        http::append_json_string(out, http::kind_name(field.kind));
        out += ',';
        // A client's type for a nullable field is a different type, so this is
        // not decoration: a field declared always-present and sent as null is a
        // crash in the consumer, on the field it was told it could trust.
        http::append_json_key(out, "nullable");
        out += field.nullable ? "true" : "false";
        out += '}';
    }
    out += ']';
    out += '}';
}

void append_routes(std::string& out, const DescriptorInput& input,
                   const PermCatalogue& catalogue) {
    http::append_json_key(out, "routes");
    out += '[';

    // Iterated in DESCRIPTION order, not policy order, because the descriptions
    // are the table a client is generated from and their order is the one an
    // application controls. Both are validated to be the same set.
    bool first = true;
    for (const RouteDescription& d : input.route_descriptions) {
        const accesscontrol::RoutePolicy* policy =
            accesscontrol::policy_for(input.routes, d.pattern, d.method);
        if (policy == nullptr) { continue; }  // descriptions_match() forbids this

        if (!first) { out += ','; }
        first = false;

        out += '{';
        http::append_json_key(out, "id");
        http::append_json_string(out, d.id);
        out += ',';
        http::append_json_key(out, "method");
        http::append_json_string(out, method_name(d.method));
        out += ',';
        http::append_json_key(out, "path");
        http::append_json_string(out, d.pattern);
        out += ',';
        http::append_json_key(out, "access");
        http::append_json_string(out, access_name(policy->access));
        out += ',';
        // The field a generator branches on: a public path is compiled into the
        // bundle, a holder path is never emitted and arrives with the session.
        //
        // This is the DISCLOSURE answer and `access` above is the AUTHORITY one,
        // and a bootstrap route is exactly where they differ: `session.current`
        // emits "public" here and "authenticated" there, which are two true
        // things about it rather than the one false thing a single field had to
        // pick between.
        http::append_json_key(out, "visibility");
        http::append_json_string(out, path_in_bundle(d, policy->access) ? "public" : "holder");
        out += ',';
        http::append_json_key(out, "perms");
        append_permission_names(out, catalogue, policy->required);
        out += ',';
        http::append_json_key(out, "capability");
        http::append_json_string(out, d.capability);
        out += ',';
        http::append_json_key(out, "rate_limit");
        http::append_json_string(out, d.rate_bucket);
        out += ',';
        http::append_json_key(out, "idempotent");
        out += d.idempotent ? "true" : "false";
        out += ',';
        http::append_json_key(out, "page");
        if (d.cursor_field.empty()) {
            out += "null";
        } else {
            out += '{';
            http::append_json_key(out, "cursor");
            http::append_json_string(out, d.cursor_field);
            out += ',';
            http::append_json_key(out, "limit_max");
            http::append_json_int(out, d.limit_max);
            out += '}';
        }
        out += ',';
        // The success body's declared shape, or null.
        //
        // Null is the honest answer and not a hole to be filled on a schedule.
        // Every route emitted null before `http/response_writer.h` existed, and
        // a route whose body is richer than that grammar still does — a client
        // falls back to its own declaration there, which is what it has always
        // done. What the writer changes is that a NON-null answer is a
        // description of the bytes rather than a claim about them: the only way
        // to produce a described body is to walk this declaration in order, so
        // the two cannot drift.
        http::append_json_key(out, "response");
        append_response(out, d);
        out += '}';
    }
    out += ']';
}

void append_capability_scopes(std::string& out,
                              std::span<const identity::CapabilityScopeSpec> scopes) {
    http::append_json_key(out, "capability_scopes");
    out += '[';
    bool first = true;
    for (const identity::CapabilityScopeSpec& s : scopes) {
        if (!first) { out += ','; }
        first = false;
        out += '{';
        http::append_json_key(out, "name");
        http::append_json_string(out, s.name);
        out += ',';
        http::append_json_key(out, "value");
        http::append_json_int(out, s.value);
        out += ',';
        // Whether redemption burns the token, and therefore whether a client may
        // ever retry a call carrying one. A single-use capability is consumed
        // whether or not the response arrived, so a retry reports failure for an
        // operation that succeeded.
        http::append_json_key(out, "single_use");
        out += s.single_use ? "true" : "false";
        out += '}';
    }
    out += ']';
}

void append_rate_limits(std::string& out, std::span<const http::RateLimitRule> rules) {
    http::append_json_key(out, "rate_limits");
    out += '[';
    bool first = true;
    for (const http::RateLimitRule& rule : rules) {
        if (!first) { out += ','; }
        first = false;
        out += '{';
        http::append_json_key(out, "bucket");
        http::append_json_string(out, rule.bucket);
        out += ',';
        // Milliseconds, named in the key, because a duration whose unit is in a
        // comment is a duration that gets read as seconds.
        http::append_json_key(out, "window_ms");
        http::append_json_int(out, rule.window.count());
        out += ',';
        http::append_json_key(out, "max_events");
        http::append_json_int(out, rule.max_events);
        out += '}';
    }
    out += ']';
}

void append_limits(std::string& out, const Limits& limits) {
    http::append_json_key(out, "limits");
    out += '{';
    http::append_json_key(out, "upload_max_bytes");
    http::append_json_int(out, static_cast<std::int64_t>(limits.upload_max_bytes));
    out += ',';
    http::append_json_key(out, "body_max_bytes");
    http::append_json_int(out, static_cast<std::int64_t>(limits.body_max_bytes));
    out += ',';
    http::append_json_key(out, "page_limit_max");
    http::append_json_int(out, limits.page_limit_max);
    out += '}';
}


// --- the content tables -----------------------------------------------------
//
// What a client RENDERS, as against the tables above, which are how it calls.
// Each of these is an application's table validated by a static_assert beside
// the table itself, so nothing here re-checks one; what it does is put each
// table into the vocabulary a generated client already speaks — names rather
// than the bit patterns the server stores.

// The flags as an OBJECT of booleans rather than the byte the server holds.
//
// A client handed `"flags":17` needs a copy of anvil's enum to read it, and a
// copy of an enum is the second table this whole document exists to remove. The
// byte is the storage representation; it is not a thing anybody outside this
// process should have to decode.
void append_field_type_flags(std::string& out, forms::FieldTypeFlag flags) {
    using F = forms::FieldTypeFlag;
    constexpr std::array<std::pair<std::string_view, F>, 6> kNamed{{
        {"options", F::Options},
        {"attachment", F::Attachment},
        {"ranged", F::Ranged},
        {"code_point_capped", F::CodePointCapped},
        {"multi_line", F::MultiLine},
        {"multi_select", F::MultiSelect},
    }};

    out += '{';
    bool first = true;
    for (const auto& [name, flag] : kNamed) {
        if (!first) { out += ','; }
        first = false;
        http::append_json_key(out, name);
        out += forms::has_flag(flags, flag) ? "true" : "false";
    }
    out += '}';
}

void append_field_types(std::string& out, std::span<const forms::FieldTypeSpec> types) {
    http::append_json_key(out, "field_types");
    out += '[';
    bool first = true;
    for (const forms::FieldTypeSpec& spec : types) {
        if (!first) { out += ','; }
        first = false;
        out += '{';
        http::append_json_key(out, "name");
        http::append_json_string(out, spec.wire_name);
        out += ',';
        // STORED, and append-only. A client that persists a draft against a code
        // is persisting the same number the server writes into every field of
        // every definition, which is why the code travels and the table index
        // does not.
        http::append_json_key(out, "code");
        http::append_json_int(out, spec.code);
        out += ',';
        // Whether this type's value is an IDENTITY. A client must know, because a
        // PII field's value is never echoed back: the answer it submitted is not
        // in the document it reads afterwards, and a form renderer that expects
        // it there renders a field that has silently emptied itself.
        const bool pii = forms::has_flag(spec.flags, forms::FieldTypeFlag::Pii);
        http::append_json_key(out, "pii");
        out += pii ? "true" : "false";
        out += ',';
        // The JSON shape an answer takes — null for a PII type, which produces no
        // answer at all rather than an empty one.
        http::append_json_key(out, "answer");
        if (pii) {
            out += "null";
        } else {
            http::append_json_string(out, forms::answer_kind_name(
                                              forms::answer_kind_of(spec.flags)));
        }
        out += ',';
        // CODE POINTS, and the name says so: a client enforcing it in UTF-16 code
        // units refuses text the server would have accepted, which for any
        // non-Latin script is most of it (ENGINEERING_RULES.md §8).
        http::append_json_key(out, "default_code_points");
        http::append_json_int(out, spec.default_code_points);
        out += ',';
        http::append_json_key(out, "flags");
        append_field_type_flags(out, spec.flags);
        out += '}';
    }
    out += ']';
}

// One value per declared locale, in the LOCALE TABLE's order — the same order
// `append_locales` emits and the same order the stored one-byte index means. An
// array rather than an object keyed by tag, because the index is what everything
// else in this document is keyed by.
void append_localized(std::string& out, const LocalizedView& text) {
    out += '[';
    for (std::size_t i = 0; i < kLocaleCount; ++i) {
        if (i != 0) { out += ','; }
        http::append_json_string(out, text.values[i]);
    }
    out += ']';
}

void append_section_fields(std::string& out, std::span<const sections::FieldSpec> fields) {
    out += '[';
    bool first = true;
    for (const sections::FieldSpec& field : fields) {
        if (!first) { out += ','; }
        first = false;
        out += '{';
        http::append_json_key(out, "key");
        http::append_json_string(out, field.key);
        out += ',';
        http::append_json_key(out, "type");
        http::append_json_string(out, sections::field_type_name(field.type));
        out += ',';
        // The label the STAFF editor puts next to the control. Without it the only
        // string a generated editor has is the key, so the box reads `cta_href`.
        http::append_json_key(out, "labels");
        append_localized(out, field.label);
        out += ',';
        http::append_json_key(out, "max_code_points");
        http::append_json_int(out, field.max_cp);
        out += ',';
        http::append_json_key(out, "localized");
        out += field.localized ? "true" : "false";
        out += ',';
        http::append_json_key(out, "required");
        out += field.required ? "true" : "false";
        out += ',';
        // In the table's order, which the registry asserts is SORTED. A client
        // that re-sorts it is free to; what it must not do is invent a value, and
        // the list is what stops a Choice control offering one.
        http::append_json_key(out, "choices");
        out += '[';
        for (std::size_t i = 0; i < field.choices.size(); ++i) {
            if (i != 0) { out += ','; }
            http::append_json_string(out, field.choices[i]);
        }
        out += ']';
        out += '}';
    }
    out += ']';
}

void append_section_images(std::string& out, std::span<const sections::ImageSpec> images) {
    out += '[';
    bool first = true;
    for (const sections::ImageSpec& image : images) {
        if (!first) { out += ','; }
        first = false;
        out += '{';
        http::append_json_key(out, "slot");
        http::append_json_string(out, image.slot);
        out += ',';
        http::append_json_key(out, "labels");
        append_localized(out, image.label);
        out += ',';
        http::append_json_key(out, "min_width");
        http::append_json_int(out, image.min_width);
        out += ',';
        http::append_json_key(out, "min_height");
        http::append_json_int(out, image.min_height);
        out += ',';
        // NULL when either half is zero, never `{"num":0,"den":0}`. An object is
        // always truthy, so a slot with no constraint rendered "shaped 0:0" beside
        // the upload control — a requirement a staff member can neither satisfy
        // nor recognise as nothing being asked.
        http::append_json_key(out, "aspect");
        if (image.aspect_num == 0 || image.aspect_den == 0) {
            out += "null";
        } else {
            out += '{';
            http::append_json_key(out, "num");
            http::append_json_int(out, image.aspect_num);
            out += ',';
            http::append_json_key(out, "den");
            http::append_json_int(out, image.aspect_den);
            out += '}';
        }
        out += '}';
    }
    out += ']';
}

void append_sections(std::string& out, std::span<const sections::SectionSpec> registry) {
    http::append_json_key(out, "sections");
    out += '[';
    bool first = true;
    for (const sections::SectionSpec& section : registry) {
        if (!first) { out += ','; }
        first = false;
        out += '{';
        http::append_json_key(out, "key");
        http::append_json_string(out, section.key);
        out += ',';
        // The page a preview URL has to land the reader on. Deriving it from the
        // key would be a second implementation of something the registry already
        // holds, in the one place that cannot check its own answer.
        http::append_json_key(out, "site_path");
        http::append_json_string(out, section.site_path);
        out += ',';
        http::append_json_key(out, "fields");
        append_section_fields(out, section.fields);
        out += ',';
        http::append_json_key(out, "images");
        append_section_images(out, section.images);
        out += '}';
    }
    out += ']';
}

void append_topics(std::string& out, std::span<const notifications::TopicSpec> topics,
                   const PermCatalogue& catalogue) {
    http::append_json_key(out, "topics");
    out += '[';
    bool first = true;
    for (const notifications::TopicSpec& topic : topics) {
        if (!first) { out += ','; }
        first = false;
        out += '{';
        http::append_json_key(out, "key");
        http::append_json_string(out, topic.key);
        out += ',';
        // STORED, and a BIT POSITION in every client's preference masks. The same
        // append-only rule the permission bits live under, and it fails the same
        // way: a renumbered code silently reinterprets every preference already
        // written.
        http::append_json_key(out, "code");
        http::append_json_int(out, topic.code);
        out += ',';
        http::append_json_key(out, "fanout");
        http::append_json_string(out, notifications::fanout_name(topic.fanout));
        out += ',';
        http::append_json_key(out, "scope");
        http::append_json_string(out, notifications::scope_name(topic.scope));
        out += ',';
        http::append_json_key(out, "default_channels");
        out += '[';
        bool first_channel = true;
        for (std::size_t i = 0; i < notifications::kChannelCount; ++i) {
            const auto channel = static_cast<notifications::ClientType>(i);
            if (!notifications::has_channel(topic.default_channels, channel)) { continue; }
            if (!first_channel) { out += ','; }
            first_channel = false;
            http::append_json_string(out, notifications::channel_name(channel));
        }
        out += ']';
        out += ',';
        http::append_json_key(out, "coalesce_window_s");
        http::append_json_int(out, topic.coalesce_window_s);
        out += ',';
        http::append_json_key(out, "retention_days");
        http::append_json_int(out, topic.retention_days);
        out += ',';
        // Whether the reader may turn it off. A client that offers a switch for a
        // topic that is not optional offers a switch the server refuses, and a
        // security alert an account can silence is not an alert.
        http::append_json_key(out, "user_optional");
        out += topic.user_optional ? "true" : "false";
        out += ',';
        http::append_json_key(out, "stealth_on_denial");
        out += topic.stealth_on_denial ? "true" : "false";
        out += ',';
        http::append_json_key(out, "perms");
        append_permission_names(out, catalogue, topic.required);
        out += ',';
        // The same split `path_in_bundle` draws over routes, and for the same
        // reason: SUBSCRIPTION IS THE DISCLOSURE. A topic gated by a permission is
        // one whose existence is part of what the permission protects, so its name
        // may not be compiled into a bundle — a bundle is a public file, and a
        // lazily-loaded chunk is a public URL. A holder learns which of these it
        // may subscribe to from the preferences endpoint, filtered to it.
        http::append_json_key(out, "visibility");
        http::append_json_string(out, topic.required.none() ? "public" : "holder");
        out += '}';
    }
    out += ']';
}

void append_events(std::string& out, std::span<const analytics::EventSpec> events) {
    http::append_json_key(out, "events");
    out += '[';
    bool first = true;
    for (const analytics::EventSpec& event : events) {
        if (!first) { out += ','; }
        first = false;
        out += '{';
        http::append_json_key(out, "name");
        http::append_json_string(out, event.name);
        out += ',';
        http::append_json_key(out, "code");
        http::append_json_int(out, event.code);
        out += ',';
        http::append_json_key(out, "class");
        http::append_json_string(out, analytics::event_class_name(event.cls));
        out += ',';
        // Refused at the door rather than filtered later, which is why a client
        // must know it: an event declaring this is never buffered and never
        // written without consent, so a client that offers it anyway is reporting
        // into a refusal it cannot see.
        http::append_json_key(out, "requires_consent");
        out += event.requires_consent ? "true" : "false";
        out += ',';
        // The CLOSED value set, because the row stores the INDEX into it. A client
        // sending a value that is not here sends one the ingest path drops, and a
        // dimension whose values came from a request is the cardinality explosion
        // the closed set exists to refuse (docs/17-analytics.md §6).
        http::append_json_key(out, "dimensions");
        out += '[';
        for (std::size_t d = 0; d < event.dimensions.size(); ++d) {
            if (d != 0) { out += ','; }
            const analytics::DimensionSpec& dimension = event.dimensions[d];
            out += '{';
            http::append_json_key(out, "name");
            http::append_json_string(out, dimension.name);
            out += ',';
            http::append_json_key(out, "values");
            out += '[';
            for (std::size_t v = 0; v < dimension.values.size(); ++v) {
                if (v != 0) { out += ','; }
                http::append_json_string(out, dimension.values[v]);
            }
            out += ']';
            out += '}';
        }
        out += ']';
        out += '}';
    }
    out += ']';
}

// --- the media table, and the srcset argument it settles --------------------
//
// `fs/namespace_spec.h` states plainly that the width ladder stays server-side:
// a client that knows the ladder is a client that will start building paths from
// it again, and the public grammar is `GET /media/{ns}/{id}/{role}` precisely so
// that it cannot. A responsive `srcset`, meanwhile, needs width descriptors —
// `320w`, `1024w` — or the browser cannot choose between the sources it is given.
//
// The two are reconcilable, and this is where: the objection is to a client
// CONSTRUCTING a path, not to it knowing a number. So a width is emitted ATTACHED
// to the role it belongs to, and nothing else is emitted at all — no ladder, no
// file extension, no format list. A client can then write
//
//     <img srcset="/media/content/{id}/thumb 320w, /media/content/{id}/card 1024w">
//
// where every URL is still a role and every number is still the server's. What it
// cannot do is assemble `w640.avif`, because it has never been told either half.
//
// The role→width mapping may change without a client release, and that stays
// true: a client re-fetching the descriptor gets new numbers against the same
// role names, and the paths it already holds keep working while it does.
void append_media(std::string& out) {
    http::append_json_key(out, "media");
    out += '{';
    // What a request with no role segment means, and therefore what a caller that
    // FORGETS the segment gets.
    http::append_json_key(out, "default_role");
    http::append_json_string(out, fs::role_name(fs::kDefaultRole));
    out += ',';
    http::append_json_key(out, "namespaces");
    out += '[';
    for (std::size_t n = 0; n < fs::kNsCount; ++n) {
        if (n != 0) { out += ','; }
        const fs::Ns ns = fs::kAllNamespaces[n];
        out += '{';
        // The `{ns}` segment of a media URL. It is the directory name too — the
        // seam keeps one string for both so the two cannot disagree — but what is
        // emitted here is the URL half, and a client never sees a path.
        http::append_json_key(out, "ns");
        http::append_json_string(out, ns.dir());
        out += ',';
        // What this namespace will take, from the same table the upload path
        // reads to refuse one. The emitted list and the enforced list are one
        // table read twice, which is what stops a client's file picker from
        // being a second copy of a server rule — and it is per namespace,
        // because a global list would close the copy and not the case that
        // reopens it.
        //
        // SVG never appears here, and not by omission: there is no Mime for it,
        // because fs/sniff.h refuses it EXPLICITLY so the rejection is
        // auditable as the probe it is.
        http::append_json_key(out, "accepts");
        out += '[';
        {
            bool first_mime = true;
            for (unsigned value = 1; value <= static_cast<unsigned>(fs::kMaxMime); ++value) {
                const auto mime = static_cast<fs::Mime>(value);
                if (!fs::mime_accepted(ns.accepts(), mime)) { continue; }
                if (!first_mime) { out += ','; }
                first_mime = false;
                http::append_json_string(out, fs::mime_type(mime));
            }
        }
        out += ']';
        out += ',';
        http::append_json_key(out, "roles");
        out += '[';
        for (std::size_t r = 0; r < fs::kRoleCount; ++r) {
            if (r != 0) { out += ','; }
            const auto role = static_cast<fs::MediaRole>(r);
            out += '{';
            http::append_json_key(out, "role");
            http::append_json_string(out, fs::role_name(role));
            out += ',';
            http::append_json_key(out, "width");
            http::append_json_int(out, fs::role_width(ns, role));
            out += '}';
        }
        out += ']';
        out += '}';
    }
    out += ']';
    out += '}';
}

void append_tables(std::string& out, const DescriptorInput& input) {
    const PermCatalogue catalogue{input.permissions};

    out += '{';
    append_error_codes(out);
    out += ',';
    append_validation_reasons(out);
    out += ',';
    append_permissions(out, input.permissions);
    out += ',';
    append_locales(out);
    out += ',';
    append_routes(out, input, catalogue);
    out += ',';
    append_capability_scopes(out, input.capability_scopes);
    out += ',';
    append_rate_limits(out, input.rate_limits);
    out += ',';
    append_limits(out, input.limits);
    out += ',';
    append_field_types(out, input.field_types);
    out += ',';
    append_sections(out, input.sections);
    out += ',';
    append_topics(out, input.topics, catalogue);
    out += ',';
    append_events(out, input.events);
    out += ',';
    append_media(out);
    out += '}';
}

void append_hex(std::string& out, std::span<const std::uint8_t> bytes) {
    constexpr std::string_view kHex = "0123456789abcdef";
    out += '"';
    for (const std::uint8_t byte : bytes) {
        out += kHex[byte >> 4U];
        out += kHex[byte & 0x0FU];
    }
    out += '"';
}

}  // namespace

void append_descriptor(std::string& out, const DescriptorInput& input) {
    // The tables are built first because the hash is over their bytes, and the
    // hash is emitted before them: a reader that streams the document gets the
    // answer to "is this the server I am talking to" before it has parsed a
    // table it may be about to discard.
    std::string tables;
    // The content tables took the reference application's document well past
    // eight kilobytes, and a section registry or a field-type table larger than
    // that one is the ordinary case rather than the exotic one.
    tables.reserve(16384);
    append_tables(tables, input);

    const crypto::Digest256 digest = crypto::sha256(std::string_view{tables});

    out.reserve(out.size() + tables.size() + 256);
    out += '{';
    http::append_json_key(out, "descriptor");
    http::append_json_int(out, kDescriptorFormat);
    out += ',';
    http::append_json_key(out, "emitted_by");
    http::append_json_string(out, version_string());
    out += ',';
    http::append_json_key(out, "app");
    out += '{';
    http::append_json_key(out, "name");
    http::append_json_string(out, input.app_name);
    out += ',';
    http::append_json_key(out, "version");
    http::append_json_string(out, input.app_version);
    out += '}';
    out += ',';
    http::append_json_key(out, "hash");
    append_hex(out, digest);
    out += ',';
    http::append_json_key(out, "tables");
    out += tables;
    out += '}';
}

std::string emit_descriptor(const DescriptorInput& input) {
    std::string out;
    append_descriptor(out, input);
    return out;
}

}  // namespace anvil::descriptor
