#include "render.h"

#include "anvil/crypto/base64url.h"
#include "anvil/fs/sniff.h"
#include "anvil/http/json_writer.h"
#include "anvil/media/grant.h"

namespace anvil::chat::detail {

namespace hj = http;

void append_time(std::string& out, db::TimeMs at) {
    hj::append_json_time(out, at.time_since_epoch().count());
}

void append_optional_time(std::string& out, const std::optional<db::TimeMs>& at) {
    if (at.has_value()) {
        append_time(out, *at);
    } else {
        out += "null";
    }
}

void append_message(std::string& out, const ChatService& service, const MessageRecord& row,
                    std::int64_t now_unix) {
    out += '{';
    hj::append_json_key(out, "seq");
    hj::append_json_int(out, row.seq);
    out += ',';
    hj::append_json_key(out, "cid");
    hj::append_json_string(out, crypto::base64url_encode(row.client_id));
    out += ',';
    hj::append_json_key(out, "sender");
    hj::append_json_uuid(out, row.sender);
    out += ',';
    hj::append_json_key(out, "kind");
    hj::append_json_string(out, message_kind_name(row.kind));
    out += ',';
    hj::append_json_key(out, "sent_at");
    append_time(out, row.sent_at);
    out += ',';
    hj::append_json_key(out, "revoked");
    out += row.revoked ? "true" : "false";
    out += ',';
    hj::append_json_key(out, "body");
    hj::append_json_string(out, row.body);
    out += ',';
    hj::append_json_key(out, "reply_to");
    if (row.ref.has_value()) {
        hj::append_json_int(out, *row.ref);
    } else {
        out += "null";
    }
    out += ',';
    hj::append_json_key(out, "edits");
    hj::append_json_int(out, row.edits);
    out += ',';
    hj::append_json_key(out, "edited_at");
    append_optional_time(out, row.edited_at);
    out += ',';
    hj::append_json_key(out, "expires_at");
    append_optional_time(out, row.expires_at);
    out += ',';
    hj::append_json_key(out, "mentions");
    out += '[';
    for (std::size_t i = 0; i < row.mentions.size(); ++i) {
        if (i != 0) { out += ','; }
        out += '{';
        hj::append_json_key(out, "user");
        hj::append_json_uuid(out, row.mentions[i].user);
        out += ',';
        hj::append_json_key(out, "offset");
        hj::append_json_int(out, row.mentions[i].offset);
        out += ',';
        hj::append_json_key(out, "length");
        hj::append_json_int(out, row.mentions[i].length);
        out += '}';
    }
    out += "],";
    hj::append_json_key(out, "attachments");
    out += '[';
    for (std::size_t i = 0; i < row.attachments.size(); ++i) {
        const AttachmentRecord& attachment = row.attachments[i];
        if (i != 0) { out += ','; }
        out += '{';
        hj::append_json_key(out, "grant");
        hj::append_json_string(out, media::mint_grant(service.grants(), attachment.ns,
                                                      attachment.media, now_unix));
        out += ',';
        hj::append_json_key(out, "type");
        hj::append_json_string(out, fs::mime_type(attachment.mime));
        out += ',';
        hj::append_json_key(out, "name");
        hj::append_json_string(out, attachment.name);
        out += ',';
        hj::append_json_key(out, "width");
        hj::append_json_int(out, attachment.width);
        out += ',';
        hj::append_json_key(out, "height");
        hj::append_json_int(out, attachment.height);
        out += ',';
        hj::append_json_key(out, "duration_ms");
        hj::append_json_int(out, attachment.duration_ms);
        out += '}';
    }
    out += "],";
    hj::append_json_key(out, "preview");
    if (row.preview.has_value()) {
        out += '{';
        hj::append_json_key(out, "url");
        hj::append_json_string(out, row.preview->url);
        out += ',';
        hj::append_json_key(out, "title");
        hj::append_json_string(out, row.preview->title);
        out += ',';
        hj::append_json_key(out, "description");
        hj::append_json_string(out, row.preview->description);
        out += '}';
    } else {
        out += "null";
    }
    out += ',';
    hj::append_json_key(out, "card");
    if (row.card.has_value() && row.card->code < service.cards().size()) {
        out += '{';
        hj::append_json_key(out, "kind");
        hj::append_json_string(out, service.cards()[row.card->code].key);
        out += ',';
        hj::append_json_key(out, "body");
        // Canonical JSON the binder produced and the service re-parsed as an
        // object before storing it, so it is spliced in rather than re-encoded.
        out += row.card->body;
        out += '}';
    } else {
        out += "null";
    }
    out += ',';
    hj::append_json_key(out, "system");
    if (row.system.has_value()) {
        out += '{';
        hj::append_json_key(out, "event");
        hj::append_json_string(out, event_name(row.system->event));
        out += ',';
        hj::append_json_key(out, "subject");
        if (row.system->subject.has_value()) {
            hj::append_json_uuid(out, *row.system->subject);
        } else {
            out += "null";
        }
        out += ',';
        hj::append_json_key(out, "role");
        hj::append_json_string(out, role_name(row.system->role));
        out += ',';
        hj::append_json_key(out, "timer");
        hj::append_json_int(out, row.system->timer_s);
        out += '}';
    } else {
        out += "null";
    }
    out += ',';
    hj::append_json_key(out, "ciphertext");
    if (row.ciphertext.empty()) {
        out += "null";
    } else {
        hj::append_json_string(out, crypto::base64url_encode(row.ciphertext));
    }
    out += ',';
    hj::append_json_key(out, "device");
    if (row.sender_device.has_value()) {
        hj::append_json_uuid(out, *row.sender_device);
    } else {
        out += "null";
    }
    out += ',';
    // The conversation's mutation counter when this message last changed, zero
    // when it never has: what a device moves its mutation cursor to (§4.5).
    hj::append_json_key(out, "mutation");
    hj::append_json_int(out, row.mutation);
    out += '}';
}

namespace {

void append_bytes(std::string& out, std::string_view key, std::span<const std::uint8_t> bytes) {
    hj::append_json_key(out, key);
    hj::append_json_string(out, crypto::base64url_encode(bytes));
}

}  // namespace

void append_published_device(std::string& out, const PublishedDevice& device) {
    out += '{';
    hj::append_json_key(out, "id");
    hj::append_json_uuid(out, device.id);
    out += ',';
    hj::append_json_key(out, "suite");
    hj::append_json_int(out, static_cast<std::int64_t>(device.keys.suite));
    out += ',';
    append_bytes(out, "agreement_key", device.keys.agreement);
    out += ',';
    append_bytes(out, "signing_key", device.keys.signing);
    out += ',';
    append_bytes(out, "signed_prekey", device.keys.signed_prekey);
    out += ',';
    append_bytes(out, "signed_prekey_signature", device.keys.signed_prekey_signature);
    out += ',';
    append_bytes(out, "last_resort_key", device.keys.last_resort);
    out += ',';
    append_bytes(out, "last_resort_signature", device.keys.last_resort_signature);
    out += ',';
    hj::append_json_key(out, "linked_at");
    append_time(out, device.linked_at);
    out += ',';
    // Null on a first device, which nothing signed: trust on first use, and a
    // client shows it as exactly that.
    hj::append_json_key(out, "link");
    if (device.link.has_value()) {
        out += '{';
        hj::append_json_key(out, "approver");
        hj::append_json_uuid(out, device.link->approver);
        out += ',';
        append_bytes(out, "signature", device.link->signature);
        out += ',';
        hj::append_json_key(out, "timestamp");
        hj::append_json_int(out, static_cast<std::int64_t>(device.link->timestamp_s));
        out += '}';
    } else {
        out += "null";
    }
    out += '}';
}

void append_device_page(std::string& out, const ConversationDevices& page) {
    out += '{';
    hj::append_json_key(out, "dsv");
    hj::append_json_int(out, page.device_set_version);
    out += ',';
    hj::append_json_key(out, "members");
    out += '[';
    for (std::size_t i = 0; i < page.members.size(); ++i) {
        if (i != 0) { out += ','; }
        out += '{';
        hj::append_json_key(out, "user");
        hj::append_json_uuid(out, page.members[i].user);
        out += ',';
        hj::append_json_key(out, "devices");
        out += '[';
        for (std::size_t j = 0; j < page.members[i].devices.size(); ++j) {
            if (j != 0) { out += ','; }
            append_published_device(out, page.members[i].devices[j]);
        }
        out += "]}";
    }
    out += "],";
    hj::append_json_key(out, "next");
    if (page.next.has_value()) {
        hj::append_json_uuid(out, *page.next);
    } else {
        out += "null";
    }
    out += '}';
}

}  // namespace anvil::chat::detail
