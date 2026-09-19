#include "anvil/notifications/template_spec.h"

#include <array>
#include <cstdint>

#include "anvil/core/types.h"
#include "anvil/i18n/bidi.h"
#include "anvil/i18n/utf8.h"

namespace anvil::notifications {
namespace {

constexpr std::string_view kParamsField = "params";

// The longest an int64 renders to, plus its sign. A stack array rather than
// std::to_string, so a rendered number costs no allocation.
constexpr std::size_t kMaxNumberChars = 20;

void append_number(std::string& out, std::int64_t value) {
    std::array<char, kMaxNumberChars> digits{};
    std::size_t length = 0;
    // Negated into an UNSIGNED accumulator: -(-2^63) overflows in int64, and
    // signed overflow is undefined behaviour (ENGINEERING_RULES.md §5).
    const bool negative = value < 0;
    auto magnitude = negative ? (~static_cast<std::uint64_t>(value) + 1U)
                              : static_cast<std::uint64_t>(value);
    do {
        digits[length] = static_cast<char>('0' + (magnitude % 10U));
        ++length;
        magnitude /= 10U;
    } while (magnitude != 0U && length < digits.size());

    if (negative) { out.push_back('-'); }
    for (std::size_t i = length; i > 0; --i) { out.push_back(digits[i - 1]); }
}

// Linear over at most eight entries. A map lookup would hash a single character
// to search a set that fits inside one cache line.
[[nodiscard]] const Param* find_param(std::span<const Param> params, char name) noexcept {
    for (const Param& param : params) {
        if (param.name == name) { return &param; }
    }
    return nullptr;
}

// One pass, appending into a reserved buffer: no allocation per placeholder and
// no per-request template parsing.
void substitute(std::string_view text, std::span<const Param> params, std::string& out) {
    out.reserve(out.size() + text.size() + (params.size() * 24));
    for (std::size_t i = 0; i < text.size(); ++i) {
        const bool placeholder = text[i] == '{' && i + 2 < text.size() &&
                                 detail::is_ascii_letter(text[i + 1]) && text[i + 2] == '}';
        if (!placeholder) {
            out.push_back(text[i]);
            continue;
        }
        const Param* param = find_param(params, text[i + 1]);
        i += 2;
        if (param == nullptr) {
            // A row whose params were written by an older build. Rendering
            // nothing gives a shorter sentence; rendering "{t}" would put the
            // template's internals into a reader's inbox.
            continue;
        }
        if (param->type == ParamType::Number) {
            append_number(out, param->number);
        } else {
            out.append(param->text);
        }
    }
}

}  // namespace

Status check_param(const Param& param) noexcept {
    if (param.type == ParamType::Number) { return ok(); }

    // A stored parameter is data from an earlier request, and a row written by an
    // older build may predate a rule. Validating here as well as at publish is
    // the same "never trust stored text" position anvil/db/codec.h takes.
    if (i18n::validate(param.text) != i18n::Utf8Error::Ok) {
        return fail(ErrorCode::ValidationFailed, kParamsField);
    }
    // Code points, never bytes: Arabic is two bytes per character, so a byte
    // limit silently halves the allowance for any non-Latin script.
    if (!i18n::within_code_point_bounds(param.text, 0, kMaxParamCodePoints)) {
        return fail(ErrorCode::ValidationFailed, kParamsField);
    }
    // Prose rather than Identifier: a title legitimately mixes scripts and needs
    // isolates to render correctly. An OVERRIDE is still refused — U+202E in a
    // notification body spoofs the whole sentence, and the reader has no way to
    // see that it did.
    if (!i18n::is_acceptable(param.text, i18n::TextClass::Prose)) {
        return fail(ErrorCode::ValidationFailed, kParamsField);
    }
    return ok();
}

Result<Rendered> render(std::span<const TemplateSpec> table, TemplateId id, Locale locale,
                        std::span<const Param> params) {
    const TemplateSpec* spec = template_of(table, id);
    // Internal rather than ValidationFailed: the id came off a stored row this
    // system wrote, so a build that cannot resolve it has an integrity problem
    // rather than bad input.
    if (spec == nullptr) { return fail(ErrorCode::Internal, "tpl"); }
    if (params.size() > kMaxParams) { return fail(ErrorCode::ValidationFailed, kParamsField); }

    for (const Param& param : params) {
        if (const Status usable = check_param(param); !usable) { return usable.error(); }
    }

    Rendered out{};
    substitute(spec->title.get(locale), params, out.title);
    substitute(spec->body.get(locale), params, out.body);
    return out;
}

}  // namespace anvil::notifications
