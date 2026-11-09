// Added by WhiteCloudOL to test FIFO reservations, parallel slots and cancellation.
#include "breeze/scheduler.h"
#include "breeze/model.h"

#include <atomic>
#include <cstdio>
#include <future>
#include <stdexcept>

using namespace breeze;

static void require(bool ok, const char * message) {
    if (!ok) throw std::runtime_error(message);
}

static void until(const std::function<bool()> & ready) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!ready()) {
        require(std::chrono::steady_clock::now() < deadline, "queue operation timed out");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

static void fifo(int parallel) {
    BreezeModel model;
    GenScheduler scheduler;
    scheduler.start(model, parallel);
    require(scheduler.try_acquire(parallel), "could not reserve all slots");
    std::vector<int> order;
    std::mutex mutex;
    std::vector<std::future<void>> requests;
    for (int i = 0; i < 3; ++i) {
        requests.push_back(std::async(std::launch::async, [&, i] {
            require(scheduler.acquire(parallel), "unexpected cancellation");
            SlotHold slot(scheduler, parallel);
            std::lock_guard<std::mutex> lock(mutex);
            order.push_back(i);
        }));
        until([&] { return scheduler.queue_status().waiting == (size_t) i + 1; });
    }
    scheduler.release(parallel);
    for (auto & request : requests) request.get();
    require(order == std::vector<int>({0, 1, 2}), "FIFO order changed");
    require(scheduler.queue_status().active == 0, "reservation leaked");
}

static void cancel_head() {
    BreezeModel model;
    GenScheduler scheduler;
    scheduler.start(model, 3);
    require(scheduler.try_acquire(3), "could not reserve parallel slots");
    std::atomic<bool> cancelled{false};
    auto first = std::async(std::launch::async, [&] {
        return scheduler.acquire(2, [&] { return cancelled.load(); });
    });
    until([&] { return scheduler.queue_status().waiting == 1; });
    std::promise<void> release;
    auto gate = release.get_future();
    std::atomic<bool> second_started{false};
    auto second = std::async(std::launch::async, [&] {
        require(scheduler.acquire(1), "second request was cancelled");
        SlotHold slot(scheduler, 1);
        second_started = true;
        gate.wait();
    });
    until([&] { return scheduler.queue_status().waiting == 2; });
    scheduler.release(1);
    require(!scheduler.try_acquire(1), "new request bypassed the FIFO");
    require(!second_started, "small request bypassed the FIFO head");
    cancelled = true;
    require(!first.get(), "cancelled head acquired slots");
    until([&] { return second_started.load(); });
    require(scheduler.queue_status().waiting == 0, "cancelled waiter was retained");
    require(scheduler.queue_status().active == 3, "parallel capacity changed");
    release.set_value();
    second.get();
    scheduler.release(2);
    require(scheduler.queue_status().active == 0, "cancellation leaked slots");
}

static void shutdown() {
    BreezeModel model;
    GenScheduler scheduler;
    scheduler.start(model, 1);
    require(scheduler.try_acquire(), "could not reserve slot");
    auto request = std::async(std::launch::async, [&] { return scheduler.acquire(); });
    until([&] { return scheduler.queue_status().waiting == 1; });
    scheduler.stop();
    require(!request.get(), "shutdown retained a waiting request");
    scheduler.release();
}

int main() {
    try {
        fifo(1);
        fifo(3);
        cancel_head();
        shutdown();
        printf("FIFO queue tests passed\n");
        return 0;
    } catch (const std::exception & e) {
        fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}
