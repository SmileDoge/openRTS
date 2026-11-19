#include <openrts/net/play.hpp>
#include <openrts/net/wire.hpp>

namespace openrts::net {
namespace {

constexpr std::uint8_t weather_version = 1;

} // namespace

Bytes encode_openrts_weather(const WeatherReport& report) {
    WireWriter writer;
    writer.u8(weather_version);
    writer.f32(report.temperature_c);
    writer.f32(report.cloud);
    writer.f32(report.rain);
    writer.f32(report.snow);
    writer.f32(report.wind_mps);
    writer.f32(report.wind_from);
    writer.f32(report.visibility_m);
    writer.f32(report.thunder);
    writer.f32(report.wetness);
    writer.f32(report.ground_snow);
    return writer.take();
}

std::optional<WeatherReport> decode_openrts_weather(const ByteView body) {
    WireReader reader(body);
    if (reader.u8() != weather_version) return std::nullopt;
    WeatherReport report;
    report.temperature_c = reader.f32();
    report.cloud = reader.f32();
    report.rain = reader.f32();
    report.snow = reader.f32();
    report.wind_mps = reader.f32();
    report.wind_from = reader.f32();
    report.visibility_m = reader.f32();
    report.thunder = reader.f32();
    report.wetness = reader.f32();
    report.ground_snow = reader.f32();
    if (!reader.ok() || !reader.at_end()) return std::nullopt;
    return report;
}

} // namespace openrts::net
