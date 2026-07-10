#include "AnvilPlayer/App/playback_supervisor.h"

#include <utility>

namespace anvil::app {

PlaybackSupervisor::PlaybackSupervisor(anvil::playback::PlayerController& controller)
    : controller_(controller) {
}

PlaybackSupervisor::~PlaybackSupervisor() {
    RequestShutdown();
    if (worker_.joinable()) {
        if (stopped_.load()) {
            worker_.detach();
        } else {
            worker_.join();
        }
    }
}

bool PlaybackSupervisor::Start(HWND notificationWindow,
                               const UINT openCompleteMessage,
                               const UINT stoppedMessage,
                               const uint64_t windowCookie) {
    std::scoped_lock lock(mutex_);
    if (worker_.joinable() || !notificationWindow || openCompleteMessage == 0 || stoppedMessage == 0) {
        return false;
    }
    notificationWindow_ = notificationWindow;
    openCompleteMessage_ = openCompleteMessage;
    stoppedMessage_ = stoppedMessage;
    windowCookie_ = windowCookie;
    shutdownRequested_ = false;
    stopped_.store(false);
    try {
        worker_ = std::thread([this] { WorkerLoop(); });
    } catch (...) {
        stopped_.store(true);
        notificationWindow_ = nullptr;
        openCompleteMessage_ = 0;
        stoppedMessage_ = 0;
        return false;
    }
    return true;
}

uint64_t PlaybackSupervisor::Open(std::filesystem::path path,
                                  const bool autoplay,
                                  const std::chrono::milliseconds timeout) {
    std::scoped_lock lock(mutex_);
    if (shutdownRequested_ || !worker_.joinable()) {
        return 0;
    }
    const uint64_t generation = generation_.fetch_add(1) + 1;
    failedGeneration_.store(0);
    if (active_) {
        activeStopSource_.request_stop();
    }
    completed_.reset();
    pending_ = OpenRequest{generation, std::move(path), autoplay, timeout};
    cv_.notify_one();
    return generation;
}

void PlaybackSupervisor::CancelOpen() {
    std::scoped_lock lock(mutex_);
    generation_.fetch_add(1);
    failedGeneration_.store(0);
    pending_.reset();
    completed_.reset();
    if (active_) {
        activeStopSource_.request_stop();
    }
}

void PlaybackSupervisor::RequestShutdown() {
    std::scoped_lock lock(mutex_);
    if (shutdownRequested_) {
        return;
    }
    shutdownRequested_ = true;
    generation_.fetch_add(1);
    failedGeneration_.store(0);
    pending_.reset();
    completed_.reset();
    if (active_) {
        activeStopSource_.request_stop();
    }
    cv_.notify_one();
}

std::optional<PlaybackSupervisor::OpenCompletion> PlaybackSupervisor::TakeCompletion() {
    std::scoped_lock lock(mutex_);
    if (!completed_) {
        return std::nullopt;
    }
    std::optional<OpenCompletion> completion{std::move(*completed_)};
    completed_.reset();
    return completion;
}

uint64_t PlaybackSupervisor::TakeFailedGeneration() {
    return failedGeneration_.exchange(0);
}

void PlaybackSupervisor::WorkerLoop() {
    for (;;) {
        OpenRequest request;
        std::stop_token stopToken;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] { return shutdownRequested_ || pending_.has_value(); });
            if (shutdownRequested_) {
                break;
            }
            request = std::move(*pending_);
            pending_.reset();
            activeStopSource_ = std::stop_source{};
            stopToken = activeStopSource_.get_token();
            active_ = true;
        }

        try {
            auto prepared = controller_.PrepareMedia(
                request.path,
                false,
                anvil::playback::MediaProbeOptions{stopToken, request.timeout});

            bool deliver = false;
            {
                std::scoped_lock lock(mutex_);
                active_ = false;
                deliver = !shutdownRequested_ &&
                          request.generation == generation_.load() &&
                          !prepared.probe.cancelled;
                if (deliver) {
                    // Keep ownership in the supervisor. The window message is
                    // only a wake-up; the regular UI timer also polls this
                    // mailbox, so PostMessage failure cannot strand BeginOpen.
                    completed_.emplace(OpenCompletion{
                        windowCookie_,
                        request.generation,
                        request.autoplay,
                        std::move(prepared),
                    });
                }
            }

            if (deliver) {
                PostMessageW(notificationWindow_,
                             openCompleteMessage_,
                             static_cast<WPARAM>(request.generation),
                             0);
            }
        } catch (...) {
            bool deliverFailure = false;
            {
                std::scoped_lock lock(mutex_);
                active_ = false;
                deliverFailure = !shutdownRequested_ &&
                                 request.generation == generation_.load();
                if (deliverFailure) {
                    // Allocation and library exceptions must not escape the
                    // worker and terminate the process. This allocation-free
                    // fallback is consumed by the same UI timer mailbox poll.
                    failedGeneration_.store(request.generation);
                }
            }
            if (deliverFailure) {
                PostMessageW(notificationWindow_,
                             openCompleteMessage_,
                             static_cast<WPARAM>(request.generation),
                             0);
            }
        }
    }

    if (notificationWindow_ && stoppedMessage_ != 0) {
        PostMessageW(notificationWindow_,
                     stoppedMessage_,
                     static_cast<WPARAM>(windowCookie_),
                     0);
    }
    // Last access to object state. A background reaper may detach immediately
    // after observing this flag without racing a subsequent member access.
    stopped_.store(true);
}

}  // namespace anvil::app
