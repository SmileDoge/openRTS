#pragma once

// Profiling zones for Tracy (https://github.com/wolfpld/tracy), compiled in
// with -DOPENRTS_ENABLE_TRACY=ON (docs/profiling.md). Without it every macro
// is empty and nothing of Tracy is built or linked.
//
//   OPENRTS_ZONE("name");          a zone to the end of the scope
//   OPENRTS_ZONE_FUNCTION();       the same, named after the function
//   OPENRTS_ZONE_TEXT(text, size); a detail on the zone (a slot, a train)
//   OPENRTS_FRAME();               a frame ends (the main loop, after present)
//   OPENRTS_THREAD_NAME("name");   this thread's name in the timeline
//   OPENRTS_PLOT("name", value);   a value over time (FPS, draw calls)
//
// The client starts on demand: until the Tracy program connects it keeps
// nothing, and it listens on localhost only.

#include <cstring>

#if defined(OPENRTS_TRACY)
#include <tracy/Tracy.hpp>
#include <tracy/TracyC.h>
#define OPENRTS_ZONE(name) ZoneScopedN(name)
#define OPENRTS_ZONE_FUNCTION() ZoneScoped
#define OPENRTS_ZONE_TEXT(text, size) ZoneText(text, size)
#define OPENRTS_FRAME() FrameMark
#define OPENRTS_THREAD_NAME(name) tracy::SetThreadName(name)
#define OPENRTS_PLOT(name, value) TracyPlot(name, value)
#else
#define OPENRTS_ZONE(name) static_cast<void>(0)
#define OPENRTS_ZONE_FUNCTION() static_cast<void>(0)
#define OPENRTS_ZONE_TEXT(text, size) static_cast<void>(sizeof(text) + sizeof(size))
#define OPENRTS_FRAME() static_cast<void>(0)
#define OPENRTS_THREAD_NAME(name) static_cast<void>(sizeof(name))
#define OPENRTS_PLOT(name, value) static_cast<void>(sizeof(value))
#endif

namespace openrts::core {

// Zones one after another on one thread, not tied to a scope: the frame's
// stages (the viewer's StageClock). A zone is opened under the name it is
// expected to end as; close() gives the name it did end as, and when that
// differs the zone is renamed (in the timeline: Tracy's statistics count it
// under the first name). Names are string literals.
class SequentialZone {
public:
    SequentialZone() = default;
    SequentialZone(const SequentialZone&) = delete;
    SequentialZone& operator=(const SequentialZone&) = delete;
    ~SequentialZone() { close(nullptr); }

#if defined(OPENRTS_TRACY)
    void open(const char* name) {
        close(nullptr);
        // An allocated source location per zone: its name is its group in
        // the statistics (equal ones are merged by the profiler).
        static constexpr char file[] = __FILE__;
        static constexpr char function[] = "frame stage";
        const auto location = ___tracy_alloc_srcloc_name(__LINE__, file, sizeof(file) - 1, function, sizeof(function) - 1, name,
                                                          std::strlen(name), 0);
        context_ = ___tracy_emit_zone_begin_alloc(location, 1);
        name_ = name;
        open_ = true;
    }
    // Ends the open zone; `name` is what it was (nullptr: as opened).
    void close(const char* name) {
        if (!open_) return;
        if (name && std::strcmp(name, name_) != 0) ___tracy_emit_zone_name(context_, name, std::strlen(name));
        ___tracy_emit_zone_end(context_);
        open_ = false;
    }

private:
    TracyCZoneCtx context_{};
    const char* name_ = "";
    bool open_ = false;
#else
    void open(const char*) {}
    void close(const char*) {}
#endif
};

} // namespace openrts::core
