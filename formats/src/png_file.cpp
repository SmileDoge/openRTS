#include <openrts/formats/png_file.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>

#include <zlib.h>

namespace openrts::formats {
namespace {

constexpr std::string_view png_signature{"\x89PNG\r\n\x1a\n", 8};

[[nodiscard]] std::uint32_t read_u32_be(const std::string_view bytes, const std::size_t at) {
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at])) << 24)
        | (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 1])) << 16)
        | (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 2])) << 8)
        | static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 3]));
}

void append_u32_be(std::string& out, const std::uint32_t value) {
    out += static_cast<char>((value >> 24) & 0xFF);
    out += static_cast<char>((value >> 16) & 0xFF);
    out += static_cast<char>((value >> 8) & 0xFF);
    out += static_cast<char>(value & 0xFF);
}

[[nodiscard]] std::uint8_t paeth(const int a, const int b, const int c) {
    const int p = a + b - c;
    const int pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    if (pa <= pb && pa <= pc) return static_cast<std::uint8_t>(a);
    return static_cast<std::uint8_t>(pb <= pc ? b : c);
}

// Undoes the filters of `rows` rows of `row_bytes` (each after its filter
// byte) in place; `bpp` bytes a pixel (at least 1).
[[nodiscard]] bool unfilter(std::uint8_t* data, const std::size_t rows, const std::size_t row_bytes, const std::size_t bpp) {
    std::uint8_t* previous = nullptr;
    for (std::size_t y = 0; y < rows; ++y) {
        std::uint8_t* line = data + y * (row_bytes + 1);
        const auto filter = line[0];
        std::uint8_t* row = line + 1;
        for (std::size_t x = 0; x < row_bytes; ++x) {
            const int a = x >= bpp ? row[x - bpp] : 0;
            const int b = previous ? previous[x] : 0;
            const int c = previous && x >= bpp ? previous[x - bpp] : 0;
            switch (filter) {
            case 0: break;
            case 1: row[x] = static_cast<std::uint8_t>(row[x] + a); break;
            case 2: row[x] = static_cast<std::uint8_t>(row[x] + b); break;
            case 3: row[x] = static_cast<std::uint8_t>(row[x] + ((a + b) >> 1)); break;
            case 4: row[x] = static_cast<std::uint8_t>(row[x] + paeth(a, b, c)); break;
            default: return false;
            }
        }
        previous = row;
    }
    return true;
}

} // namespace

PngDecodeResult decode_png(const std::string_view bytes) {
    if (bytes.size() < 8 || bytes.substr(0, 8) != png_signature) return {.error = "not a PNG file"};
    std::uint32_t width = 0, height = 0;
    int depth = 0, colour = -1, interlace = 0;
    std::string compressed;
    std::vector<std::array<std::uint8_t, 4>> palette;
    std::vector<std::uint8_t> transparency;
    std::size_t at = 8;
    bool ended = false;
    while (!ended && at + 12 <= bytes.size()) {
        const auto length = read_u32_be(bytes, at);
        const auto type = bytes.substr(at + 4, 4);
        if (length > bytes.size() - at - 12) return {.error = "a truncated PNG chunk"};
        const auto data = bytes.substr(at + 8, length);
        if (type == "IHDR") {
            if (length < 13) return {.error = "a short IHDR"};
            width = read_u32_be(data, 0);
            height = read_u32_be(data, 4);
            depth = static_cast<unsigned char>(data[8]);
            colour = static_cast<unsigned char>(data[9]);
            interlace = static_cast<unsigned char>(data[12]);
        } else if (type == "PLTE") {
            for (std::size_t i = 0; i + 2 < length; i += 3)
                palette.push_back({static_cast<std::uint8_t>(data[i]), static_cast<std::uint8_t>(data[i + 1]),
                                   static_cast<std::uint8_t>(data[i + 2]), 255});
        } else if (type == "tRNS") {
            transparency.assign(data.begin(), data.end());
        } else if (type == "IDAT") {
            compressed.append(data);
        } else if (type == "IEND") {
            ended = true;
        }
        at += 12 + length;
    }
    if (width == 0 || height == 0 || width > 16384 || height > 16384) return {.error = "PNG size missing or beyond 16384"};
    int samples = 0;
    switch (colour) {
    case 0: samples = 1; break;
    case 2: samples = 3; break;
    case 3: samples = 1; break;
    case 4: samples = 2; break;
    case 6: samples = 4; break;
    default: return {.error = "an unknown PNG colour type " + std::to_string(colour)};
    }
    if (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16) return {.error = "an unknown PNG bit depth"};
    if (colour == 3 && palette.empty()) return {.error = "a palette PNG without PLTE"};
    for (std::size_t i = 0; colour == 3 && i < transparency.size() && i < palette.size(); ++i) palette[i][3] = transparency[i];

    const std::size_t bits = static_cast<std::size_t>(samples) * depth;
    const std::size_t bpp = (std::max)(std::size_t{1}, bits / 8);
    // Adam7: the passes' origins and steps; one pass of the whole image else.
    struct Pass {
        std::uint32_t x0, y0, dx, dy;
    };
    const std::vector<Pass> passes = interlace == 1
                                         ? std::vector<Pass>{{0, 0, 8, 8}, {4, 0, 8, 8}, {0, 4, 4, 8}, {2, 0, 4, 4}, {0, 2, 2, 4}, {1, 0, 2, 2}, {0, 1, 1, 2}}
                                         : std::vector<Pass>{{0, 0, 1, 1}};
    std::size_t raw_size = 0;
    for (const auto& pass : passes) {
        const std::size_t w = width > pass.x0 ? (width - pass.x0 + pass.dx - 1) / pass.dx : 0;
        const std::size_t h = height > pass.y0 ? (height - pass.y0 + pass.dy - 1) / pass.dy : 0;
        if (w && h) raw_size += h * ((w * bits + 7) / 8 + 1);
    }
    std::vector<std::uint8_t> raw(raw_size);
    {
        z_stream stream{};
        if (inflateInit(&stream) != Z_OK) return {.error = "zlib would not start"};
        stream.next_in = reinterpret_cast<Bytef*>(compressed.data());
        stream.avail_in = static_cast<uInt>(compressed.size());
        stream.next_out = raw.data();
        stream.avail_out = static_cast<uInt>(raw.size());
        const int status = inflate(&stream, Z_FINISH);
        const auto produced = raw.size() - stream.avail_out;
        inflateEnd(&stream);
        if ((status != Z_STREAM_END && status != Z_BUF_ERROR && status != Z_OK) || produced != raw.size())
            return {.error = "the PNG's pixels are damaged or short"};
    }

    PngImage image{.width = width, .height = height, .pixels = std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height * 4)};
    const int max_value = (1 << (std::min)(depth, 8)) - 1;
    // A sample of a row (its value at the file's depth; 16 bits: the high byte).
    const auto sample = [&](const std::uint8_t* row, const std::size_t index) -> int {
        if (depth == 16) return row[index * 2];
        if (depth == 8) return row[index];
        const std::size_t bit = index * depth;
        return (row[bit / 8] >> (8 - depth - bit % 8)) & max_value;
    };
    // tRNS of grey or RGB: the 16-bit sample that is transparent.
    const auto key = [&](const int index) -> int {
        if (transparency.size() < static_cast<std::size_t>(index * 2 + 2)) return -1;
        const int value = (transparency[index * 2] << 8) | transparency[index * 2 + 1];
        return depth == 16 ? value >> 8 : value;
    };
    std::size_t offset = 0;
    for (const auto& pass : passes) {
        const std::size_t w = width > pass.x0 ? (width - pass.x0 + pass.dx - 1) / pass.dx : 0;
        const std::size_t h = height > pass.y0 ? (height - pass.y0 + pass.dy - 1) / pass.dy : 0;
        if (!w || !h) continue;
        const std::size_t row_bytes = (w * bits + 7) / 8;
        if (!unfilter(raw.data() + offset, h, row_bytes, bpp)) return {.error = "an unknown PNG row filter"};
        for (std::size_t y = 0; y < h; ++y) {
            const std::uint8_t* row = raw.data() + offset + y * (row_bytes + 1) + 1;
            for (std::size_t x = 0; x < w; ++x) {
                std::uint8_t* out = image.pixels.data() + ((pass.y0 + y * pass.dy) * width + pass.x0 + x * pass.dx) * 4;
                const auto scale = [&](const int v) { return static_cast<std::uint8_t>(depth >= 8 ? v : v * 255 / max_value); };
                switch (colour) {
                case 0: {
                    const int v = sample(row, x);
                    out[0] = out[1] = out[2] = scale(v);
                    out[3] = v == key(0) ? 0 : 255;
                    break;
                }
                case 2: {
                    const int r = sample(row, x * 3), g = sample(row, x * 3 + 1), b = sample(row, x * 3 + 2);
                    out[0] = static_cast<std::uint8_t>(r);
                    out[1] = static_cast<std::uint8_t>(g);
                    out[2] = static_cast<std::uint8_t>(b);
                    out[3] = (r == key(0) && g == key(1) && b == key(2)) ? 0 : 255;
                    break;
                }
                case 3: {
                    const auto index = static_cast<std::size_t>(sample(row, x));
                    const auto entry = index < palette.size() ? palette[index] : std::array<std::uint8_t, 4>{0, 0, 0, 255};
                    std::memcpy(out, entry.data(), 4);
                    break;
                }
                case 4:
                    out[0] = out[1] = out[2] = static_cast<std::uint8_t>(sample(row, x * 2));
                    out[3] = static_cast<std::uint8_t>(sample(row, x * 2 + 1));
                    break;
                case 6:
                    for (int c = 0; c < 4; ++c) out[c] = static_cast<std::uint8_t>(sample(row, x * 4 + c));
                    break;
                }
            }
        }
        offset += h * (row_bytes + 1);
    }
    return {.image = std::move(image)};
}

PngDecodeResult decode_png_path(const std::filesystem::path& file) {
    std::ifstream input(file, std::ios::binary);
    if (!input) return {.error = "cannot open " + file.string()};
    const std::string bytes(std::istreambuf_iterator<char>(input), {});
    return decode_png(bytes);
}

std::string encode_png(const std::uint32_t width, const std::uint32_t height, const std::vector<std::uint8_t>& rgba) {
    if (width == 0 || height == 0 || rgba.size() != static_cast<std::size_t>(width) * height * 4) return {};
    const std::size_t row_bytes = static_cast<std::size_t>(width) * 4;
    // Each row with the filter whose bytes sum smallest (signed): the usual
    // way to pick it, and it packs about as well as trying them all.
    std::vector<std::uint8_t> filtered((row_bytes + 1) * height);
    std::vector<std::uint8_t> trial(row_bytes);
    for (std::uint32_t y = 0; y < height; ++y) {
        const std::uint8_t* row = rgba.data() + y * row_bytes;
        const std::uint8_t* above = y > 0 ? row - row_bytes : nullptr;
        std::uint8_t* out = filtered.data() + y * (row_bytes + 1);
        long best = std::numeric_limits<long>::max();
        for (int filter = 0; filter < 5; ++filter) {
            long sum = 0;
            for (std::size_t x = 0; x < row_bytes; ++x) {
                const int a = x >= 4 ? row[x - 4] : 0;
                const int b = above ? above[x] : 0;
                const int c = above && x >= 4 ? above[x - 4] : 0;
                int predicted = 0;
                switch (filter) {
                case 1: predicted = a; break;
                case 2: predicted = b; break;
                case 3: predicted = (a + b) >> 1; break;
                case 4: predicted = paeth(a, b, c); break;
                default: break;
                }
                trial[x] = static_cast<std::uint8_t>(row[x] - predicted);
                sum += std::abs(static_cast<int>(static_cast<std::int8_t>(trial[x])));
            }
            if (sum < best) {
                best = sum;
                out[0] = static_cast<std::uint8_t>(filter);
                std::memcpy(out + 1, trial.data(), row_bytes);
            }
        }
    }
    uLongf packed_size = compressBound(static_cast<uLong>(filtered.size()));
    std::string packed(packed_size, '\0');
    if (compress2(reinterpret_cast<Bytef*>(packed.data()), &packed_size, filtered.data(), static_cast<uLong>(filtered.size()), 9) != Z_OK)
        return {};
    packed.resize(packed_size);

    std::string png(png_signature);
    const auto chunk = [&](const char* type, const std::string& data) {
        append_u32_be(png, static_cast<std::uint32_t>(data.size()));
        const std::string body = std::string(type, 4) + data;
        png += body;
        append_u32_be(png, static_cast<std::uint32_t>(crc32(0L, reinterpret_cast<const Bytef*>(body.data()), static_cast<uInt>(body.size()))));
    };
    std::string header;
    append_u32_be(header, width);
    append_u32_be(header, height);
    header += std::string("\x08\x06\x00\x00\x00", 5); // 8 bits, RGBA, deflate, filters, not interlaced
    chunk("IHDR", header);
    chunk("IDAT", packed);
    chunk("IEND", {});
    return png;
}

} // namespace openrts::formats
