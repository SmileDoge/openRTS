// openrts-ace's formats: PNG read and written (palette with tRNS, 16-bit
// grey, interlaced), ACE written in each kind and read back by the game's own
// reader (structured exactly, DXT1 closely, its mask, the file's mipmaps).
// With an RTS root: a real DXT1 texture written again reads back alike.
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include <openrts/formats/ace_file.hpp>
#include <openrts/formats/ace_writer.hpp>
#include <openrts/formats/png_file.hpp>

#include <zlib.h>

namespace {

using namespace openrts::formats;

int failures = 0;

void check(const bool condition, const std::string& what) {
    if (!condition) {
        ++failures;
        std::cerr << "FAILED: " << what << '\n';
    }
}

void put_u32_be(std::string& out, const std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) out += static_cast<char>((value >> shift) & 0xFF);
}

void chunk(std::string& png, const char* type, const std::string& data) {
    put_u32_be(png, static_cast<std::uint32_t>(data.size()));
    const std::string body = std::string(type, 4) + data;
    png += body;
    put_u32_be(png, static_cast<std::uint32_t>(crc32(0L, reinterpret_cast<const Bytef*>(body.data()), static_cast<uInt>(body.size()))));
}

// A PNG of the given rows (each already packed at the file's depth, filter 0
// prepended here), colour type and depth; extra chunks before IDAT.
[[nodiscard]] std::string make_png(const std::uint32_t width, const std::uint32_t height, const int depth, const int colour,
                                   const std::vector<std::string>& rows, const std::string& extra = {}, const int interlace = 0) {
    std::string png("\x89PNG\r\n\x1a\n", 8);
    std::string header;
    put_u32_be(header, width);
    put_u32_be(header, height);
    header += static_cast<char>(depth);
    header += static_cast<char>(colour);
    header += std::string(2, '\0');
    header += static_cast<char>(interlace);
    chunk(png, "IHDR", header);
    png += extra;
    std::string raw;
    for (const auto& row : rows) raw += '\0' + row;
    uLongf size = compressBound(static_cast<uLong>(raw.size()));
    std::string packed(size, '\0');
    compress(reinterpret_cast<Bytef*>(packed.data()), &size, reinterpret_cast<const Bytef*>(raw.data()), static_cast<uLong>(raw.size()));
    packed.resize(size);
    chunk(png, "IDAT", packed);
    chunk(png, "IEND", {});
    return png;
}

[[nodiscard]] std::vector<std::uint8_t> noise(const std::uint32_t width, const std::uint32_t height, const unsigned seed) {
    std::mt19937 random(seed);
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4);
    for (auto& value : pixels) value = static_cast<std::uint8_t>(random() & 0xFF);
    return pixels;
}

void png_tests() {
    const auto pixels = noise(37, 23, 1);
    const auto png = encode_png(37, 23, pixels);
    const auto back = decode_png(png);
    check(back.ok() && back.image->width == 37 && back.image->height == 23 && back.image->pixels == pixels, "PNG RGBA8 written and read back");

    // A 2-bit palette with tRNS: 4 colours, the third half transparent.
    std::string extra;
    chunk(extra, "PLTE", std::string("\xFF\x00\x00\x00\xFF\x00\x00\x00\xFF\x10\x20\x30", 12));
    chunk(extra, "tRNS", std::string("\xFF\xFF\x80", 3));
    const auto palette = decode_png(make_png(4, 1, 2, 3, {std::string("\x1B", 1)}, extra)); // indices 0 1 2 3
    check(palette.ok(), "palette PNG read: " + palette.error);
    if (palette.ok()) {
        const auto& p = palette.image->pixels;
        check(p[0] == 255 && p[3] == 255 && p[5] == 255 && p[10] == 255 && p[11] == 128 && p[12] == 0x10 && p[15] == 255,
              "palette entries, tRNS alpha (and none past it)");
    }
    // 16-bit grey: the high byte.
    const auto grey = decode_png(make_png(2, 1, 16, 0, {std::string("\x12\x34\xAB\xCD", 4)}));
    check(grey.ok() && grey.image->pixels[0] == 0x12 && grey.image->pixels[4] == 0xAB && grey.image->pixels[7] == 255, "16-bit grey");

    // Adam7: an 8 x 8 RGBA image in its seven passes.
    const auto image = noise(8, 8, 2);
    struct Pass { int x0, y0, dx, dy; };
    const Pass passes[7] = {{0, 0, 8, 8}, {4, 0, 8, 8}, {0, 4, 4, 8}, {2, 0, 4, 4}, {0, 2, 2, 4}, {1, 0, 2, 2}, {0, 1, 1, 2}};
    std::vector<std::string> rows;
    for (const auto& pass : passes) {
        for (int y = pass.y0; y < 8; y += pass.dy) {
            std::string row;
            for (int x = pass.x0; x < 8; x += pass.dx) row.append(reinterpret_cast<const char*>(image.data() + (y * 8 + x) * 4), 4);
            if (!row.empty()) rows.push_back(row);
        }
    }
    const auto interlaced = decode_png(make_png(8, 8, 8, 6, rows, {}, 1));
    check(interlaced.ok() && interlaced.image->pixels == image, "interlaced (Adam7) PNG");
    check(!decode_png("not a png").ok(), "not a PNG refused");
}

[[nodiscard]] double mean_error(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
    double sum = 0.0;
    for (std::size_t i = 0; i < a.size(); i += 4)
        for (int c = 0; c < 3; ++c) sum += std::abs(static_cast<int>(a[i + c]) - static_cast<int>(b[i + c]));
    return sum / static_cast<double>(a.size() / 4 * 3);
}

void ace_tests() {
    // Structured kinds read back exactly (alpha as each keeps it).
    auto image = noise(64, 64, 3);
    for (const bool zlib : {true, false}) {
        for (const auto format : {AceFormat::rgb, AceFormat::rgb_mask, AceFormat::rgba}) {
            const auto encoded = encode_ace(64, 64, image, {.format = format, .mipmaps = true, .zlib = zlib});
            check(encoded.ok(), "structured ACE written");
            const auto decoded = decode_structured_ace_rgba(encoded.bytes);
            check(decoded.ok(), "structured ACE read back: " + decoded.error);
            if (!decoded.ok()) continue;
            bool same = true;
            for (std::size_t i = 0; i < image.size(); i += 4) {
                for (int c = 0; c < 3; ++c) same &= decoded.image->pixels[i + c] == image[i + c];
                const auto alpha = decoded.image->pixels[i + 3];
                const auto expected = format == AceFormat::rgb ? 255 : format == AceFormat::rgb_mask ? (image[i + 3] >= 128 ? 255 : 0) : image[i + 3];
                same &= alpha == expected;
            }
            check(same, "structured ACE pixels exact (" + std::to_string(static_cast<int>(format)) + (zlib ? ", zlib)" : ", plain)"));
            check(decoded.image->mips_from_file && decoded.image->mips.size() == 6, "its mipmaps taken from the file (options 5)");
        }
    }

    // DXT1: a smooth picture comes back close, the mask exact.
    std::vector<std::uint8_t> smooth(128 * 128 * 4);
    for (int y = 0; y < 128; ++y) {
        for (int x = 0; x < 128; ++x) {
            auto* p = &smooth[(y * 128 + x) * 4];
            p[0] = static_cast<std::uint8_t>(x * 2);
            p[1] = static_cast<std::uint8_t>(y * 2);
            p[2] = static_cast<std::uint8_t>(128 + 100 * std::sin(x * 0.1) * std::cos(y * 0.07));
            p[3] = (x / 16 + y / 16) % 3 == 0 ? 0 : 255;
        }
    }
    const auto dxt = encode_ace(128, 128, smooth, {.format = AceFormat::dxt1});
    const auto dxt_back = decode_structured_ace_rgba(dxt.bytes);
    check(dxt.ok() && dxt_back.ok(), "DXT1 ACE written and read: " + dxt_back.error);
    if (dxt_back.ok()) {
        const auto& p = dxt_back.image->pixels;
        bool mask = true;
        std::vector<std::uint8_t> opaque_only = p, original = smooth;
        for (std::size_t i = 0; i < p.size(); i += 4) {
            mask &= (smooth[i + 3] >= 128) == (p[i + 3] == 255);
            if (smooth[i + 3] < 128) {
                for (int c = 0; c < 3; ++c) opaque_only[i + c] = original[i + c] = 0;
            }
        }
        check(mask, "DXT1's mask: transparent where the PNG was");
        const double error = mean_error(original, opaque_only);
        std::cout << "DXT1 mean error " << error << " of 255" << std::endl;
        check(error < 4.0, "DXT1 close to the picture");
        check(dxt_back.image->mips_from_file && dxt_back.image->mips.size() == 7, "DXT1's mipmaps taken from the file (options 21)");
    }
    // A single colour block: exact to 5:6:5.
    std::uint8_t tile[16][4];
    for (auto& texel : tile) {
        texel[0] = 200;
        texel[1] = 100;
        texel[2] = 50;
        texel[3] = 255;
    }
    const auto block = encode_dxt1_block(tile, false);
    check(block[4] == 0 && block[5] == 0 && block[6] == 0 && block[7] == 0, "a single colour: every texel the first entry");

    // Not a power of two: no mipmaps, said so, still read.
    const auto odd = encode_ace(30, 20, noise(30, 20, 4), {.format = AceFormat::dxt1});
    check(odd.ok() && !odd.notes.empty() && decode_structured_ace_rgba(odd.bytes).ok(), "a 30 x 20 DXT1: no mipmaps, read back");
    check(suggested_ace_format(smooth) == AceFormat::dxt1 && suggested_ace_format(noise(4, 4, 5)) == AceFormat::rgba, "the suggested format");
}

} // namespace

int main(int argc, char* argv[]) {
    png_tests();
    ace_tests();
    // A real DXT1 texture (an RTS root given): written again, read back alike.
    if (argc > 1) {
        const auto file = std::filesystem::path(argv[1]) / "GLOBAL" / "TEXTURES" / "7bridge2.ace";
        const auto original = decode_structured_ace_rgba_path(file);
        check(original.ok(), "7bridge2.ace read: " + original.error);
        if (original.ok()) {
            const auto& image = *original.image;
            const auto again = encode_ace(image.width, image.height, image.pixels, {.format = AceFormat::dxt1});
            const auto back = decode_structured_ace_rgba(again.bytes);
            check(back.ok(), "7bridge2.ace written again and read");
            if (back.ok()) {
                const double error = mean_error(image.pixels, back.image->pixels);
                std::cout << "7bridge2.ace again as DXT1: mean error " << error << ", " << again.bytes.size() / 1024 << " KB" << std::endl;
                check(error < 3.0, "7bridge2.ace again close to itself");
            }
        }
    }
    if (failures == 0) std::cout << "ace and png: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
