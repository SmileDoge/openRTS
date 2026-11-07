#include <openrts/renderer/task_pool.hpp>

#include <openrts/core/profile.hpp>

#include <algorithm>
#include <string>

namespace openrts::renderer {

TaskPool::TaskPool(const std::size_t threads) {
    const auto count = (std::max)(threads, std::size_t{1});
    workers_.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        workers_.emplace_back([this, i] {
            const auto name = "worker " + std::to_string(i + 1);
            OPENRTS_THREAD_NAME(name.c_str());
            work();
        });
    }
}

TaskPool::~TaskPool() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    for (auto& worker : workers_) worker.join();
}

void TaskPool::start(std::vector<std::function<void()>> tasks) {
    {
        std::lock_guard lock(mutex_);
        tasks_ = std::move(tasks);
        next_ = 0;
        running_ = 0;
    }
    wake_.notify_all();
}

void TaskPool::wait() {
    std::unique_lock lock(mutex_);
    done_.wait(lock, [this] { return next_ >= tasks_.size() && running_ == 0; });
    tasks_.clear();
    next_ = 0;
}

void TaskPool::work() {
    std::unique_lock lock(mutex_);
    for (;;) {
        wake_.wait(lock, [this] { return stopping_ || next_ < tasks_.size(); });
        if (stopping_) return;
        const auto index = next_++;
        ++running_;
        auto task = tasks_[index];
        lock.unlock();
        task();
        lock.lock();
        --running_;
        if (next_ >= tasks_.size() && running_ == 0) done_.notify_all();
    }
}

} // namespace openrts::renderer
