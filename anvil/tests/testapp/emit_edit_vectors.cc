// Prints the edit recipe's golden vectors as the JSON fixture the client commits.
//
// The vectors themselves are tests/testapp/edit_vectors.h; this program adds
// what anvil's decoder READ from each accepted one, so the client's decoder is
// checked field by field and not only by whether it re-encodes. A vector the
// decoder here refuses carries its fault and nothing else.
//
//     build/release/tests/testapp_emit_edit_vectors > <hammer>/tests/edit/recipe_vectors.json

#include <cstdio>
#include <string>
#include <vector>

#include "anvil/crypto/base64url.h"
#include "anvil/http/json_writer.h"
#include "anvil/images/recipe.h"

#include "edit_vectors.h"

namespace {

namespace images = anvil::images;
namespace json = anvil::http;

void append_uint_array(std::string& out, const std::uint16_t* values, std::size_t count) {
    out += '[';
    for (std::size_t i = 0; i < count; ++i) {
        if (i > 0) { out += ','; }
        json::append_json_int(out, values[i]);
    }
    out += ']';
}

void append_recipe(std::string& out, const images::Recipe& recipe) {
    out += '{';
    json::append_json_key(out, "turns");
    json::append_json_int(out, recipe.turns);
    out += ',';
    json::append_json_key(out, "flip");
    out += recipe.flip ? "true" : "false";
    out += ',';
    json::append_json_key(out, "crop");
    if (recipe.crop.has_value()) {
        const std::uint16_t rect[4] = {recipe.crop->x, recipe.crop->y, recipe.crop->w,
                                       recipe.crop->h};
        append_uint_array(out, rect, 4);
    } else {
        out += "null";
    }
    out += ',';
    json::append_json_key(out, "long_edge_px");
    if (recipe.long_edge_px.has_value()) {
        json::append_json_int(out, *recipe.long_edge_px);
    } else {
        out += "null";
    }
    out += ',';
    json::append_json_key(out, "strokes");
    out += '[';
    for (std::size_t s = 0; s < recipe.strokes.size(); ++s) {
        const images::RecipeStroke& stroke = recipe.strokes[s];
        if (s > 0) { out += ','; }
        out += '{';
        json::append_json_key(out, "rgba");
        const std::uint16_t rgba[4] = {stroke.rgba[0], stroke.rgba[1], stroke.rgba[2],
                                       stroke.rgba[3]};
        append_uint_array(out, rgba, 4);
        out += ',';
        json::append_json_key(out, "width");
        json::append_json_int(out, stroke.width);
        out += ',';
        json::append_json_key(out, "points");
        append_uint_array(out, recipe.points.data() + static_cast<std::size_t>(stroke.first_point) * 2,
                          static_cast<std::size_t>(stroke.point_count) * 2);
        out += '}';
    }
    out += "]}";
}

}  // namespace

int main() {
    std::string out;
    out += "{";
    json::append_json_key(out, "limits");
    out += '{';
    json::append_json_key(out, "max_strokes");
    json::append_json_int(out, testapp::kEditVectorLimits.max_strokes);
    out += ',';
    json::append_json_key(out, "max_points");
    json::append_json_int(out, testapp::kEditVectorLimits.max_points);
    out += ',';
    json::append_json_key(out, "max_edge_px");
    json::append_json_int(out, testapp::kEditVectorLimits.max_edge_px);
    out += ',';
    json::append_json_key(out, "min_edge_px");
    json::append_json_int(out, testapp::kEditVectorLimits.min_edge_px);
    out += "},";
    json::append_json_key(out, "vectors");
    out += "[\n";
    bool first = true;
    for (const testapp::EditVector& vector : testapp::kEditVectors) {
        if (!first) { out += ",\n"; }
        first = false;
        out += '{';
        json::append_json_key(out, "name");
        json::append_json_string(out, vector.name);
        out += ',';
        json::append_json_key(out, "recipe");
        json::append_json_string(out, vector.recipe);
        out += ',';
        json::append_json_key(out, "source");
        out += '[';
        json::append_json_int(out, vector.source_width);
        out += ',';
        json::append_json_int(out, vector.source_height);
        out += "],";
        json::append_json_key(out, "fault");
        if (vector.fault.empty()) {
            out += "null";
        } else {
            json::append_json_string(out, vector.fault);
        }
        out += ',';
        json::append_json_key(out, "out");
        if (vector.fault.empty()) {
            out += '[';
            json::append_json_int(out, vector.out_width);
            out += ',';
            json::append_json_int(out, vector.out_height);
            out += ']';
        } else {
            out += "null";
        }
        out += ',';
        json::append_json_key(out, "decoded");
        const auto bytes = anvil::crypto::base64url_decode(vector.recipe);
        const anvil::Result<images::Recipe> decoded =
            bytes.has_value()
                ? images::decode_recipe(*bytes, testapp::kEditVectorLimits)
                : anvil::Result<images::Recipe>{anvil::fail(anvil::ErrorCode::ValidationFailed)};
        if (decoded.ok()) {
            append_recipe(out, decoded.value());
        } else {
            out += "null";
        }
        out += '}';
    }
    out += "\n]}\n";
    std::fwrite(out.data(), 1, out.size(), stdout);
    return 0;
}
