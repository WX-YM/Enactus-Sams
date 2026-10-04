#include "app/legacy.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <string>
#include <utility>

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/document/view.hpp>
#include <bsoncxx/stdx/optional.hpp>
#include <bsoncxx/types.hpp>
#include <mongocxx/options/find.hpp>

#include "anvil/accounts/identifier.h"
#include "anvil/auth/prehash.h"
#include "anvil/core/uuid.h"
#include "anvil/db/versioned.h"
#include "anvil/entries/registry.h"
#include "anvil/forms/repository.h"
#include "anvil/forms/service.h"
#include "anvil/http/json_writer.h"
#include "anvil/input/arena.h"
#include "anvil/input/json.h"
#include "anvil/sections/payload.h"
#include "anvil/identity/login_identity.h"
#include "anvil/sections/default_images.h"
#include "anvil/sections/registry.h"
#include "app/applications.h"
#include "app/services.h"
#include "app/staff.h"
#include "app/teams.h"
#include "entries.h"
#include "field_types.h"
#include "perms.h"
#include "sections.h"

namespace enactus::legacy {

namespace {

namespace ent = anvil::entries;
namespace sec = anvil::sections;
namespace fm = anvil::forms;
using anvil::ErrorCode;
using anvil::Uuid;
using bsoncxx::builder::basic::kvp;
using bsoncxx::builder::basic::make_document;

constexpr std::string_view kLegacySuperAdminEmail = "admin@enactussams.org";
constexpr std::string_view kDatabaseCollectionUsers = "users";
constexpr std::string_view kDatabaseCollectionTeams = "teams";
constexpr std::string_view kDatabaseCollectionApplications = "applications";
constexpr std::string_view kDatabaseCollectionContent = "content";
constexpr std::string_view kDatabaseCollectionFormSchema = "form_schema";
constexpr std::string_view kDatabaseCollectionFormSubmissions = "form_submissions";

// Every legacy content key with a section field to go to.
struct KeyMap final {
    std::string_view legacy;
    std::string_view section;
    std::string_view field;
};

constexpr std::array<KeyMap, 61> kKeyMap{{
    {"aboutHeading", "home.about", "heading"},
    {"aboutKicker", "home.about", "kicker"},
    {"aboutP1", "home.about", "p1"},
    {"aboutP2", "home.about", "p2"},
    {"aboutTags", "home.about", "tags"},
    {"closedBannerDesc", "home.join", "closed_desc"},
    {"closedBannerTitle", "home.join", "closed_title"},
    {"footerNote", "home.footer", "note"},
    {"footerSocialFb", "home.footer", "facebook"},
    {"footerSocialInsta", "home.footer", "instagram"},
    {"footerSocialLinkedin", "home.footer", "linkedin"},
    {"footerSocialTiktok", "home.footer", "tiktok"},
    {"heroCampus", "home.hero", "campus"},
    {"heroChapter", "home.hero", "chapter"},
    {"heroCta1", "home.hero", "cta1"},
    {"heroCta2", "home.hero", "cta2"},
    {"heroHeadline1", "home.hero", "headline1"},
    {"heroHeadline2", "home.hero", "headline2"},
    {"heroHeadline3", "home.hero", "headline3"},
    {"heroStat1Label", "home.hero", "stat1_label"},
    {"heroStat1Num", "home.hero", "stat1_num"},
    {"heroStat2Label", "home.hero", "stat2_label"},
    {"heroStat2Num", "home.hero", "stat2_num"},
    {"heroStat3Label", "home.hero", "stat3_label"},
    {"heroStat3Num", "home.hero", "stat3_num"},
    {"heroSubtitle", "home.hero", "subtitle"},
    {"heroTagline", "home.hero", "tagline"},
    {"insideDesc", "home.inside", "desc"},
    {"insideKicker", "home.inside", "kicker"},
    {"insideTitle", "home.inside", "title"},
    {"joinCtaClosed", "home.join", "cta_closed"},
    {"joinCtaOpen", "home.join", "cta_open"},
    {"joinDesc", "home.join", "desc"},
    {"joinKickerClosed", "home.join", "kicker_closed"},
    {"joinKickerOpen", "home.join", "kicker_open"},
    {"joinNoteClosed", "home.join", "note_closed"},
    {"joinNoteOpen", "home.join", "note_open"},
    {"joinTitle", "home.join", "title"},
    {"lifeKicker", "home.life", "kicker"},
    {"lifeSubtitle", "home.life", "subtitle"},
    {"lifeTitle", "home.life", "title"},
    {"recruitmentOpen", "home.join", "open"},
    {"tafrahDesc", "home.tafrah", "desc"},
    {"tafrahFooter", "home.tafrah", "footer"},
    {"tafrahH1Desc", "home.tafrah", "h1_desc"},
    {"tafrahH1Title", "home.tafrah", "h1_title"},
    {"tafrahH2Desc", "home.tafrah", "h2_desc"},
    {"tafrahH2Title", "home.tafrah", "h2_title"},
    {"tafrahH3Desc", "home.tafrah", "h3_desc"},
    {"tafrahH3Title", "home.tafrah", "h3_title"},
    {"tafrahH4Desc", "home.tafrah", "h4_desc"},
    {"tafrahH4Title", "home.tafrah", "h4_title"},
    {"tafrahKicker", "home.tafrah", "kicker"},
    {"tafrahTagline", "home.tafrah", "tagline"},
    {"tafrahTitle", "home.tafrah", "title"},
    {"tafrahVisible", "home.tafrah", "visible"},
    // footerAbout is the two address lines joined by a newline; split below.
    {"footerAbout", "home.footer", "address_line1"},
    {"joinNote", "home.join", "note_open"},
    {"heroStat1", "home.hero", "stat1_num"},
    {"heroStat2", "home.hero", "stat2_num"},
    {"heroStat3", "home.hero", "stat3_num"},
}};

[[nodiscard]] std::string_view trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0) { text.remove_prefix(1); }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) { text.remove_suffix(1); }
    return text;
}

[[nodiscard]] std::string lower(std::string_view text) {
    std::string out{text};
    for (char& c : out) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    return out;
}

// At most `max` code points, never splitting a UTF-8 sequence.
[[nodiscard]] std::string clamp_code_points(std::string_view text, std::size_t max) {
    std::size_t points = 0;
    std::size_t i = 0;
    while (i < text.size()) {
        const auto lead = static_cast<unsigned char>(text[i]);
        std::size_t width = 1;
        if (lead >= 0xF0) { width = 4; } else if (lead >= 0xE0) { width = 3; } else if (lead >= 0xC0) { width = 2; }
        if (points == max || i + width > text.size()) { break; }
        i += width;
        ++points;
    }
    return std::string{text.substr(0, i)};
}

[[nodiscard]] std::string text_field(const bsoncxx::document::view& doc, std::string_view key) {
    const auto element = doc[key];
    if (!element) { return {}; }
    switch (element.type()) {
        case bsoncxx::type::k_string: return std::string{element.get_string().value};
        case bsoncxx::type::k_int32: return std::to_string(element.get_int32().value);
        case bsoncxx::type::k_int64: return std::to_string(element.get_int64().value);
        case bsoncxx::type::k_double: {
            const double value = element.get_double().value;
            const auto whole = static_cast<std::int64_t>(value);
            return static_cast<double>(whole) == value ? std::to_string(whole) : std::to_string(value);
        }
        case bsoncxx::type::k_bool: return element.get_bool().value ? "true" : "false";
        default: return {};
    }
}

[[nodiscard]] std::optional<bool> bool_field(const bsoncxx::document::view& doc, std::string_view key) {
    const auto element = doc[key];
    if (!element) { return std::nullopt; }
    if (element.type() == bsoncxx::type::k_bool) { return element.get_bool().value; }
    if (element.type() == bsoncxx::type::k_string) {
        const auto raw = element.get_string().value;
        const std::string value = lower(trim(std::string_view{raw.data(), raw.size()}));
        if (value == "true") { return true; }
        if (value == "false") { return false; }
    }
    return std::nullopt;
}

[[nodiscard]] std::vector<std::string> string_array(const bsoncxx::document::view& doc, std::string_view key) {
    std::vector<std::string> out;
    const auto element = doc[key];
    if (!element || element.type() != bsoncxx::type::k_array) { return out; }
    for (const auto& item : element.get_array().value) {
        if (item.type() == bsoncxx::type::k_string) {
            const auto raw = item.get_string().value;
            out.emplace_back(trim(std::string_view{raw.data(), raw.size()}));
        }
    }
    return out;
}

[[nodiscard]] sec::SectionValue text_value(std::string_view text) {
    sec::SectionValue value{};
    value.primary() = std::string{text};
    return value;
}

[[nodiscard]] std::string entry_text(const ent::EntryDocument& entry, std::string_view field) {
    const ent::EntryContent* copy = entry.working();
    if (copy == nullptr) { return {}; }
    const sec::SectionField* found = copy->content.find(field);
    return found != nullptr ? found->value.primary() : std::string{};
}

[[nodiscard]] const ent::KindSpec& kind(std::string_view key) { return *ent::find_kind(kKinds, key); }

[[nodiscard]] std::size_t field_max(const ent::KindSpec& spec, std::string_view key) {
    for (const sec::FieldSpec& field : spec.shape.fields) {
        if (field.key == key) { return field.max_cp; }
    }
    return 0;
}

mongocxx::collection legacy(mongocxx::client& client, std::string_view name) {
    return client[services().database][std::string{name}];
}

// Every entry of a kind (or of one parent), all pages.
[[nodiscard]] std::vector<ent::EntryDocument> all_entries(mongocxx::client& client, const ent::KindSpec& spec,
                                                          std::optional<Uuid> parent = std::nullopt) {
    std::vector<ent::EntryDocument> out;
    ent::EntryQuery query{};
    query.parent = parent;
    query.stage = ent::Stage::Working;
    query.limit = ent::kMaxPage;
    for (int page = 0; page < 10000; ++page) {
        auto listed = services().entries.list(client, spec, query);
        if (!listed) { break; }
        for (const ent::EntryDocument& entry : listed.value().entries) { out.push_back(entry); }
        if (!listed.value().next.has_value()) { break; }
        query.after = listed.value().next;
    }
    return out;
}

[[nodiscard]] std::optional<Uuid> register_image(mongocxx::client& client, const Options& options,
                                                 std::string_view url, Report& report) {
    const auto local = local_file_for(url, options.doc_root, options.uploads_dir);
    if (!local.has_value()) {
        ++report.images_missing;
        report.notes.push_back("image not importable (remote or malformed URL): " + std::string{url});
        return std::nullopt;
    }
    const auto id = sec::register_default_image(client, services().media, site_namespace(), local->directory,
                                                local->file, Uuid{});
    if (!id.has_value()) {
        ++report.images_missing;
        report.notes.push_back("image missing or unreadable: " + local->directory + "/" + local->file);
    }
    return id;
}

// --- users -----------------------------------------------------------------

void import_users(mongocxx::client& client, Report& report) {
    Services& s = services();
    for (const bsoncxx::document::view doc : legacy(client, kDatabaseCollectionUsers).find({})) {
        const std::string raw_email{trim(text_field(doc, "email"))};
        std::string email;
        if (!anvil::input::is_ok(anvil::accounts::canonicalise(anvil::identity::LoginIdentity::Email, raw_email, email))) {
            ++report.users_refused;
            report.notes.push_back("user with an invalid email skipped");
            continue;
        }
        const std::string stored = text_field(doc, "password");
        std::string record;
        if (stored.starts_with("$argon2id$")) {
            auto wrapped = s.prehash.wrap_legacy(stored);
            if (!wrapped) {
                ++report.users_refused;
                report.notes.push_back("user " + email + ": password record is not a wrappable argon2id hash");
                continue;
            }
            record = std::move(wrapped).value();
        } else if (!stored.empty()) {
            // A row the old in-place migration never reached: still plaintext.
            // Hashed once, here, and the plaintext is never written anywhere.
            record = s.prehash.enroll_plaintext(
                stored, s.prehash.derive_salt(static_cast<std::uint8_t>(anvil::identity::LoginIdentity::Email), email));
        } else {
            ++report.users_refused;
            report.notes.push_back("user " + email + ": no password record");
            continue;
        }

        const std::string legacy_role = text_field(doc, "role");
        const bool superadmin = is_superadmin(legacy_role, email);
        const anvil::PermSet granted = permissions_from(string_array(doc, "permissions")) & kGrantable;
        const Uuid id = anvil::uuid::generate_v7();
        const anvil::Status inserted = s.accounts_repo.insert(
            client, anvil::identity::NewUser{.id = id,
                                             .email_normalised = email,
                                             .email_display = email,
                                             .username_normalised = {},
                                             .username_display = {},
                                             .password_hash = record,
                                             .phone_e164 = {},
                                             .locale = anvil::Locale{},
                                             .status = anvil::UserStatus::Active});
        if (!inserted) {
            if (inserted.error().code == ErrorCode::Conflict) {
                ++report.users_skipped;
            } else {
                ++report.users_refused;
                report.notes.push_back("user " + email + ": insert failed");
            }
            continue;
        }
        const auto typed = s.staff.set_user_type(client, id, anvil::repo::kInitialVersion,
                                                 superadmin ? anvil::UserType::SuperAdmin : anvil::UserType::Staff,
                                                 with_implied(granted), {}, anvil::identity::RoleTable{},
                                                 anvil::UserType::SuperAdmin);
        if (!typed) {
            report.notes.push_back("user " + email + ": imported, but setting the account type failed");
        }
        (void)StaffProfiles::put(client, id, StaffProfile{role_from(legacy_role), std::string{trim(text_field(doc, "team"))}});
        ++report.users_imported;
    }
}

// --- teams -----------------------------------------------------------------

[[nodiscard]] bool listed(const std::vector<std::string>& names, std::string_view name) {
    return std::any_of(names.begin(), names.end(), [name](const std::string& n) { return same_team_name(n, name); });
}

void import_teams(mongocxx::client& client, const bsoncxx::stdx::optional<bsoncxx::document::value>& content,
                  Report& report) {
    struct LegacyTeam final {
        std::string name;
        std::string desc;
        std::vector<std::pair<std::string, std::string>> members;  // name, role
    };
    std::vector<LegacyTeam> teams;
    for (const bsoncxx::document::view doc : legacy(client, kDatabaseCollectionTeams).find({})) {
        const std::string name{trim(text_field(doc, "name"))};
        if (name.empty()) { continue; }
        LegacyTeam* team = nullptr;
        for (LegacyTeam& seen : teams) {
            if (same_team_name(seen.name, name)) { team = &seen; }
        }
        if (team == nullptr) {
            teams.push_back(LegacyTeam{name, {}, {}});
            team = &teams.back();
        }
        if (team->desc.empty()) { team->desc = std::string{trim(text_field(doc, "desc"))}; }
        const auto members = doc["memberList"];
        if (members && members.type() == bsoncxx::type::k_array) {
            for (const auto& member : members.get_array().value) {
                if (member.type() != bsoncxx::type::k_document) { continue; }
                const std::string person{trim(text_field(member.get_document().value, "name"))};
                std::string role{trim(text_field(member.get_document().value, "role"))};
                if (person.empty()) { continue; }
                if (role.empty()) { role = "Member"; }
                team->members.emplace_back(person, role);
            }
        }
    }

    std::vector<std::string> recruiting;
    std::vector<std::string> inside;
    bool have_recruiting = false;
    bool have_inside = false;
    if (content.has_value()) {
        const bsoncxx::document::view view = content->view();
        if (view["recruitmentTeams"] && view["recruitmentTeams"].type() == bsoncxx::type::k_array) {
            have_recruiting = true;
            recruiting = string_array(view, "recruitmentTeams");
        }
        const auto inside_teams = view["insideTeams"];
        if (inside_teams && inside_teams.type() == bsoncxx::type::k_array) {
            have_inside = true;
            for (const auto& item : inside_teams.get_array().value) {
                if (item.type() != bsoncxx::type::k_document) { continue; }
                const std::string name{trim(text_field(item.get_document().value, "name"))};
                if (name.empty()) { continue; }
                inside.push_back(name);
                const std::string desc{trim(text_field(item.get_document().value, "desc"))};
                bool known = false;
                for (LegacyTeam& team : teams) {
                    if (same_team_name(team.name, name)) {
                        known = true;
                        if (!desc.empty()) { team.desc = desc; }
                    }
                }
                if (!known) { teams.push_back(LegacyTeam{name, desc, {}}); }
            }
        }
    }
    if (teams.empty()) { return; }

    const ent::KindSpec& team_kind = kind(kTeamKind);
    const ent::KindSpec& member_kind = kind(kMemberKind);
    const ent::FlagSet recruiting_bit = ent::flag_bit(team_kind, "recruiting");
    const ent::FlagSet showcase_bit = ent::flag_bit(team_kind, "showcase");
    const std::size_t name_max = field_max(team_kind, "name");
    const std::size_t desc_max = field_max(team_kind, "desc");

    for (const LegacyTeam& team : teams) {
        const std::string name = clamp_code_points(team.name, name_max);
        const std::string desc = clamp_code_points(team.desc, desc_max);
        ent::FlagSet flags = 0;
        if (!have_recruiting || listed(recruiting, name)) { flags |= recruiting_bit; }
        if (!have_inside || listed(inside, name)) { flags |= showcase_bit; }

        std::optional<Uuid> id;
        const auto existing = find_team_by_name(client, name);
        if (existing && existing.value().has_value()) {
            id = existing.value()->id;
            if (!desc.empty() && desc != existing.value()->desc) {
                ent::EntryEdit edit{};
                edit.patch.set("desc", text_value(desc));
                (void)services().entries.write(client, team_kind, *id, existing.value()->version, edit, Uuid{});
            }
            (void)services().entries.set_flags(client, team_kind, *id, flags,
                                               static_cast<ent::FlagSet>((recruiting_bit | showcase_bit) & ~flags));
            ++report.teams_merged;
        } else {
            ent::NewEntry entry{};
            entry.content.set("name", text_value(name));
            entry.content.set("desc", text_value(desc));
            entry.flags = flags;
            std::string base = slugify(name);
            if (base.empty()) { base = "team"; }
            for (int attempt = 0; attempt < 20 && !id.has_value(); ++attempt) {
                entry.slug = attempt == 0 ? base : base + "-" + std::to_string(attempt + 1);
                const auto created = services().entries.create(client, team_kind, entry, Uuid{});
                if (created) {
                    id = created.value().id;
                } else if (created.error().code != ErrorCode::Conflict) {
                    break;
                }
            }
            if (!id.has_value()) {
                report.notes.push_back("team " + name + ": could not be created");
                continue;
            }
            ++report.teams_created;
        }

        std::vector<std::string> present;
        for (const ent::EntryDocument& member : all_entries(client, member_kind, *id)) {
            present.push_back(entry_text(member, "name"));
        }
        for (const auto& [person, role] : team.members) {
            if (listed(present, person)) { continue; }
            ent::NewEntry member{};
            member.parent = *id;
            member.content.set("name", text_value(clamp_code_points(person, field_max(member_kind, "name"))));
            member.content.set("role", text_value(clamp_code_points(role, field_max(member_kind, "role"))));
            if (services().entries.create(client, member_kind, member, Uuid{})) {
                present.push_back(person);
                ++report.members_imported;
            } else {
                report.notes.push_back("member " + person + " of " + name + ": could not be created");
            }
        }
    }

    // The seeded teams the old site never had: removed while still empty, so
    // the new site lists exactly the teams the old one did.
    std::vector<std::string> wanted;
    for (const LegacyTeam& team : teams) { wanted.push_back(team.name); }
    for (const ent::EntryDocument& entry : all_entries(client, team_kind)) {
        if (entry.children == 0 && !listed(wanted, entry_text(entry, "name"))) {
            (void)services().entries.remove(client, team_kind, entry.id, entry.version);
        }
    }
}

// --- applications ----------------------------------------------------------

void import_applications(mongocxx::client& client, Report& report) {
    const ent::KindSpec& spec = kind(kApplicationKind);
    mongocxx::options::find oldest_first;
    oldest_first.sort(make_document(kvp("_id", 1)));
    for (const bsoncxx::document::view doc : legacy(client, kDatabaseCollectionApplications).find({}, oldest_first)) {
        std::string email;
        if (!anvil::input::is_ok(anvil::accounts::canonicalise(anvil::identity::LoginIdentity::Email,
                                                               trim(text_field(doc, "email")), email))) {
            ++report.applications_skipped;
            report.notes.push_back("application with an invalid email skipped");
            continue;
        }
        const std::string slug = application_slug(email);
        const auto existing = services().entries.find_by_slug(client, spec, slug, ent::Stage::Working);
        if (existing && existing.value().has_value()) {
            ++report.applications_skipped;
            continue;
        }
        const auto clamp = [&spec](std::string_view field, std::string_view value) {
            return text_value(clamp_code_points(trim(value), field_max(spec, field)));
        };
        PersonName person = split_name(trim(text_field(doc, "name")));
        if (person.first.empty()) { person.first = std::string{trim(text_field(doc, "firstName"))}; }
        if (person.first.empty()) { person.first = "-"; }

        std::string team{trim(text_field(doc, "team"))};
        if (const auto named = find_team_by_name(client, team); named && named.value().has_value()) {
            team = named.value()->name;
        }
        if (team.empty()) { team = "-"; }
        std::string referral{trim(text_field(doc, "referredTo"))};
        if (const auto named = find_team_by_name(client, referral); named && named.value().has_value()) {
            referral = named.value()->name;
        }
        std::string status = status_from(text_field(doc, "status"));
        if (status == "referred" && referral.empty()) { status = "pending"; }
        std::string phone{trim(text_field(doc, "phone"))};
        if (phone.empty()) { phone = "-"; }

        ent::NewEntry entry{};
        entry.slug = slug;
        entry.content.set("email", text_value(email));
        entry.content.set("first_name", clamp("first_name", person.first));
        entry.content.set("last_name", clamp("last_name", person.last));
        entry.content.set("phone", clamp("phone", phone));
        entry.content.set("reason", clamp("reason", text_field(doc, "reason")));
        entry.content.set("status", text_value(status));
        entry.content.set("team", clamp("team", team));
        entry.content.set("referred_to", clamp("referred_to", referral));
        entry.content.set("decision", clamp("decision", text_field(doc, "decision")));
        if (services().entries.create(client, spec, entry, Uuid{})) {
            ++report.applications_imported;
        } else {
            ++report.applications_skipped;
            report.notes.push_back("application from " + email + ": refused by the application kind's checks");
        }
    }
}

// --- content ---------------------------------------------------------------

// Writes `patch` over the published section, dropping one refused field at a
// time: a legacy value that no longer passes (a link that is not a URL, an
// over-long line) keeps its default rather than costing the whole section.
void write_section(mongocxx::client& client, const sec::SectionSpec& spec, sec::SectionContent patch,
                   Report& report) {
    for (int attempt = 0; attempt < 64 && (!patch.fields.empty() || !patch.images.empty()); ++attempt) {
        const auto document = services().sections.read_document(client, spec, sec::SectionState::Published);
        if (!document || !document.value().has_value()) {
            report.notes.push_back("section " + std::string{spec.key} + " is missing; run migrate before --legacy");
            return;
        }
        const auto written = services().sections.write(client, spec, sec::SectionState::Published,
                                                       document.value()->version, patch, Uuid{});
        if (written) {
            report.section_fields += patch.fields.size() + patch.images.size();
            return;
        }
        const std::string_view refused = written.error().field;
        const auto before = patch.fields.size() + patch.images.size();
        std::erase_if(patch.fields, [refused](const sec::SectionField& f) { return f.key == refused; });
        std::erase_if(patch.images, [refused](const sec::SectionImage& i) { return i.slot == refused; });
        if (patch.fields.size() + patch.images.size() == before) {
            report.notes.push_back("section " + std::string{spec.key} + ": refused as a whole");
            report.section_fields_refused += before;
            return;
        }
        ++report.section_fields_refused;
        report.notes.push_back("section " + std::string{spec.key} + ": kept the default for " + std::string{refused});
    }
}

// Every legacy value goes through anvil's own section binder (the one the
// admin route uses), so a value the editor would refuse — a link that is not an
// http(s) URL, a control character, an over-long line — is refused here too
// and keeps its default, one field at a time.
[[nodiscard]] sec::SectionContent bind_legacy(const sec::SectionSpec& spec,
                                              std::vector<std::pair<std::string, std::string>> texts,
                                              std::vector<std::pair<std::string, bool>> flags, Report& report) {
    const sec::BindPolicy policy{services().config.site_origin};
    for (int attempt = 0; attempt < 64; ++attempt) {
        std::string json{"{"};
        bool first = true;
        for (const auto& [key, value] : texts) {
            if (!first) { json += ','; }
            first = false;
            anvil::http::append_json_string(json, key);
            json += ':';
            anvil::http::append_json_string(json, value);
        }
        for (const auto& [key, value] : flags) {
            if (!first) { json += ','; }
            first = false;
            anvil::http::append_json_string(json, key);
            json += value ? ":true" : ":false";
        }
        json += '}';
        anvil::input::BodyArena arena;
        const anvil::input::JsonDocument document = anvil::input::parse_json(json, arena);
        sec::SectionContent bound{};
        if (!document.ok()) {
            report.notes.push_back("section " + std::string{spec.key} + ": legacy values do not parse");
            return {};
        }
        const auto bad = sec::bind_data(spec, &document.root(), policy, bound);
        if (!bad.has_value()) { return bound; }
        const std::string refused{bad->field};
        const auto before = texts.size() + flags.size();
        std::erase_if(texts, [&refused](const auto& kv) { return kv.first == refused; });
        std::erase_if(flags, [&refused](const auto& kv) { return kv.first == refused; });
        if (refused.empty() || texts.size() + flags.size() == before) {
            report.notes.push_back("section " + std::string{spec.key} + ": legacy values refused as a whole");
            report.section_fields_refused += before;
            return {};
        }
        ++report.section_fields_refused;
        report.notes.push_back("section " + std::string{spec.key} + ": kept the default for " + refused);
    }
    return {};
}

void import_sections(mongocxx::client& client, const bsoncxx::document::view& content, const Options& options,
                     Report& report) {
    for (const sec::SectionSpec& spec : kSections) {
        std::vector<std::pair<std::string, std::string>> texts;
        std::vector<std::pair<std::string, bool>> flags;
        const auto taken = [&](std::string_view field) {
            return std::any_of(texts.begin(), texts.end(), [field](const auto& kv) { return kv.first == field; }) ||
                   std::any_of(flags.begin(), flags.end(), [field](const auto& kv) { return kv.first == field; });
        };
        for (const KeyMap& map : kKeyMap) {
            if (map.section != spec.key || !content[map.legacy]) { continue; }
            const sec::FieldSpec* field = nullptr;
            for (const sec::FieldSpec& candidate : spec.fields) {
                if (candidate.key == map.field) { field = &candidate; }
            }
            if (field == nullptr || taken(map.field)) { continue; }
            if (field->type == sec::FieldType::Bool) {
                if (const auto flag = bool_field(content, map.legacy); flag.has_value()) {
                    flags.emplace_back(std::string{map.field}, *flag);
                }
                continue;
            }
            std::string text = text_field(content, map.legacy);
            if (map.legacy == "footerAbout") {
                const auto newline = text.find('\n');
                if (newline != std::string::npos) {
                    if (!taken("address_line2")) {
                        texts.emplace_back("address_line2", clamp_code_points(trim(std::string_view{text}.substr(newline + 1)), 200));
                    }
                    text.resize(newline);
                }
            }
            const std::string_view trimmed = trim(text);
            if (trimmed.empty()) { continue; }
            texts.emplace_back(std::string{map.field}, clamp_code_points(trimmed, field->max_cp));
        }
        sec::SectionContent patch = bind_legacy(spec, std::move(texts), std::move(flags), report);
        if (spec.key == "home.tafrah") {
            const std::string site = text_field(content, "tafrahSiteImage");
            if (!trim(site).empty()) {
                if (const auto id = register_image(client, options, trim(site), report); id.has_value()) {
                    patch.images.push_back(sec::SectionImage{"site", *id});
                }
            }
        }
        write_section(client, spec, std::move(patch), report);
    }
}

void import_galleries(mongocxx::client& client, const bsoncxx::document::view& content, const Options& options,
                      Report& report) {
    constexpr std::array<std::pair<std::string_view, std::string_view>, 3> kGalleries{{
        {"aboutImages", "gallery.about"},
        {"mediaGallery", "gallery.life"},
        {"tafrahImages", "gallery.tafrah"},
    }};
    for (const auto& [legacy_key, kind_key] : kGalleries) {
        const auto array = content[legacy_key];
        if (!array || array.type() != bsoncxx::type::k_array) { continue; }
        std::vector<std::pair<Uuid, std::string>> photos;
        for (const auto& item : array.get_array().value) {
            std::string url;
            std::string caption;
            if (item.type() == bsoncxx::type::k_document) {
                url = text_field(item.get_document().value, "url");
                caption = text_field(item.get_document().value, "alt");
                if (caption.empty()) { caption = text_field(item.get_document().value, "caption"); }
            } else if (item.type() == bsoncxx::type::k_string) {
                url = std::string{item.get_string().value};
            }
            if (trim(url).empty()) { continue; }
            if (const auto id = register_image(client, options, trim(url), report); id.has_value()) {
                photos.emplace_back(*id, caption);
            }
        }
        if (photos.empty()) { continue; }
        const ent::KindSpec& spec = kind(kind_key);
        // The old site's gallery replaces the seeded one, in the old order.
        for (const ent::EntryDocument& entry : all_entries(client, spec)) {
            (void)services().entries.remove(client, spec, entry.id, entry.version);
        }
        for (const auto& [id, caption] : photos) {
            ent::NewEntry entry{};
            entry.content.set("caption", text_value(clamp_code_points(trim(caption), field_max(spec, "caption"))));
            entry.content.images.push_back(sec::SectionImage{"photo", id});
            if (services().entries.create(client, spec, entry, Uuid{})) {
                ++report.gallery_photos;
            } else {
                report.notes.push_back(std::string{kind_key} + ": a photo was refused (too small or a duplicate)");
            }
        }
    }
}

// --- forms -----------------------------------------------------------------

[[nodiscard]] std::optional<anvil::db::TimeMs> parse_iso(std::string_view text) {
    std::tm tm{};
    const std::string copy{text};
    if (::strptime(copy.c_str(), "%Y-%m-%dT%H:%M:%S", &tm) == nullptr) { return std::nullopt; }
    const std::time_t seconds = ::timegm(&tm);
    if (seconds < 0) { return std::nullopt; }
    return anvil::db::TimeMs{std::chrono::seconds{seconds}};
}

void import_form(mongocxx::client& client, Report& report) {
    const auto schema_doc = legacy(client, kDatabaseCollectionFormSchema).find_one({});
    if (!schema_doc) { return; }
    const bsoncxx::document::view schema_view = schema_doc->view();
    const auto fields = schema_view["fields"];
    if (!fields || fields.type() != bsoncxx::type::k_array) { return; }

    std::string title{trim(text_field(schema_view, "title"))};
    if (title.empty()) { title = "Application Form"; }
    title = clamp_code_points(title, fm::kMaxLabelCodePoints);

    const auto existing = services().forms.list(client, std::nullopt, fm::kMaxFormListLimit, anvil::Locale{});
    if (existing) {
        for (const fm::FormListEntry& form : existing.value().entries) {
            if (form.title == title) {
                report.notes.push_back("form \"" + title + "\" already imported");
                return;
            }
        }
    }

    fm::FormSchema schema{};
    schema.title[anvil::config::kDefaultLocale] = title;
    schema.status = fm::FormStatus::Active;
    schema.max_submissions = 0;
    schema.one_per_user = false;
    std::vector<std::string> labels;
    std::size_t index = 0;
    for (const auto& item : fields.get_array().value) {
        if (item.type() != bsoncxx::type::k_document || schema.fields.size() >= fm::kMaxFormFields) { continue; }
        const bsoncxx::document::view field_view = item.get_document().value;
        const std::string label{trim(text_field(field_view, "label"))};
        if (label.empty()) { continue; }
        ++index;
        fm::FieldSpec field{};
        field.fid = *fm::Fid::parse("f" + std::to_string(index));
        field.label[anvil::config::kDefaultLocale] = clamp_code_points(label, fm::kMaxLabelCodePoints);
        const fm::FieldTypeSpec* type = fm::field_type_by_name(kFieldTypes, field_type_from(text_field(field_view, "type")));
        field.type = type->code;
        field.optional = !bool_field(field_view, "required").value_or(false);
        if (type->code == fm::field_type_by_name(kFieldTypes, "SELECT_SINGLE")->code) {
            const std::string options = text_field(field_view, "options");
            std::size_t start = 0;
            std::size_t option_index = 0;
            while (start <= options.size() && field.options.size() < fm::kMaxFieldOptions) {
                const std::size_t comma = options.find(',', start);
                const std::string_view part =
                    trim(std::string_view{options}.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
                if (!part.empty()) {
                    fm::FormOption option{};
                    option.value = option_value(part, ++option_index);
                    option.label[anvil::config::kDefaultLocale] = clamp_code_points(part, fm::kMaxLabelCodePoints);
                    const bool duplicate = std::any_of(field.options.begin(), field.options.end(),
                                                       [&option](const fm::FormOption& o) { return o.value == option.value; });
                    if (!duplicate) { field.options.push_back(std::move(option)); }
                }
                if (comma == std::string::npos) { break; }
                start = comma + 1;
            }
            if (field.options.empty()) {
                field.type = fm::field_type_by_name(kFieldTypes, "TEXT_SHORT")->code;
            }
        }
        labels.push_back(label);
        schema.fields.push_back(std::move(field));
    }
    if (schema.fields.empty()) { return; }
    if (const anvil::Status valid = fm::validate_schema(kFieldTypes, schema); !valid) {
        report.notes.push_back("the legacy form schema does not validate (" + std::string{valid.error().field} +
                               "); not imported");
        return;
    }
    const auto created = services().forms.create(client, schema, Uuid{});
    if (!created) {
        report.notes.push_back("the legacy form could not be created");
        return;
    }
    ++report.forms_imported;
    const auto definition = services().forms.repository().find_definition(client, created.value());
    if (!definition || !definition.value().has_value()) { return; }
    const fm::FormDefinition& form = *definition.value();

    std::int64_t imported = 0;
    mongocxx::options::find oldest_first;
    oldest_first.sort(make_document(kvp("_id", 1)));
    for (const bsoncxx::document::view doc : legacy(client, kDatabaseCollectionFormSubmissions).find({}, oldest_first)) {
        const auto data = doc["data"];
        if (!data || data.type() != bsoncxx::type::k_document) { continue; }
        const bsoncxx::document::view answers_view = data.get_document().value;
        fm::SubmissionRecord record{};
        for (std::size_t i = 0; i < form.fields.size(); ++i) {
            const fm::FieldSpec& field = form.fields[i];
            const std::string value{trim(text_field(answers_view, labels[i]))};
            if (value.empty()) { continue; }
            fm::Answer answer{};
            answer.fid = field.fid;
            if (!field.options.empty()) {
                const auto chosen = std::find_if(field.options.begin(), field.options.end(), [&value](const fm::FormOption& o) {
                    return lower(o.label[anvil::config::kDefaultLocale]) == lower(value);
                });
                if (chosen == field.options.end()) { continue; }
                answer.kind = fm::AnswerKind::Choice;
                answer.text = chosen->value;
            } else {
                answer.kind = fm::AnswerKind::Text;
                answer.text = clamp_code_points(value, field.max_code_points == 0 ? fm::kMaxFieldCodePoints : field.max_code_points);
            }
            record.answers.push_back(std::move(answer));
        }
        if (record.answers.empty()) { continue; }
        record.id = anvil::uuid::generate_v7();
        record.form = form.id;
        record.ip = {};
        record.submitted_at = parse_iso(text_field(doc, "submittedAt")).value_or(anvil::db::now_ms());
        record.form_version = form.version;
        record.has_pii = false;
        record.enforce_one_per_user = false;
        if (services().forms.repository().insert_submission(client, record)) { ++imported; }
    }
    if (imported > 0) {
        const std::array<std::pair<Uuid, std::int64_t>, 1> counts{{{form.id, imported}}};
        (void)services().forms.repository().add_submission_counts(client, counts);
    }
    report.responses_imported += static_cast<std::size_t>(imported);
}

}  // namespace

// --- the pure mappings -------------------------------------------------------

anvil::PermSet permissions_from(const std::vector<std::string>& names) {
    anvil::PermSet out{};
    for (const std::string& name : names) {
        const auto bit = kPerms.bit_for_name(lower(trim(name)));
        if (bit.has_value()) { out = out | anvil::perm_mask(static_cast<Perm>(*bit)); }
    }
    return out & kGrantable;
}

std::string role_from(std::string_view legacy_role) {
    const std::string role = lower(trim(legacy_role));
    if (role == "superadmin" || role == "super admin" || role == "admin") { return "high board"; }
    if (role == "vice_manager" || role == "vice-manager" || role == "vice manager") { return "vice manager"; }
    if (role == "hr") { return "HR"; }
    if (role == "high_board" || role == "highboard") { return "high board"; }
    for (const std::string_view known : kStaffRoles) {
        if (lower(known) == role) { return std::string{known}; }
    }
    return "member";
}

bool is_superadmin(std::string_view role, std::string_view email) {
    return lower(trim(role)) == "superadmin" || lower(trim(email)) == kLegacySuperAdminEmail;
}

PersonName split_name(std::string_view name) {
    name = trim(name);
    const std::size_t space = name.find_first_of(" \t");
    if (space == std::string_view::npos) { return PersonName{std::string{name}, "-"}; }
    const std::string_view rest = trim(name.substr(space));
    return PersonName{std::string{name.substr(0, space)}, rest.empty() ? "-" : std::string{rest}};
}

std::string status_from(std::string_view legacy_status) {
    std::string status = lower(trim(legacy_status));
    std::replace(status.begin(), status.end(), ' ', '_');
    return sec::is_choice(kApplicationStatuses, status) ? status : "pending";
}

std::string option_value(std::string_view label, std::size_t index) {
    std::string out;
    bool dash = false;
    for (const char raw : label) {
        const auto c = static_cast<unsigned char>(raw);
        if (c < 0x80 && std::isalnum(c) != 0) {
            if (dash && !out.empty()) { out += '_'; }
            dash = false;
            out += static_cast<char>(std::tolower(c));
        } else {
            dash = true;
        }
        if (out.size() >= 60) { break; }
    }
    if (out.empty()) { out = "opt" + std::to_string(index); }
    return out;
}

std::string_view field_type_from(std::string_view legacy_type) {
    const std::string type = lower(trim(legacy_type));
    if (type == "textarea") { return "TEXT_LONG"; }
    if (type == "email") { return "EMAIL"; }
    if (type == "tel" || type == "phone") { return "PHONE"; }
    if (type == "select") { return "SELECT_SINGLE"; }
    return "TEXT_SHORT";
}

std::optional<SectionTarget> section_target(std::string_view legacy_key) {
    for (const KeyMap& map : kKeyMap) {
        if (map.legacy == legacy_key) { return SectionTarget{map.section, map.field}; }
    }
    return std::nullopt;
}

std::optional<LocalFile> local_file_for(std::string_view url, std::string_view doc_root, std::string_view uploads_dir) {
    url = trim(url);
    // An absolute URL is accepted only for its path, and only when that path is
    // one the old backend served from disk.
    if (url.starts_with("https://") || url.starts_with("http://")) {
        const std::size_t host = url.find("://") + 3;
        const std::size_t path = url.find('/', host);
        if (path == std::string_view::npos) { return std::nullopt; }
        url = url.substr(path);
    }
    if (const std::size_t cut = url.find_first_of("?#"); cut != std::string_view::npos) { url = url.substr(0, cut); }
    // The old site stored its bundled images document-relative ("assets/x.jpg",
    // sometimes "./assets/x.jpg"): the same files as "/assets/x.jpg". Only the
    // two roots below are rooted this way; anything else relative is refused.
    // `rooted` outlives every view taken into it below.
    std::string rooted;
    if (url.starts_with("./")) { url.remove_prefix(2); }
    if (url.starts_with("assets/") || url.starts_with("uploads/")) {
        rooted = "/" + std::string{url};
        url = rooted;
    }
    if (!url.empty() && url.front() != '/') { return std::nullopt; }
    std::vector<std::string_view> segments;
    std::size_t start = 1;
    while (start <= url.size()) {
        const std::size_t slash = url.find('/', start);
        const std::string_view segment = url.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
        if (segment.empty() || segment.front() == '.' || segment.size() > 128) { return std::nullopt; }
        for (const char c : segment) {
            const auto u = static_cast<unsigned char>(c);
            if (!(std::isalnum(u) != 0 || c == '-' || c == '_' || c == '.') || u >= 0x80) { return std::nullopt; }
        }
        segments.push_back(segment);
        if (slash == std::string_view::npos) { break; }
        start = slash + 1;
    }
    if (segments.size() < 2) { return std::nullopt; }
    std::string directory;
    if (segments.front() == "assets") {
        directory = std::string{doc_root} + "/assets";
    } else if (segments.front() == "uploads") {
        directory = std::string{uploads_dir};
    } else {
        return std::nullopt;
    }
    for (std::size_t i = 1; i + 1 < segments.size(); ++i) {
        directory += '/';
        directory += segments[i];
    }
    return LocalFile{std::move(directory), std::string{segments.back()}};
}

Report import_all(mongocxx::client& client, const Options& options) {
    Report report{};
    import_users(client, report);
    const auto content = legacy(client, kDatabaseCollectionContent).find_one({});
    import_teams(client, content, report);
    import_applications(client, report);
    if (content) {
        import_sections(client, content->view(), options, report);
        import_galleries(client, content->view(), options, report);
    }
    import_form(client, report);
    return report;
}

}  // namespace enactus::legacy
