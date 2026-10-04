// stb_image implementation unit — compiled once, linked into enactus_backend.
// Kept in its own .cc so api.cc changes do not force a re-parse of the heavy stb headers.

#define STB_IMAGE_IMPLEMENTATION
// Uploads are untrusted input. Compile in only the two decoders the upload
// endpoint accepts: most published stb_image CVEs live in the GIF, PSD, HDR,
// PIC, PNM and TGA decoders, which this removes from the binary entirely.
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_MAX_DIMENSIONS 16384
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_IMPLEMENTATION

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wcast-qual"

#include "third_party/stb_image.h"
#include "third_party/stb_image_write.h"
#include "third_party/stb_image_resize2.h"

#pragma GCC diagnostic pop
