// Renders a fixed set of edit recipes over a generated source, as the fixture
// hammer's preview is compared against pixel by pixel.
//
// The recipe vectors (edit_vectors.h) prove the two codecs agree on BYTES and
// on the planned size. They cannot prove the two renderers agree on the
// picture: hammer previews an edit as an SVG the browser paints, anvil renders
// it with libvips and its own stroke rasteriser, and "the preview is the render
// up to edge anti-aliasing" is a claim about pixels (docs/21-image-edits.md
// §4.2). hammer's parity run screenshots its real preview at each output size
// and compares it with the files this program writes.
//
//     build/release/tests/testapp_emit_edit_renders <hammer>/tests/edit/parity
//
// It writes `source.png`, one PNG per case and `manifest.json`. Every output is
// the derived object's MASTER, which is a PNG for a PNG source: lossless, so
// the comparison measures the renderer and not a codec.
//
// The source is a gradient whose red channel runs with x and whose green runs
// with y, so a turn or a flip the two sides disagree about is a whole-picture
// difference and not a subtle one.

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include <vips/vips.h>

#include "anvil/core/uuid.h"
#include "anvil/crypto/base64url.h"
#include "anvil/fs/paths.h"
#include "anvil/http/json_writer.h"
#include "anvil/images/edit.h"
#include "anvil/images/probe.h"
#include "anvil/images/recipe.h"

#include "namespaces.h"

namespace {

namespace images = anvil::images;
namespace fs = anvil::fs;
namespace json = anvil::http;

constexpr int kSourceWidth = 640;
constexpr int kSourceHeight = 480;

struct Case final {
    std::string_view name;
    std::string_view file;
    images::Recipe   recipe;
};

// One stroke, appended to a recipe: rgba, width, then x y pairs.
void add_stroke(images::Recipe& recipe, std::array<std::uint8_t, 4> rgba, std::uint16_t width,
                std::initializer_list<std::uint16_t> points) {
    const auto first = static_cast<std::uint32_t>(recipe.points.size() / 2);
    recipe.points.insert(recipe.points.end(), points.begin(), points.end());
    recipe.strokes.push_back(images::RecipeStroke{
        rgba, width, static_cast<std::uint16_t>(points.size() / 2), first});
}

[[nodiscard]] std::vector<Case> cases() {
    std::vector<Case> out;

    images::Recipe turned_flipped{};
    turned_flipped.turns = 1;
    turned_flipped.flip = true;
    out.push_back({"a quarter turn and a flip", "turn-flip.png", turned_flipped});

    images::Recipe cropped{};
    cropped.crop = images::FixedRect{16384, 16384, 32768, 32768};
    out.push_back({"a centred crop", "crop.png", cropped});

    images::Recipe resized{};
    resized.long_edge_px = 400;
    out.push_back({"a resize of the long edge", "resize.png", resized});

    // Opaque, then translucent over it: source-over in sRGB with straight alpha
    // is what the two sides have to agree on where they cross.
    images::Recipe drawn{};
    add_stroke(drawn, {220, 30, 30, 255}, 1638, {6554, 9830, 32768, 52428, 58982, 13107});
    add_stroke(drawn, {20, 60, 230, 128}, 3277, {3277, 32768, 62258, 32768});
    add_stroke(drawn, {255, 255, 255, 255}, 2458, {45875, 45875});
    out.push_back({"strokes, opaque and translucent", "strokes.png", drawn});

    images::Recipe everything{};
    everything.turns = 3;
    everything.flip = true;
    everything.crop = images::FixedRect{6554, 0, 52428, 65535};
    everything.long_edge_px = 600;
    add_stroke(everything, {250, 200, 0, 255}, 2458, {13107, 13107, 52428, 52428});
    add_stroke(everything, {0, 160, 80, 160}, 4915, {52428, 6554, 13107, 58982});
    out.push_back({"every operation at once", "everything.png", everything});

    // The negative control: the first case without its flip. A comparison that
    // cannot tell these two apart is not measuring anything.
    images::Recipe turned{};
    turned.turns = 1;
    out.push_back({"control: a quarter turn only", "control-turn.png", turned});
    return out;
}

[[nodiscard]] bool write_source(const std::string& path) {
    VipsImage* xy = nullptr;
    if (vips_xyz(&xy, kSourceWidth, kSourceHeight, nullptr) != 0) { return false; }
    const double scale[2] = {255.0 / (kSourceWidth - 1), 255.0 / (kSourceHeight - 1)};
    const double offset[2] = {0.0, 0.0};
    VipsImage* scaled = nullptr;
    const int scaled_ok = vips_linear(xy, &scaled, scale, offset, 2, "uchar", TRUE, nullptr);
    g_object_unref(xy);
    if (scaled_ok != 0) { return false; }
    VipsImage* rgb = nullptr;
    const int joined = vips_bandjoin_const1(scaled, &rgb, 96.0, nullptr);
    g_object_unref(scaled);
    if (joined != 0) { return false; }
    // sRGB, so the saver writes an ordinary 8-bit RGB PNG.
    VipsImage* tagged = nullptr;
    const int copied =
        vips_copy(rgb, &tagged, "interpretation", VIPS_INTERPRETATION_sRGB, nullptr);
    g_object_unref(rgb);
    if (copied != 0) { return false; }
    const int written = vips_pngsave(tagged, path.c_str(), "strip", TRUE, nullptr);
    g_object_unref(tagged);
    return written == 0;
}

[[nodiscard]] bool copy_out(const std::filesystem::path& from, const std::filesystem::path& to) {
    std::error_code ec;
    std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing, ec);
    return !ec;
}

int fail(std::string_view why) {
    std::fprintf(stderr, "testapp_emit_edit_renders: %.*s\n", static_cast<int>(why.size()),
                 why.data());
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: testapp_emit_edit_renders <output directory>\n");
        return 2;
    }
    const std::filesystem::path out_dir = argv[1];
    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);
    if (ec) { return fail("cannot create the output directory"); }

    images::init("testapp_emit_edit_renders");
    if (!images::available()) { return fail("built without libvips"); }

    std::string root = std::filesystem::temp_directory_path().string() + "/anvil-renders-XXXXXX";
    if (::mkdtemp(root.data()) == nullptr) { return fail("cannot create a storage directory"); }
    fs::Storage::init(root);
    const fs::Storage& storage = fs::Storage::instance();

    int status = 0;
    const anvil::Uuid source = anvil::uuid::generate_v4();
    const std::filesystem::path source_master =
        std::filesystem::path{root} /
        std::string{fs::media_relative_path(testapp::kMedia, source).view()};
    const images::ImageInfo info{kSourceWidth, kSourceHeight, 1};

    std::string manifest;
    manifest.reserve(4096);

    if (!storage.open_shard_for_write(testapp::kMedia, source).ok() ||
        !write_source(source_master.string()) ||
        !copy_out(source_master, out_dir / "source.png")) {
        status = fail("cannot write the source");
    } else {
        manifest += R"({"source":{"file":"source.png","width":)";
        json::append_json_int(manifest, kSourceWidth);
        manifest += R"(,"height":)";
        json::append_json_int(manifest, kSourceHeight);
        manifest += R"(},"limits":{"max_strokes":)";
        json::append_json_int(manifest, images::kEditLimits.max_strokes);
        manifest += R"(,"max_points":)";
        json::append_json_int(manifest, images::kEditLimits.max_points);
        manifest += R"(,"max_edge_px":)";
        json::append_json_int(manifest, images::kEditLimits.max_edge_px);
        manifest += R"(,"min_edge_px":)";
        json::append_json_int(manifest, images::kEditLimits.min_edge_px);
        manifest += R"(},"cases":[)";

        bool first = true;
        for (const Case& c : cases()) {
            const anvil::Result<images::EditPlan> plan =
                images::plan_edit(c.recipe, info, images::kEditLimits);
            if (!plan) {
                status = fail(c.name);
                break;
            }
            const anvil::Uuid id = anvil::uuid::generate_v4();
            const int fd = ::open(source_master.c_str(), O_RDONLY | O_CLOEXEC);
            if (fd < 0) {
                status = fail("cannot reopen the source");
                break;
            }
            const anvil::Result<images::RenderedEdit> rendered = images::render_edit(
                storage, testapp::kMedia, id, fd, fs::Mime::Png, c.recipe, plan.value());
            ::close(fd);
            if (!rendered) {
                status = fail(c.name);
                break;
            }
            const std::filesystem::path master =
                std::filesystem::path{root} /
                std::string{fs::media_relative_path(testapp::kMedia, id).view()};
            if (!copy_out(master, out_dir / std::string{c.file})) {
                status = fail("cannot copy a render out");
                break;
            }

            std::vector<std::uint8_t> encoded;
            images::encode_recipe(c.recipe, encoded);
            if (!first) { manifest += ','; }
            first = false;
            manifest += '{';
            json::append_json_key(manifest, "name");
            json::append_json_string(manifest, c.name);
            manifest += ',';
            json::append_json_key(manifest, "file");
            json::append_json_string(manifest, c.file);
            manifest += ',';
            json::append_json_key(manifest, "recipe");
            json::append_json_string(manifest, anvil::crypto::base64url_encode(encoded));
            manifest += ',';
            json::append_json_key(manifest, "out_width");
            json::append_json_int(manifest, rendered.value().width);
            manifest += ',';
            json::append_json_key(manifest, "out_height");
            json::append_json_int(manifest, rendered.value().height);
            manifest += '}';
        }
        manifest += "]}\n";
    }

    if (status == 0) {
        std::ofstream file{out_dir / "manifest.json", std::ios::binary | std::ios::trunc};
        file << manifest;
        if (!file) { status = fail("cannot write the manifest"); }
    }

    fs::Storage::shutdown();
    std::filesystem::remove_all(root, ec);
    return status;
}
