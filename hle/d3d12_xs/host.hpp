// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "object.hpp"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace g6shim::d3d12_xs {

namespace later {
struct HostTexture;
}

namespace shadow {
class Shadow;
}

namespace texture {
class Staging;
}

namespace host {

inline constexpr std::uint32_t kPcStateMask = 0x01ff'ffff;
inline constexpr std::uint32_t kRtvRing = 1024;
inline constexpr std::uint32_t kPresentFramesInFlight = 2;

struct PresentedPlane {
    ID3D12Resource* texture = nullptr;
    std::uint32_t state = 0;
};

bool compose(ID3D12GraphicsCommandList* list, std::span<const PresentedPlane> planes, ID3D12Resource* target);
inline constexpr std::uint64_t kScratchChunk = 4 << 20;
inline constexpr std::uint64_t kTimestampFrequency = 100'000'000;

inline constexpr std::uint32_t kBufferOnlyStates = 0x1 | 0x2 | 0x100 | 0x200;

std::uint32_t pc_state(std::uint32_t state);
std::uint32_t texture_state(std::uint32_t state);
std::uint32_t legal_for(std::uint32_t flags, std::uint32_t state);
std::uint32_t legal_state(ID3D12Resource* texture, std::uint32_t state);
D3D12_RESOURCE_BARRIER transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                                  D3D12_RESOURCE_STATES after);

struct Written {
    enum class Kind { kValue, kGpuTimestamp, kSystemTimestamp };

    static Written of(std::uint64_t value);
    static Written gpu_timestamp();
    static Written system_timestamp();

    void write(std::uint8_t* at, std::size_t bytes) const;

    Kind kind = Kind::kValue;
    std::uint64_t value = 0;
};

struct CopyStep {
    std::uint8_t* destination = nullptr;
    const std::uint8_t* source = nullptr;
    std::size_t bytes = 0;
    bool title_memory = true;
};

struct RefreshStep {
    std::shared_ptr<shadow::Shadow> shadow;
    std::size_t address = 0;
    std::size_t bytes = 0;
};

struct WorkStep {
    std::function<void()> work;
    bool title_memory = true;
};

struct GpuWriteStep {
    std::shared_ptr<shadow::Shadow> shadow;
    std::size_t address = 0;
    std::size_t bytes = 0;
};

struct TextureStep {
    std::shared_ptr<texture::Staging> staging;
};

using Step = std::variant<CopyStep, RefreshStep, WorkStep, GpuWriteStep, TextureStep>;

struct WriteStep {
    std::uint8_t* destination = nullptr;
    Written written;
    std::size_t bytes = 0;
};

struct Deferred {
    std::vector<Step> copies;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> uploads;
    std::vector<Microsoft::WRL::ComPtr<IUnknown>> kept;
    std::set<std::tuple<std::size_t, std::size_t, std::size_t>> refreshed;
    std::set<std::pair<const void*, std::uint64_t>> refreshed_sets;
    std::unordered_set<std::size_t> textures;
    std::vector<WriteStep> writes;
    bool budget_zeroed = false;
};

struct Batch {
    void add(const std::shared_ptr<shadow::Shadow>& shadow, std::size_t address, std::size_t bytes);
    bool texture_due(const void* staging);
    void title_written();
    void flush();
    std::uint64_t copied() const { return copied_; }

private:
    std::vector<std::tuple<std::shared_ptr<shadow::Shadow>, std::size_t, std::size_t>> refreshes_;
    std::unordered_set<const void*> textures_;
    std::uint64_t copied_ = 0;
};

struct SubmissionResult {
    HRESULT signal = E_FAIL;
    HRESULT armed = E_FAIL;
    DWORD wait = WAIT_FAILED;
};

using SubmissionStep = SubmissionResult (*)(ID3D12CommandQueue* queue, ID3D12Fence* fence, std::uint64_t value,
                                           HANDLE event, void* context);

void set_submission_step_for_test(SubmissionStep step, void* context);

struct PhaseSample {
    double milliseconds = 0;
    std::uint64_t bytes = 0;
};

struct SubmissionTiming {
    PhaseSample prepare;
    PhaseSample execute_call;
    PhaseSample wait;
    PhaseSample copy_back;
    PhaseSample gpu;
    bool gpu_known = false;
};

struct IntervalTiming {
    std::uint64_t submissions = 0;
    PhaseSample prepare;
    double prepare_max_ms = 0;
    PhaseSample execute_call;
    double execute_max_ms = 0;
    PhaseSample wait;
    double wait_max_ms = 0;
    PhaseSample copy_back;
    double copy_back_max_ms = 0;
    PhaseSample gpu;
    std::uint64_t gpu_samples = 0;
};

void add_submission_timing(IntervalTiming& interval, const SubmissionTiming& sample);
IntervalTiming summarise_interval(std::span<const SubmissionTiming> samples);
std::optional<double> last_gpu_milliseconds_for_test();

class HostList;

class Host {
public:
    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;
    ~Host();

    static Host* get();

    ID3D12Device* device() const { return device_.Get(); }
    ID3D12CommandQueue* queue() const { return queue_.Get(); }
    ID3D12Fence* fence() const { return fence_.Get(); }

    std::unique_ptr<HostList> new_list();
    void execute(const std::vector<HostList*>& runs);
    std::optional<std::string> log_messages();
    bool mesh_shaders();

    D3D12_CPU_DESCRIPTOR_HANDLE rtv(ID3D12Resource* resource, const D3D12_RENDER_TARGET_VIEW_DESC* desc);
    D3D12_CPU_DESCRIPTOR_HANDLE dsv(ID3D12Resource* resource, const D3D12_DEPTH_STENCIL_VIEW_DESC* desc);

    std::optional<std::pair<Microsoft::WRL::ComPtr<ID3D12Resource>, std::uint8_t*>> upload_buffer(
        std::uint64_t bytes);
    std::optional<std::pair<Microsoft::WRL::ComPtr<ID3D12Resource>, std::uint8_t*>> shared_buffer(
        std::uint64_t bytes);

    std::optional<bool> present(ID3D12Resource* texture, std::uint32_t state, std::uint64_t frame);
    std::optional<bool> present(std::span<const PresentedPlane> planes, std::uint64_t frame);

private:
    Host();

    struct Presenter;
    friend bool compose(ID3D12GraphicsCommandList* list, std::span<const PresentedPlane> planes,
                        ID3D12Resource* target);
    bool composition_available(DXGI_FORMAT format);
    bool compose_planes(ID3D12GraphicsCommandList* list, std::span<const PresentedPlane> planes,
                        ID3D12Resource* target);
    void log_composition_failure();
    ID3D12PipelineState* pipeline_locked(DXGI_FORMAT format);
    bool ensure_composer_locked(DXGI_FORMAT format);

    struct Composer;
    std::unique_ptr<Presenter> make_presenter(std::uint32_t width, std::uint32_t height, DXGI_FORMAT format);

    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    std::mutex submitted_mutex_;
    std::uint64_t submitted_ = 0;
    struct HeldSubmission {
        std::uint64_t enqueued = 0;
        bool signalled = false;
        std::vector<Microsoft::WRL::ComPtr<IUnknown>> objects;
    };

    void release_retained();
    void retain_incomplete(const std::vector<HostList*>& runs, bool signalled);

    std::vector<HeldSubmission> held_;
    std::uint64_t confirmed_ = 0;
    HANDLE event_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtvs_;
    std::uint32_t rtv_size_ = 0;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> dsvs_;
    std::uint32_t dsv_size_ = 0;
    std::atomic<std::uint32_t> next_dsv_{0};
    Microsoft::WRL::ComPtr<ID3D12InfoQueue> messages_;
    std::atomic<std::uint32_t> next_rtv_{0};
    std::mutex presenter_mutex_;
    std::unique_ptr<Presenter> presenter_;
    std::mutex composer_mutex_;
    std::unique_ptr<Composer> composer_;
    bool composition_failed_ = false;

    struct TimestampQueries {
        Microsoft::WRL::ComPtr<ID3D12QueryHeap> heap;
        Microsoft::WRL::ComPtr<ID3D12Resource> readback;
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> begin_allocator;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> begin_list;
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> end_allocator;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> end_list;
        std::uint64_t frequency = 0;
        std::uint64_t* mapped = nullptr;
        bool ready = false;
        bool failed = false;
    };

    bool bracket_timestamps(std::vector<ID3D12CommandList*>& lists);
    std::optional<double> resolve_timestamps();
    bool ensure_timestamps();

    std::unique_ptr<TimestampQueries> timestamps_;
};

class HostList {
public:
    HostList(const HostList&) = delete;
    HostList& operator=(const HostList&) = delete;
    HostList(HostList&&) = delete;
    HostList& operator=(HostList&&) = delete;

    ID3D12GraphicsCommandList* list() const { return list_.Get(); }

    void close();
    void reset();
    std::optional<std::pair<ID3D12Resource*, std::uint32_t>> transition(Object* resource, std::uint32_t wanted);
    void note_state(Object* resource, std::uint32_t state);

    void defer_copy(std::uint8_t* destination, const std::uint8_t* source, std::size_t bytes);
    void defer_fill(std::uint8_t* destination, const std::uint8_t* source, std::size_t bytes);
    void defer_refresh(const std::shared_ptr<shadow::Shadow>& shadow, std::size_t address, std::size_t bytes);
    void defer_gpu_write(const std::shared_ptr<shadow::Shadow>& shadow, std::size_t address, std::size_t bytes);
    void defer_write(std::uint8_t* destination, Written written, std::size_t bytes);
    void defer(std::function<void()> work);
    void defer_host(std::function<void()> work);
    void defer_texture(std::shared_ptr<texture::Staging> staging);

    std::optional<std::pair<std::uint64_t, std::uint8_t*>> upload(std::uint64_t bytes);
    std::optional<std::pair<std::uint64_t, std::uint8_t*>> scratch(std::uint64_t bytes);

    struct Upload {
        Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
        std::uint64_t gpu = 0;
        std::uint8_t* data = nullptr;
    };
    std::optional<Upload> upload_with_resource(std::uint64_t bytes);
    std::optional<std::uint64_t> budget();
    void keep(IUnknown* object);
    bool first_use(std::size_t key);
    bool first_refresh(const void* owner, std::uint64_t generation);
    void used_pipeline(const std::string& name);

private:
    friend class Host;
    HostList() = default;

    struct State {
        std::uint32_t first = 0;
        std::uint32_t last = 0;
        std::shared_ptr<later::HostTexture> copy;
    };

    struct ScratchChunk {
        Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
        std::uint8_t* data = nullptr;
        std::uint64_t gpu = 0;
    };

    void run_deferred(Batch& batch);
    void discard_unpublished();
    void retain_objects(std::vector<Microsoft::WRL::ComPtr<IUnknown>>& objects);
    void replace_busy_allocators();

    bool allocator_busy_ = false;
    ID3D12CommandList* prelude();

    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator_;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list_;
    std::mutex deferred_mutex_;
    Deferred deferred_;
    std::mutex scratch_mutex_;
    std::vector<ScratchChunk> chunks_;
    std::size_t current_chunk_ = 0;
    std::uint64_t used_ = 0;
    std::mutex states_mutex_;
    std::unordered_map<std::uintptr_t, State> states_;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> prelude_allocator_;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> prelude_;
    std::mutex pipelines_mutex_;
    std::set<std::string> pipelines_;
    std::once_flag budget_once_;
    bool budget_available_ = false;
    std::uint64_t budget_gpu_ = 0;
    std::uint8_t* budget_data_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> budget_buffer_;
};

Host* host();
void name(IUnknown* object, const std::string& text);
[[noreturn]] void fail(const std::string& text);

void ensure_state(HostList& list, Object* resource, std::uint32_t state);

void copy_mapped(std::uint8_t* destination, const std::uint8_t* source, std::size_t bytes, bool title_destination);
bool read_into(const std::uint8_t* source, std::span<std::uint8_t> destination);
void write_mapped(std::uint8_t* destination, const std::uint8_t* source, std::size_t bytes);

}  // namespace host

}  // namespace g6shim::d3d12_xs
