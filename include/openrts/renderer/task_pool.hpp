#pragma once

// A few long-lived worker threads for the frame's parallel work (recording
// command buffers): started once, woken per batch of tasks, so a frame pays
// no thread creation.

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace openrts::renderer {

class TaskPool {
public:
    // `threads` workers (at least one).
    explicit TaskPool(std::size_t threads);
    ~TaskPool();
    TaskPool(const TaskPool&) = delete;
    TaskPool& operator=(const TaskPool&) = delete;

    // Hands the tasks to the workers and returns at once; wait() blocks
    // until every one of them ran. Tasks of one start() run concurrently,
    // in no particular order.
    void start(std::vector<std::function<void()>> tasks);
    void wait();
    [[nodiscard]] std::size_t threads() const noexcept { return workers_.size(); }

private:
    void work();

    std::vector<std::thread> workers_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable done_;
    std::vector<std::function<void()>> tasks_;
    std::size_t next_ = 0;
    std::size_t running_ = 0;
    bool stopping_ = false;
};

} // namespace openrts::renderer
