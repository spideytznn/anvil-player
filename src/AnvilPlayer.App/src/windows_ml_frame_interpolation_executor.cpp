#include "AnvilPlayer/App/windows_ml_frame_interpolation_executor.h"

#include <windows.ai.machinelearning.native.h>
#include <winrt/Windows.AI.MachineLearning.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/base.h>
#include <wrl/client.h>

#include <array>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

namespace anvil::app {
namespace {

std::wstring HResultError(const wchar_t* prefix, const HRESULT result) {
    wchar_t code[16]{};
    swprintf_s(code, L"%08X", static_cast<unsigned int>(result));
    return std::wstring(prefix) + L"_0x" + code;
}

}  // namespace

struct WindowsMlFrameInterpolationExecutor::Impl {
    using LearningModel = winrt::Windows::AI::MachineLearning::LearningModel;
    using LearningModelBinding = winrt::Windows::AI::MachineLearning::LearningModelBinding;
    using LearningModelDevice = winrt::Windows::AI::MachineLearning::LearningModelDevice;
    using LearningModelSession = winrt::Windows::AI::MachineLearning::LearningModelSession;
    using TensorFloat16Bit = winrt::Windows::AI::MachineLearning::TensorFloat16Bit;

    struct Job {
        Microsoft::WRL::ComPtr<ID3D12Resource> input;
        Microsoft::WRL::ComPtr<ID3D12Resource> output;
        Microsoft::WRL::ComPtr<ID3D12Fence> dependencyFence;
        uint64_t dependencyValue = 0;
        uint64_t completionValue = 0;
        UINT width = 0;
        UINT height = 0;
    };

    LearningModel model{nullptr};
    LearningModelDevice mlDevice{nullptr};
    LearningModelSession session{nullptr};
    Microsoft::WRL::ComPtr<ITensorStaticsNative> tensorStatics;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    Microsoft::WRL::ComPtr<ID3D12Fence> completionFence;
    winrt::hstring inputName;
    winrt::hstring outputName;
    std::thread worker;
    mutable std::mutex mutex;
    std::condition_variable condition;
    std::deque<Job> jobs;
    std::unordered_map<uint64_t, std::wstring> completionErrors;
    uint64_t nextCompletionValue = 1;
    bool stopping = false;
    bool ready = false;
    bool jobActive = false;
    std::wstring lastError;

    TensorFloat16Bit WrapTensor(ID3D12Resource* resource,
                                const std::array<int64_t, 4>& shape) const {
        TensorFloat16Bit tensor{nullptr};
        winrt::check_hresult(tensorStatics->CreateFromD3D12Resource(
            resource, const_cast<int64_t*>(shape.data()), static_cast<int>(shape.size()),
            reinterpret_cast<IUnknown**>(winrt::put_abi(tensor))));
        return tensor;
    }

    void Run() {
        const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        for (;;) {
            Job job;
            {
                std::unique_lock lock(mutex);
                condition.wait(lock, [this] { return stopping || !jobs.empty(); });
                if (stopping && jobs.empty()) break;
                job = std::move(jobs.front());
                jobs.pop_front();
                jobActive = true;
            }

            std::wstring error;
            if (job.dependencyFence && job.dependencyValue != 0 &&
                FAILED(queue->Wait(job.dependencyFence.Get(), job.dependencyValue))) {
                error = L"windows_ml_dependency_wait_failed";
            }
            if (error.empty()) {
                try {
                    const std::array<int64_t, 4> inputShape{
                        1, 7, static_cast<int64_t>(job.height), static_cast<int64_t>(job.width)};
                    const std::array<int64_t, 4> outputShape{
                        1, 3, static_cast<int64_t>(job.height), static_cast<int64_t>(job.width)};
                    TensorFloat16Bit input = WrapTensor(job.input.Get(), inputShape);
                    TensorFloat16Bit output = WrapTensor(job.output.Get(), outputShape);
                    LearningModelBinding binding{session};
                    binding.Bind(inputName, input);
                    binding.Bind(outputName, output);
                    session.Evaluate(binding, L"anvil-frame-graph");
                } catch (const winrt::hresult_error& failure) {
                    error = HResultError(L"windows_ml_inference_failed", failure.code());
                } catch (...) {
                    error = L"windows_ml_inference_failed_unknown";
                }
            }
            {
                std::scoped_lock lock(mutex);
                completionErrors[job.completionValue] = std::move(error);
            }
            if (FAILED(queue->Signal(completionFence.Get(), job.completionValue))) {
                std::scoped_lock lock(mutex);
                lastError = L"windows_ml_completion_signal_failed";
            }
            {
                std::scoped_lock lock(mutex);
                jobActive = false;
            }
        }
        if (SUCCEEDED(apartment)) CoUninitialize();
    }

    void Stop() {
        {
            std::scoped_lock lock(mutex);
            stopping = true;
            jobs.clear();
        }
        condition.notify_all();
        if (worker.joinable()) worker.join();
        jobs.clear();
        completionErrors.clear();
        session = nullptr;
        mlDevice = nullptr;
        model = nullptr;
        tensorStatics.Reset();
        completionFence.Reset();
        queue.Reset();
        inputName.clear();
        outputName.clear();
        nextCompletionValue = 1;
        stopping = false;
        ready = false;
        jobActive = false;
    }
};

WindowsMlFrameInterpolationExecutor::WindowsMlFrameInterpolationExecutor()
    : impl_(std::make_unique<Impl>()) {}

WindowsMlFrameInterpolationExecutor::~WindowsMlFrameInterpolationExecutor() {
    Reset();
}

bool WindowsMlFrameInterpolationExecutor::Initialize(
    ID3D12Device* device,
    ID3D12CommandQueue* computeQueue,
    const std::filesystem::path& modelPath) {
    Reset();
    if (!device || !computeQueue || modelPath.empty()) {
        impl_->lastError = L"windows_ml_missing_initialization_argument";
        return false;
    }
    try {
        const winrt::hstring deviceClass{L"Windows.AI.MachineLearning.LearningModelDevice"};
        Microsoft::WRL::ComPtr<ILearningModelDeviceFactoryNative> deviceFactory;
        winrt::check_hresult(RoGetActivationFactory(
            static_cast<HSTRING>(winrt::get_abi(deviceClass)), IID_PPV_ARGS(&deviceFactory)));
        winrt::check_hresult(deviceFactory->CreateFromD3D12CommandQueue(
            computeQueue,
            reinterpret_cast<IUnknown**>(winrt::put_abi(impl_->mlDevice))));

        const winrt::hstring tensorClass{L"Windows.AI.MachineLearning.TensorFloat16Bit"};
        winrt::check_hresult(RoGetActivationFactory(
            static_cast<HSTRING>(winrt::get_abi(tensorClass)),
            IID_PPV_ARGS(&impl_->tensorStatics)));

        impl_->model = Impl::LearningModel::LoadFromFilePath(modelPath.wstring());
        const auto inputs = impl_->model.InputFeatures();
        const auto outputs = impl_->model.OutputFeatures();
        if (inputs.Size() != 1 || outputs.Size() != 1) {
            impl_->lastError = L"windows_ml_unexpected_model_interface";
            Reset();
            return false;
        }
        impl_->inputName = inputs.GetAt(0).Name();
        impl_->outputName = outputs.GetAt(0).Name();
        impl_->session = Impl::LearningModelSession{impl_->model, impl_->mlDevice};
        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                       IID_PPV_ARGS(&impl_->completionFence)))) {
            impl_->lastError = L"windows_ml_completion_fence_create_failed";
            Reset();
            return false;
        }
        impl_->queue = computeQueue;
        impl_->ready = true;
        impl_->lastError.clear();
        impl_->worker = std::thread([this] { impl_->Run(); });
        return true;
    } catch (const winrt::hresult_error& failure) {
        impl_->lastError = HResultError(L"windows_ml_initialize_failed", failure.code());
    } catch (...) {
        impl_->lastError = L"windows_ml_initialize_failed_unknown";
    }
    Reset();
    return false;
}

void WindowsMlFrameInterpolationExecutor::Reset() {
    if (impl_) impl_->Stop();
}

bool WindowsMlFrameInterpolationExecutor::IsReady() const noexcept {
    std::scoped_lock lock(impl_->mutex);
    return impl_->ready;
}

bool WindowsMlFrameInterpolationExecutor::CanSubmit() const noexcept {
    std::scoped_lock lock(impl_->mutex);
    constexpr std::size_t kMaxOutstandingJobs = 2;
    return impl_->ready &&
        impl_->jobs.size() + (impl_->jobActive ? 1u : 0u) < kMaxOutstandingJobs;
}

std::wstring WindowsMlFrameInterpolationExecutor::BackendName() const {
    return L"windows_ml_native_d3d12";
}

std::wstring WindowsMlFrameInterpolationExecutor::LastError() const {
    std::scoped_lock lock(impl_->mutex);
    return impl_->lastError;
}

SubmitResult WindowsMlFrameInterpolationExecutor::Submit(
    ID3D12Resource* inputTensor,
    ID3D12Resource* outputTensor,
    const UINT tensorWidth,
    const UINT tensorHeight,
    ID3D12Fence* dependencyFence,
    const uint64_t dependencyValue) {
    SubmitResult result;
    if (!inputTensor || !outputTensor || tensorWidth == 0 || tensorHeight == 0) {
        result.reason = L"windows_ml_missing_tensor";
        return result;
    }
    {
        std::scoped_lock lock(impl_->mutex);
        if (!impl_->ready) {
            result.reason = L"windows_ml_executor_not_ready";
            return result;
        }
        constexpr std::size_t kMaxOutstandingJobs = 2;
        if (impl_->jobs.size() + (impl_->jobActive ? 1u : 0u) >=
            kMaxOutstandingJobs) {
            result.reason = L"windows_ml_executor_busy";
            return result;
        }
        Impl::Job job;
        job.input = inputTensor;
        job.output = outputTensor;
        job.width = tensorWidth;
        job.height = tensorHeight;
        job.dependencyFence = dependencyFence;
        job.dependencyValue = dependencyValue;
        job.completionValue = impl_->nextCompletionValue++;
        result.accepted = true;
        result.completion.fence = impl_->completionFence;
        result.completion.value = job.completionValue;
        impl_->jobs.push_back(std::move(job));
    }
    impl_->condition.notify_one();
    return result;
}

std::wstring WindowsMlFrameInterpolationExecutor::TakeCompletionError(
    const uint64_t completionValue) {
    std::scoped_lock lock(impl_->mutex);
    const auto found = impl_->completionErrors.find(completionValue);
    if (found == impl_->completionErrors.end()) {
        return L"windows_ml_completion_status_missing";
    }
    std::wstring error = std::move(found->second);
    impl_->completionErrors.erase(found);
    return error;
}

}  // namespace anvil::app
