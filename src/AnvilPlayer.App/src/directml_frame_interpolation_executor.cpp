#include "AnvilPlayer/App/directml_frame_interpolation_executor.h"

#include <DirectML.h>
#include <dml_provider_factory.h>
#include <onnxruntime_cxx_api.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

namespace anvil::app {
namespace {

std::wstring SanitizeError(const char* message) {
    std::wstring output;
    if (!message) return output;
    while (*message && output.size() < 240) {
        const unsigned char value = static_cast<unsigned char>(*message++);
        output.push_back(value >= 32 && value < 127 ? static_cast<wchar_t>(value) : L'_');
    }
    return output;
}

}  // namespace

struct DirectMlFrameInterpolationExecutor::Impl {
    struct Job {
        Microsoft::WRL::ComPtr<ID3D12Resource> input;
        Microsoft::WRL::ComPtr<ID3D12Resource> output;
        Microsoft::WRL::ComPtr<ID3D12Fence> dependencyFence;
        uint64_t dependencyValue = 0;
        uint64_t completionValue = 0;
        TensorShape shape;
    };

    Ort::Env environment{ORT_LOGGING_LEVEL_WARNING, "anvil-directml-frame-graph"};
    std::unique_ptr<Ort::Session> session;
    const OrtDmlApi* dmlApi = nullptr;
    Microsoft::WRL::ComPtr<IDMLDevice> dmlDevice;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    Microsoft::WRL::ComPtr<ID3D12Fence> completionFence;
    std::string inputName;
    std::string outputName;
    std::thread worker;
    mutable std::mutex mutex;
    std::condition_variable condition;
    std::deque<Job> jobs;
    uint64_t nextCompletionValue = 1;
    bool stopping = false;
    bool ready = false;
    bool jobActive = false;
    std::wstring lastError;
    std::unordered_map<uint64_t, std::wstring> completionErrors;

    void SetError(std::wstring error) {
        std::scoped_lock lock(mutex);
        lastError = std::move(error);
    }

    void Run() {
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

            bool succeeded = true;
            std::wstring jobError;
            const auto fail = [&](std::wstring error) {
                jobError = std::move(error);
                succeeded = false;
            };
            if (job.dependencyFence && job.dependencyValue != 0 &&
                FAILED(queue->Wait(job.dependencyFence.Get(), job.dependencyValue))) {
                fail(L"directml_dependency_wait_failed");
            }
            void* inputAllocation = nullptr;
            void* outputAllocation = nullptr;
            if (succeeded) {
                OrtStatus* status = dmlApi->CreateGPUAllocationFromD3DResource(
                    job.input.Get(), &inputAllocation);
                if (status) {
                    fail(L"directml_wrap_input_failed");
                    Ort::GetApi().ReleaseStatus(status);
                }
            }
            if (succeeded) {
                OrtStatus* status = dmlApi->CreateGPUAllocationFromD3DResource(
                    job.output.Get(), &outputAllocation);
                if (status) {
                    fail(L"directml_wrap_output_failed");
                    Ort::GetApi().ReleaseStatus(status);
                }
            }

            if (succeeded) {
                try {
                    const std::array<int64_t, 4> inputShape{
                        1, 7, static_cast<int64_t>(job.shape.height),
                        static_cast<int64_t>(job.shape.width)};
                    const std::array<int64_t, 4> outputShape{
                        1, 3, static_cast<int64_t>(job.shape.height),
                        static_cast<int64_t>(job.shape.width)};
                    const std::size_t inputElements =
                        static_cast<std::size_t>(7) * job.shape.width * job.shape.height;
                    const std::size_t outputElements =
                        static_cast<std::size_t>(3) * job.shape.width * job.shape.height;
                    Ort::MemoryInfo memoryInfo(
                        "DML", OrtDeviceAllocator, 0, OrtMemTypeDefault);
                    {
                        Ort::Value inputValue = Ort::Value::CreateTensor<Ort::Float16_t>(
                            memoryInfo,
                            static_cast<Ort::Float16_t*>(inputAllocation), inputElements,
                            inputShape.data(), inputShape.size());
                        Ort::Value outputValue = Ort::Value::CreateTensor<Ort::Float16_t>(
                            memoryInfo,
                            static_cast<Ort::Float16_t*>(outputAllocation), outputElements,
                            outputShape.data(), outputShape.size());
                        Ort::IoBinding binding(*session);
                        binding.BindInput(inputName.c_str(), inputValue);
                        binding.BindOutput(outputName.c_str(), outputValue);
                        session->Run(Ort::RunOptions{nullptr}, binding);
                    }
                } catch (const Ort::Exception& error) {
                    fail(L"directml_inference_failed_" + SanitizeError(error.what()));
                } catch (...) {
                    fail(L"directml_inference_failed_unknown");
                }
            }

            if (outputAllocation) {
                OrtStatus* status = dmlApi->FreeGPUAllocation(outputAllocation);
                if (status) Ort::GetApi().ReleaseStatus(status);
            }
            if (inputAllocation) {
                OrtStatus* status = dmlApi->FreeGPUAllocation(inputAllocation);
                if (status) Ort::GetApi().ReleaseStatus(status);
            }
            {
                std::scoped_lock lock(mutex);
                completionErrors[job.completionValue] = std::move(jobError);
            }
            // Publish status before signaling. The compositor can therefore
            // consume a definitive per-job result once the fence is complete.
            if (FAILED(queue->Signal(completionFence.Get(), job.completionValue))) {
                SetError(L"directml_completion_signal_failed");
            }
            {
                std::scoped_lock lock(mutex);
                jobActive = false;
            }
        }
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
        session.reset();
        completionFence.Reset();
        queue.Reset();
        dmlDevice.Reset();
        dmlApi = nullptr;
        completionErrors.clear();
        nextCompletionValue = 1;
        ready = false;
        jobActive = false;
        stopping = false;
    }
};

DirectMlFrameInterpolationExecutor::DirectMlFrameInterpolationExecutor()
    : impl_(std::make_unique<Impl>()) {}

DirectMlFrameInterpolationExecutor::~DirectMlFrameInterpolationExecutor() {
    Reset();
}

std::filesystem::path DirectMlFrameInterpolationExecutor::DefaultModelPath() {
    std::array<wchar_t, 32768> path{};
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) return {};
    return std::filesystem::path(path.data()).parent_path() /
        L"assets" / L"models" / L"rife_v4.25_lite_fp16.onnx";
}

std::size_t DirectMlFrameInterpolationExecutor::OutputBufferBytes(
    const TensorShape shape) noexcept {
    return static_cast<std::size_t>(3) * shape.width * shape.height * sizeof(uint16_t);
}

bool DirectMlFrameInterpolationExecutor::Initialize(
    ID3D12Device* device,
    ID3D12CommandQueue* computeQueue,
    const std::filesystem::path& modelPath) {
    Reset();
    if (!device || !computeQueue || modelPath.empty()) {
        impl_->lastError = L"directml_missing_initialization_argument";
        return false;
    }
    try {
        if (FAILED(DMLCreateDevice(device, DML_CREATE_DEVICE_FLAG_NONE,
                                   IID_PPV_ARGS(&impl_->dmlDevice)))) {
            impl_->lastError = L"directml_device_create_failed";
            return false;
        }
        const void* providerApi = nullptr;
        Ort::ThrowOnError(Ort::GetApi().GetExecutionProviderApi(
            "DML", ORT_API_VERSION, &providerApi));
        impl_->dmlApi = static_cast<const OrtDmlApi*>(providerApi);
        if (!impl_->dmlApi) {
            impl_->lastError = L"directml_provider_api_unavailable";
            return false;
        }
        Ort::SessionOptions options;
        options.DisableMemPattern();
        options.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        Ort::ThrowOnError(impl_->dmlApi->SessionOptionsAppendExecutionProvider_DML1(
            options, impl_->dmlDevice.Get(), computeQueue));
        impl_->session = std::make_unique<Ort::Session>(
            impl_->environment, modelPath.c_str(), options);
        if (impl_->session->GetInputCount() != 1 || impl_->session->GetOutputCount() != 1) {
            impl_->lastError = L"directml_unexpected_model_interface";
            Reset();
            return false;
        }
        const auto inputInfo = impl_->session->GetInputTypeInfo(0).GetTensorTypeAndShapeInfo();
        const auto outputInfo = impl_->session->GetOutputTypeInfo(0).GetTensorTypeAndShapeInfo();
        const auto inputShape = inputInfo.GetShape();
        const auto outputShape = outputInfo.GetShape();
        if (inputInfo.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16 ||
            outputInfo.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16 ||
            inputShape.size() != 4 || (inputShape[1] > 0 && inputShape[1] != 7) ||
            outputShape.size() != 4 || (outputShape[1] > 0 && outputShape[1] != 3)) {
            impl_->lastError = L"directml_model_must_be_fp16_7_to_3";
            Reset();
            return false;
        }
        Ort::AllocatorWithDefaultOptions allocator;
        const auto inputName = impl_->session->GetInputNameAllocated(0, allocator);
        const auto outputName = impl_->session->GetOutputNameAllocated(0, allocator);
        impl_->inputName = inputName.get();
        impl_->outputName = outputName.get();
        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                       IID_PPV_ARGS(&impl_->completionFence)))) {
            impl_->lastError = L"directml_completion_fence_create_failed";
            Reset();
            return false;
        }
        impl_->queue = computeQueue;
        impl_->ready = true;
        impl_->worker = std::thread([this] { impl_->Run(); });
        return true;
    } catch (const Ort::Exception& error) {
        impl_->lastError = L"directml_initialize_failed_" + SanitizeError(error.what());
    } catch (...) {
        impl_->lastError = L"directml_initialize_failed_unknown";
    }
    Reset();
    return false;
}

void DirectMlFrameInterpolationExecutor::Reset() {
    if (impl_) impl_->Stop();
}

bool DirectMlFrameInterpolationExecutor::IsReady() const noexcept {
    std::scoped_lock lock(impl_->mutex);
    return impl_->ready;
}

bool DirectMlFrameInterpolationExecutor::CanSubmit() const noexcept {
    std::scoped_lock lock(impl_->mutex);
    constexpr std::size_t kMaxOutstandingJobs = 2;
    return impl_->ready &&
        impl_->jobs.size() + (impl_->jobActive ? 1u : 0u) < kMaxOutstandingJobs;
}

std::wstring DirectMlFrameInterpolationExecutor::BackendName() const {
    return L"onnx_runtime_directml_dml1_d3d12";
}

std::wstring DirectMlFrameInterpolationExecutor::LastError() const {
    std::scoped_lock lock(impl_->mutex);
    return impl_->lastError;
}

SubmitResult DirectMlFrameInterpolationExecutor::Submit(
    ID3D12Resource* inputTensor,
    ID3D12Resource* outputTensor,
    const UINT tensorWidth,
    const UINT tensorHeight,
    ID3D12Fence* dependencyFence,
    const uint64_t dependencyValue) {
    SubmitResult result;
    if (!inputTensor || !outputTensor) {
        result.reason = L"directml_missing_tensor";
        return result;
    }
    {
        std::scoped_lock lock(impl_->mutex);
        if (!impl_->ready || tensorWidth == 0 || tensorHeight == 0) {
            result.reason = L"directml_executor_not_configured";
            return result;
        }
        constexpr std::size_t kMaxOutstandingJobs = 2;
        if (impl_->jobs.size() + (impl_->jobActive ? 1u : 0u) >=
            kMaxOutstandingJobs) {
            result.reason = L"directml_executor_busy";
            return result;
        }
        Impl::Job job;
        job.input = inputTensor;
        job.output = outputTensor;
        job.dependencyFence = dependencyFence;
        job.dependencyValue = dependencyValue;
        job.completionValue = impl_->nextCompletionValue++;
        job.shape = {tensorWidth, tensorHeight};
        result.accepted = true;
        result.completion.fence = impl_->completionFence;
        result.completion.value = job.completionValue;
        impl_->jobs.push_back(std::move(job));
    }
    impl_->condition.notify_one();
    return result;
}

std::wstring DirectMlFrameInterpolationExecutor::TakeCompletionError(
    const uint64_t completionValue) {
    std::scoped_lock lock(impl_->mutex);
    const auto found = impl_->completionErrors.find(completionValue);
    if (found == impl_->completionErrors.end()) {
        return L"directml_completion_status_missing";
    }
    std::wstring error = std::move(found->second);
    impl_->completionErrors.erase(found);
    return error;
}

}  // namespace anvil::app
