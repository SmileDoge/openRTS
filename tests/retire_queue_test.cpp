#include <iostream>
#include <memory>

#include <openrts/renderer/gpu_retire_queue.hpp>

namespace {

int failures = 0;

void check(const bool condition, const char* what) {
    if (!condition) {
        ++failures;
        std::cerr << "FAILED: " << what << '\n';
    }
}

// Counts destructions, standing in for GPU buffers and bind groups.
struct Resource {
    explicit Resource(int* counter) : destroyed(counter) {}
    Resource(const Resource&) = delete;
    Resource& operator=(const Resource&) = delete;
    ~Resource() { ++*destroyed; }
    int* destroyed;
};

} // namespace

int main() {
    using openrts::renderer::GpuRetireQueue;
    int destroyed = 0;
    GpuRetireQueue queue;

    // Retired while frame 1 is recorded: in use until the GPU finishes frame 1.
    queue.retire(std::make_shared<Resource>(&destroyed));
    const auto first = queue.submit();
    check(first == 1, "first frame number");
    check(queue.collect() == 0 && destroyed == 0, "not freed before the GPU finished");

    // Retired during frame 2, still referenced by a CPU job.
    auto held = std::make_shared<Resource>(&destroyed);
    queue.retire(held);
    const auto second = queue.submit();

    queue.complete(first);
    check(queue.collect() == 1 && destroyed == 1, "freed once its frame completed");

    queue.complete(second);
    check(queue.collect() == 0 && destroyed == 1, "kept while a job holds it");
    held.reset();
    check(queue.collect() == 1 && destroyed == 2, "freed when the last holder drops it");

    // Completion is monotonic; an older notification does not roll back.
    queue.retire(std::make_shared<Resource>(&destroyed));
    queue.complete(1);
    check(queue.collect() == 0, "a stale completion does not free newer frames");
    queue.submit();
    queue.complete(3);
    check(queue.collect() == 1 && destroyed == 3, "freed after its frame");

    queue.retire(std::make_shared<Resource>(&destroyed));
    check(queue.pending() == 1, "pending count");
    queue.drain();
    check(destroyed == 4 && queue.pending() == 0, "drain on device idle");

    if (failures == 0) std::cout << "retire queue: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
