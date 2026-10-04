#pragma once

// The Content CMS (anvil sections), the photo galleries (anvil entries), image
// uploads (anvil fs + media pipeline) and media serving (X-Accel-Redirect).

#include <optional>
#include <string>
#include <string_view>

#include <mongocxx/client.hpp>

#include "anvil/entries/registry.h"
#include "anvil/sections/content.h"
#include "anvil/sections/registry.h"
#include "app/http.h"

namespace enactus {

// A gallery kind by its short URL name ("about", "life", "tafrah"), never by
// the raw request text: the table's own .rodata spelling is what travels on.
[[nodiscard]] const anvil::entries::KindSpec* gallery_kind(std::string_view short_name) noexcept;

// The image URL base every payload builds `src` from. A client appends a role:
// `/media/site/<id>/card`.
inline constexpr std::string_view kImageBase = "/media/site";

// Appends `{"id":"…","src":"…"}` for an image slot of `content`, or `null`.
void append_image(std::string& out, const anvil::sections::SectionContent& content,
                  std::string_view slot);

// Appends a published copy's data fields as a flat JSON object.
void append_data(std::string& out, const anvil::sections::SectionSpec& shape,
                 const anvil::sections::SectionContent& content);

namespace routes {
void sections_list(const http::HttpRequestPtr& req, http::Responder&& respond);
void sections_publish(const http::HttpRequestPtr& req, http::Responder&& respond,
                      const std::string& key);
void gallery_list(const http::HttpRequestPtr& req, http::Responder&& respond,
                  const std::string& kind);
void gallery_add(const http::HttpRequestPtr& req, http::Responder&& respond,
                 const std::string& kind);
void gallery_remove(const http::HttpRequestPtr& req, http::Responder&& respond,
                    const std::string& kind, const std::string& id);
void gallery_reorder(const http::HttpRequestPtr& req, http::Responder&& respond,
                     const std::string& kind);
void media_upload(const http::HttpRequestPtr& req, http::Responder&& respond);
void media_object(const http::HttpRequestPtr& req, http::Responder&& respond,
                  const std::string& ns, const std::string& id, const std::string& role);
}  // namespace routes

}  // namespace enactus
