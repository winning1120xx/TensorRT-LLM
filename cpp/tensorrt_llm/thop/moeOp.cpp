/*
 * Copyright (c) 2022-2024, NVIDIA CORPORATION.  All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "tensorrt_llm/common/workspace.h"
#include "tensorrt_llm/kernels/cutlass_kernels/fp8_blockscale_gemm/fp8_blockscale_gemm.h"
#include "tensorrt_llm/kernels/internal_cutlass_kernels/include/moe_gemm_kernels.h"
#include "tensorrt_llm/kernels/internal_cutlass_kernels/include/moe_kernels.h"
#include "tensorrt_llm/runtime/torchUtils.h"
#include "tensorrt_llm/thop/thUtils.h"

#include <ATen/native/cuda/Resize.h>

#include <functional>

#define C10_THROW_ERROR_FORMATTED(ErrorType, ...)                                                                      \
    do                                                                                                                 \
    {                                                                                                                  \
        std::ostringstream oss;                                                                                        \
        oss << __VA_ARGS__;                                                                                            \
        C10_THROW_ERROR(ErrorType, oss.str());                                                                         \
    } while (0)

namespace torch_ext
{

namespace common = tensorrt_llm::common;
namespace kernels = tensorrt_llm::kernels;
using profiler_backend = kernels::GemmProfilerBackend;

struct GemmIDMoe
{
    profiler_backend::GemmToProfile gemm_idx;
    int64_t hidden_size;
    int64_t inter_size;
    int num_experts;
    int experts_per_token;

    bool operator==(GemmIDMoe const& id) const
    {
        return id.gemm_idx == gemm_idx && id.hidden_size == hidden_size && id.inter_size == inter_size
            && id.num_experts == num_experts && id.experts_per_token == experts_per_token;
    }

    friend std::ostream& operator<<(std::ostream& out, GemmIDMoe const& id)
    {
        out << "gemm_idx, hidden_size, inter_size, num_experts, experts_per_token=" << static_cast<int>(id.gemm_idx)
            << "," << id.hidden_size << "," << id.inter_size << "," << id.num_experts << "," << id.experts_per_token;
        return out;
    }
};

struct GemmIDMoeHash
{
    std::size_t operator()(GemmIDMoe const& id) const
    {
        size_t hash = std::hash<int>{}(static_cast<int>(id.gemm_idx));
        hash ^= std::hash<int64_t>{}(id.hidden_size);
        hash ^= std::hash<int64_t>{}(id.inter_size);
        hash ^= std::hash<int>{}(id.num_experts);
        hash ^= std::hash<int>{}(id.experts_per_token);
        return hash;
    }
};

using ProfileId = int;
using MProfileMap = std::unordered_map<int, ProfileId>;
using MProfileMapPtr = std::shared_ptr<MProfileMap>;

struct MNKProfileMap
{
    std::unordered_map<GemmIDMoe, MProfileMapPtr, GemmIDMoeHash> profile_map;

    bool existsMProfileMap(GemmIDMoe const& id)
    {
        auto const iter = profile_map.find(id);
        return iter != profile_map.end();
    }

    void createMProfileMap(GemmIDMoe const& id)
    {
        profile_map[id] = std::make_shared<MProfileMap>();
    }

    MProfileMapPtr getMProfileMap(GemmIDMoe const& id)
    {
        auto const iter = profile_map.find(id);
        if (iter == profile_map.end())
        {
            C10_THROW_ERROR_FORMATTED(Error, "Cannot find ID (" << id << ") in the profile map. Abort.");
        }
        return iter->second;
    }
};

struct RunnerTypeKey
{
    c10::ScalarType activation_dtype;
    c10::ScalarType weight_dtype;
    c10::ScalarType output_dtype;
    bool min_latency_mode;

    bool operator==(RunnerTypeKey const& key) const
    {
        return key.activation_dtype == activation_dtype && key.weight_dtype == weight_dtype
            && key.output_dtype == output_dtype && key.min_latency_mode == min_latency_mode;
    }
};

struct RunnerTypeKeyHash
{
    std::size_t operator()(RunnerTypeKey const& key) const
    {
        size_t hash = std::hash<int>{}(static_cast<int>(key.activation_dtype));
        hash ^= std::hash<int>{}(static_cast<int>(key.weight_dtype));
        hash ^= std::hash<int>{}(static_cast<int>(key.output_dtype));
        hash ^= std::hash<bool>{}(key.min_latency_mode);
        return hash;
    }
};

class FusedMoeRunner : public torch::CustomClassHolder
{
public:
    static c10::intrusive_ptr<FusedMoeRunner> getInstance(c10::ScalarType activation_dtype,
        c10::ScalarType weight_dtype, c10::ScalarType output_dtype, bool use_fp8_block_scaling, bool min_latency_mode)
    {
        static std::mutex instance_map_mutex;
        std::lock_guard<std::mutex> lock(instance_map_mutex);

        static std::unordered_map<RunnerTypeKey, c10::intrusive_ptr<FusedMoeRunner>, RunnerTypeKeyHash> instance_map;

        auto const key = RunnerTypeKey{activation_dtype, weight_dtype, output_dtype, min_latency_mode};
        auto const iter = instance_map.find(key);
        if (iter == instance_map.end())
        {
            auto instance = c10::make_intrusive<FusedMoeRunner>(
                activation_dtype, weight_dtype, output_dtype, use_fp8_block_scaling, min_latency_mode);
            instance_map[key] = instance;
            return instance;
        }
        return iter->second;
    }

    template <typename Type, bool NeedQuant = false>
    std::unique_ptr<kernels::CutlassMoeFCRunnerInterface> switch_output_type(c10::ScalarType output_type)
    {
        switch (output_type)
        {
        case c10::ScalarType::Long: // INT64 == FP4
        case c10::ScalarType::Float8_e4m3fn:
            // TODO We need an atomic FP8 reduction for the finalize fusions
            C10_THROW_ERROR_FORMATTED(NotImplementedError,
                "Outputting " << torch::toString(output_type) << " directly is not currently supported");
            // return std::make_unique<kernels::CutlassMoeFCRunner<Type, Type>>();
        case c10::ScalarType::Half:
            if constexpr (NeedQuant)
            {
                return std::make_unique<kernels::CutlassMoeFCRunner<Type, Type, half, half>>();
            }
            else
            {
                return std::make_unique<kernels::CutlassMoeFCRunner<Type, Type, half, Type>>();
            }
#ifdef ENABLE_BF16
        case c10::ScalarType::BFloat16:
            if constexpr (NeedQuant)
            {
                return std::make_unique<kernels::CutlassMoeFCRunner<Type, Type, __nv_bfloat16, __nv_bfloat16>>();
            }
            else
            {
                return std::make_unique<kernels::CutlassMoeFCRunner<Type, Type, __nv_bfloat16, Type>>();
            }
#endif
        default:
            C10_THROW_ERROR_FORMATTED(Error,
                "Invalid output type " << torch::toString(output_type) << " specified for "
                                       << torch::toString(mActivationDtype));
        }
    };

    FusedMoeRunner(c10::ScalarType activation_dtype, c10::ScalarType weight_dtype, c10::ScalarType output_dtype,
        bool use_fp8_block_scaling, bool min_latency_mode)
    {
        mActivationDtype = activation_dtype;
        mWeightDtype = weight_dtype;
        mOutputDtype = output_dtype;
        mUseFp8BlockScaling = use_fp8_block_scaling;
        mMinLatencyMode = min_latency_mode;
        mInnerDimMultiplier = 1;

        // keep consistent with cpp/tensorrt_llm/plugins/mixtureOfExperts/mixtureOfExpertsPlugin.cpp
        if (mActivationDtype == c10::ScalarType::Half && mWeightDtype == c10::ScalarType::Half)
        {
            mKernelRunner = std::make_shared<kernels::CutlassMoeFCRunner<half, half>>();
        }
#ifdef ENABLE_BF16
        else if (mActivationDtype == c10::ScalarType::BFloat16 && mWeightDtype == c10::ScalarType::BFloat16)
        {
            mKernelRunner = std::make_shared<kernels::CutlassMoeFCRunner<__nv_bfloat16, __nv_bfloat16>>();
        }
#ifdef ENABLE_FP8
        else if (mActivationDtype == c10::ScalarType::BFloat16 && mWeightDtype == c10::ScalarType::Float8_e4m3fn)
        {
            mKernelRunner = std::make_unique<kernels::CutlassMoeFCRunner<__nv_bfloat16, __nv_fp8_e4m3>>();
        }
#endif
#endif

#ifdef ENABLE_FP8
        if (isFp8Quant())
        {
            mKernelRunner = switch_output_type<__nv_fp8_e4m3>(mOutputDtype);
        }
#endif
#ifdef ENABLE_FP4
        if (isNvfp4Quant())
        {
            mInnerDimMultiplier = 16;
            switch (mActivationDtype)
            {
            case c10::ScalarType::Half:
#ifdef ENABLE_BF16
            case c10::ScalarType::BFloat16:
#endif
                mKernelRunner = switch_output_type<__nv_fp4_e2m1, true>(mOutputDtype);
                break;
            default: mKernelRunner = switch_output_type<__nv_fp4_e2m1, false>(mOutputDtype);
            }
        }
#endif
        if (!mKernelRunner)
        {
            C10_THROW_ERROR_FORMATTED(Error,
                "Could not construct fused moe op with the requested input combination Activation: "
                    << torch::toString(mActivationDtype) << ", Weight: " << torch::toString(mWeightDtype)
                    << ", Output: " << torch::toString(mOutputDtype));
        }

        mProfiler = std::make_shared<kernels::GemmProfilerBackend>();
        mMNKProfileMap = std::make_shared<MNKProfileMap>();
        mAllProfiles = mKernelRunner->getTactics();
        mMinDimM = -1;
        mMaxDimM = -1;
    }

    ~FusedMoeRunner() = default;
    FusedMoeRunner(FusedMoeRunner const&) = delete;
    void operator=(FusedMoeRunner const&) = delete;

    void runProfile(torch::Tensor const& fc2_expert_weights, int64_t const top_k, int64_t const tp_size,
        int64_t const tp_rank, int64_t const ep_size, int64_t const ep_rank, std::vector<int64_t> num_token_buckets)
    {
        std::lock_guard<std::mutex> lock(mMutex);

        if (mUseFp8BlockScaling)
        {
            return; // TODO
        }

        CHECK_INPUT(fc2_expert_weights, mWeightDtype)
        TORCH_CHECK(fc2_expert_weights.dim() == 3, "fc2_expert_weights must be 3D.");

        int64_t hidden_size = fc2_expert_weights.sizes()[1];
        int64_t inter_size = fc2_expert_weights.sizes()[2] * mInnerDimMultiplier;
        int num_experts = static_cast<int>(fc2_expert_weights.sizes()[0] * ep_size);

        std::sort(num_token_buckets.begin(), num_token_buckets.end());
        mMinDimM = num_token_buckets.front();
        mMaxDimM = num_token_buckets.back();

        cudaStream_t stream;
        common::check_cuda_error(cudaStreamCreate(&stream));

        profiler_backend::GemmToProfile gemm_idxes[]
            = {profiler_backend::GemmToProfile::GEMM_1, profiler_backend::GemmToProfile::GEMM_2};

        for (auto const& gemm_idx : gemm_idxes)
        {
            runProfileGemmIdx(hidden_size, inter_size, num_experts, static_cast<int>(top_k), static_cast<int>(tp_size),
                static_cast<int>(tp_rank), static_cast<int>(ep_size), static_cast<int>(ep_rank), num_token_buckets,
                gemm_idx, stream);
        }

        common::check_cuda_error(cudaStreamDestroy(stream));
    }

    c10::optional<std::vector<int64_t>> getProfileIds(int64_t const num_tokens, torch::Tensor const& fc2_expert_weights,
        int64_t const top_k, int64_t const num_experts)
    {
        std::lock_guard<std::mutex> lock(mMutex);

        CHECK_INPUT(fc2_expert_weights, mWeightDtype)
        TORCH_CHECK(fc2_expert_weights.dim() == 3, "fc2_expert_weights must be 3D.");

        int64_t hidden_size = fc2_expert_weights.sizes()[1];
        int64_t inter_size = fc2_expert_weights.sizes()[2] * mInnerDimMultiplier;
        auto gemm_id_moe1 = GemmIDMoe{profiler_backend::GemmToProfile::GEMM_1, hidden_size, inter_size,
            static_cast<int>(num_experts), static_cast<int>(top_k)};
        auto gemm_id_moe2 = GemmIDMoe{profiler_backend::GemmToProfile::GEMM_2, hidden_size, inter_size,
            static_cast<int>(num_experts), static_cast<int>(top_k)};

        if (!mMNKProfileMap->existsMProfileMap(gemm_id_moe1) || !mMNKProfileMap->existsMProfileMap(gemm_id_moe2))
        {
            return c10::nullopt;
        }

        int64_t capped_num_tokens = num_tokens;
        if (num_tokens < mMinDimM)
        {
            capped_num_tokens = mMinDimM;
        }
        else if (num_tokens > mMaxDimM)
        {
            capped_num_tokens = mMaxDimM;
        }

        int gemm1_profile_id = mMNKProfileMap->getMProfileMap(gemm_id_moe1)->at(capped_num_tokens);
        int gemm2_profile_id = mMNKProfileMap->getMProfileMap(gemm_id_moe2)->at(capped_num_tokens);
        std::vector<int64_t> profile_ids = {gemm1_profile_id, gemm2_profile_id};
        return profile_ids;
    }

    torch::Tensor runMoe(torch::Tensor const& input, torch::Tensor const& token_selected_experts,
        torch::optional<torch::Tensor> token_final_scales, torch::Tensor const& fc1_expert_weights,
        torch::Tensor const& fc2_expert_weights, torch::optional<c10::ArrayRef<torch::Tensor>> quant_scales,
        torch::optional<torch::Tensor> input_sf, int64_t const tp_size, int64_t const tp_rank, int64_t const ep_size,
        int64_t const ep_rank, torch::optional<c10::ArrayRef<int64_t>> profile_ids)
    {
        std::lock_guard<std::mutex> lock(mMutex);

        CHECK_INPUT(input, mActivationDtype)
        CHECK_INPUT(token_selected_experts, at::ScalarType::Int)
        if (token_final_scales)
        {
            CHECK_INPUT(token_final_scales.value(), at::ScalarType::Float)
        }
        CHECK_INPUT(fc1_expert_weights, mWeightDtype)
        CHECK_INPUT(fc2_expert_weights, mWeightDtype)

        TORCH_CHECK(input.dim() == 2, "input must be 2D.");
        TORCH_CHECK(token_selected_experts.dim() == 2, "token_selected_experts must be 2D.");

        TORCH_CHECK(fc1_expert_weights.dim() == 3, "fc1_expert_weights must be 3D.");
        TORCH_CHECK(fc2_expert_weights.dim() == 3, "fc2_expert_weights must be 3D.");
        TORCH_CHECK(input.sizes()[0] == token_selected_experts.sizes()[0],
            "input and token_selected_experts must have the same num tokens.");
        if (token_final_scales)
        {
            TORCH_CHECK(token_final_scales.value().dim() == 2, "token_selected_experts_probs must be 2D.");
            TORCH_CHECK(input.sizes()[0] == token_final_scales.value().sizes()[0],
                "input and token_selected_experts_probs must have the same num tokens.");
            TORCH_CHECK(token_selected_experts.sizes()[1] == token_final_scales.value().sizes()[1],
                "token_selected_experts and token_final_scales must have the same number of experts per token.");
        }
        TORCH_CHECK(fc1_expert_weights.sizes()[0] == fc2_expert_weights.sizes()[0],
            "fc1_expert_weights and fc2_expert_weights must have the same number of experts.");
        TORCH_CHECK(fc1_expert_weights.sizes()[1] == fc2_expert_weights.sizes()[2] * mInnerDimMultiplier * 2,
            "fc1_expert_weights inter size must be 2 times fc2_expert_weights inter size.");

        int experts_per_token = token_selected_experts.sizes()[1];
        int64_t num_rows = input.sizes()[0];
        int64_t hidden_size = fc2_expert_weights.sizes()[1];
        int64_t inter_size = fc2_expert_weights.sizes()[2] * mInnerDimMultiplier;
        int const num_experts_on_rank = fc2_expert_weights.sizes()[0];
        auto const num_experts_total = static_cast<int>(num_experts_on_rank * ep_size);
        auto parallelism_config = kernels::MOEParallelismConfig(tp_size, tp_rank, ep_size, ep_rank);
        auto activation_type = tensorrt_llm::ActivationType::Swiglu;

        setRunnerProfiles(profile_ids);

        auto stream = at::cuda::getCurrentCUDAStream(input.get_device());

        std::vector<int64_t> output_shape = {num_rows, hidden_size};
        auto output = torch::empty(output_shape, input.options().dtype(mOutputDtype));

        WorkspaceInfo workspace_info = getWorkspaceInfo(num_rows, hidden_size, inter_size, num_experts_total,
            static_cast<int>(experts_per_token), activation_type, parallelism_config);

        auto const quant_params = getQuantParams(num_experts_on_rank, hidden_size, inter_size, quant_scales);
        kernels::MoeMinLatencyParams min_latency_params{};

        // TODO: support lora in the future
        kernels::LoraParams lora_params{};
        mKernelRunner->runMoe(input.const_data_ptr(),
            input_sf.has_value() ? input_sf.value().const_data_ptr() : nullptr,
            reinterpret_cast<int const*>(token_selected_experts.const_data_ptr()),
            token_final_scales.has_value() ? reinterpret_cast<float const*>(token_final_scales.value().const_data_ptr())
                                           : nullptr,
            fc1_expert_weights.const_data_ptr(), nullptr, activation_type, fc2_expert_weights.const_data_ptr(), nullptr,
            quant_params, num_rows, hidden_size, inter_size, num_experts_total, static_cast<int>(experts_per_token),
            static_cast<char*>(workspace_info.workspace), output.data_ptr(),
            static_cast<int*>(workspace_info.src_to_dest_map), parallelism_config, false, lora_params,
            mUseFp8BlockScaling, mMinLatencyMode, min_latency_params, stream);

        return output;
    }

    std::tuple<torch::Tensor, torch::Tensor, torch::Tensor, torch::Tensor> runMoeMinLantency(torch::Tensor const& input,
        torch::Tensor const& token_selected_experts, torch::optional<torch::Tensor> token_final_scales,
        torch::Tensor const& fc1_expert_weights, torch::Tensor const& fc2_expert_weights,
        torch::optional<c10::ArrayRef<torch::Tensor>> quant_scales, torch::optional<torch::Tensor> input_sf,
        int64_t const tp_size, int64_t const tp_rank, int64_t const ep_size, int64_t const ep_rank,
        torch::optional<c10::ArrayRef<int64_t>> profile_ids)
    {
        std::lock_guard<std::mutex> lock(mMutex);

        CHECK_INPUT(input, mActivationDtype)
        CHECK_INPUT(token_selected_experts, at::ScalarType::Int)
        if (token_final_scales)
        {
            CHECK_INPUT(token_final_scales.value(), at::ScalarType::Float)
        }
        CHECK_INPUT(fc1_expert_weights, mWeightDtype)
        CHECK_INPUT(fc2_expert_weights, mWeightDtype)

        TORCH_CHECK(input.dim() == 2, "input must be 2D.");
        TORCH_CHECK(token_selected_experts.dim() == 2, "token_selected_experts must be 2D.");

        TORCH_CHECK(fc1_expert_weights.dim() == 3, "fc1_expert_weights must be 3D.");
        TORCH_CHECK(fc2_expert_weights.dim() == 3, "fc2_expert_weights must be 3D.");
        TORCH_CHECK(input.sizes()[0] == token_selected_experts.sizes()[0],
            "input and token_selected_experts must have the same num tokens.");
        if (token_final_scales)
        {
            TORCH_CHECK(token_final_scales.value().dim() == 2, "token_selected_experts_probs must be 2D.");
            TORCH_CHECK(input.sizes()[0] == token_final_scales.value().sizes()[0],
                "input and token_selected_experts_probs must have the same num tokens.");
            TORCH_CHECK(token_selected_experts.sizes()[1] == token_final_scales.value().sizes()[1],
                "token_selected_experts and token_final_scales must have the same number of experts per token.");
        }
        TORCH_CHECK(fc1_expert_weights.sizes()[0] == fc2_expert_weights.sizes()[0],
            "fc1_expert_weights and fc2_expert_weights must have the same number of experts.");
        TORCH_CHECK(fc1_expert_weights.sizes()[1] == fc2_expert_weights.sizes()[2] * mInnerDimMultiplier * 2,
            "fc1_expert_weights inter size must be 2 times fc2_expert_weights inter size.");

        int experts_per_token = token_selected_experts.sizes()[1];
        int64_t num_rows = input.sizes()[0];
        int64_t hidden_size = fc2_expert_weights.sizes()[1];
        int64_t inter_size = fc2_expert_weights.sizes()[2] * mInnerDimMultiplier;
        int const num_experts_on_rank = fc2_expert_weights.sizes()[0];
        auto const num_experts_total = static_cast<int>(num_experts_on_rank * ep_size);
        auto parallelism_config = kernels::MOEParallelismConfig(tp_size, tp_rank, ep_size, ep_rank);
        auto activation_type = tensorrt_llm::ActivationType::Swiglu;

        setRunnerProfiles(profile_ids);

        auto stream = at::cuda::getCurrentCUDAStream(input.get_device());

        std::vector<int64_t> output_shape = {num_rows * num_experts_on_rank, hidden_size};
        auto output = torch::empty(output_shape, input.options().dtype(mOutputDtype));

        auto num_active_experts_per_node = torch::empty({1}, input.options().dtype(at::ScalarType::Int));
        auto experts_to_token_score
            = torch::empty({num_experts_on_rank, num_rows}, input.options().dtype(at::ScalarType::Float));
        auto active_expert_global_ids = torch::empty({num_experts_on_rank}, input.options().dtype(at::ScalarType::Int));

        kernels::MoeMinLatencyParams min_latency_params{};
        min_latency_params.num_active_experts_per_node = static_cast<int*>(num_active_experts_per_node.data_ptr());
        min_latency_params.experts_to_token_score = static_cast<float*>(experts_to_token_score.data_ptr());
        min_latency_params.active_expert_global_ids = static_cast<int*>(active_expert_global_ids.data_ptr());

        WorkspaceInfo workspace_info = getWorkspaceInfo(num_rows, hidden_size, inter_size, num_experts_total,
            static_cast<int>(experts_per_token), activation_type, parallelism_config);

        auto const quant_params = getQuantParams(num_experts_on_rank, hidden_size, inter_size, quant_scales);

        // TODO: support lora in the future
        kernels::LoraParams lora_params{};
        mKernelRunner->runMoe(input.const_data_ptr(),
            input_sf.has_value() ? input_sf.value().const_data_ptr() : nullptr,
            reinterpret_cast<int const*>(token_selected_experts.const_data_ptr()),
            token_final_scales.has_value() ? reinterpret_cast<float const*>(token_final_scales.value().const_data_ptr())
                                           : nullptr,
            fc1_expert_weights.const_data_ptr(), nullptr, activation_type, fc2_expert_weights.const_data_ptr(), nullptr,
            quant_params, num_rows, hidden_size, inter_size, num_experts_total, static_cast<int>(experts_per_token),
            static_cast<char*>(workspace_info.workspace), output.data_ptr(),
            static_cast<int*>(workspace_info.src_to_dest_map), parallelism_config, false, lora_params,
            mUseFp8BlockScaling, mMinLatencyMode, min_latency_params, stream);

        return std::make_tuple(output, num_active_experts_per_node, experts_to_token_score, active_expert_global_ids);
    }

private:
    struct WorkspaceInfo
    {
        void* workspace{};
        void* src_to_dest_map{};
    };

    std::mutex mMutex;
    std::shared_ptr<kernels::CutlassMoeFCRunnerInterface> mKernelRunner;
    std::shared_ptr<kernels::GemmProfilerBackend> mProfiler;
    std::shared_ptr<MNKProfileMap> mMNKProfileMap;
    int64_t mMinDimM;
    int64_t mMaxDimM;
    c10::ScalarType mActivationDtype;
    c10::ScalarType mWeightDtype;
    c10::ScalarType mOutputDtype;
    // number of elements packed into the inner dimension of a matrix
    // e.g. 16 nvfp4 elements are packed into a single int64 element
    int64_t mInnerDimMultiplier;

    bool mUseFp8BlockScaling = false;
    bool mMinLatencyMode = false;

    using Profile = tensorrt_llm::cutlass_extensions::CutlassGemmConfig;
    std::vector<Profile> mAllProfiles;

    void runProfileGemmIdx(int64_t const hidden_size, int64_t const inter_size, int const num_experts,
        int const experts_per_token, int const tp_size, int const tp_rank, int const ep_size, int const ep_rank,
        std::vector<int64_t> const& num_token_buckets, profiler_backend::GemmToProfile const gemm_idx,
        cudaStream_t stream)
    {
        auto gemm_id_moe = GemmIDMoe{gemm_idx, hidden_size, inter_size, num_experts, experts_per_token};

        if (mMNKProfileMap->existsMProfileMap(gemm_id_moe))
        {
            return;
        }

        mMNKProfileMap->createMProfileMap(gemm_id_moe);

        mProfiler->mGemmToProfile = gemm_idx;
        // TODO: support more dtypes and expert parallelism
        auto parallelism_config = kernels::MOEParallelismConfig(tp_size, tp_rank, ep_size, ep_rank);
        mProfiler->init(*mKernelRunner.get(), mProfiler->mGemmToProfile,
            tensorrt_llm::runtime::TorchUtils::dataType(mActivationDtype),
            tensorrt_llm::runtime::TorchUtils::dataType(mWeightDtype),
            tensorrt_llm::runtime::TorchUtils::dataType(mOutputDtype), num_experts, experts_per_token, hidden_size,
            inter_size, /* group_size */ -1, tensorrt_llm::ActivationType::Swiglu,
            /* bias */ false, /* use_lora */ false, mMinLatencyMode, parallelism_config);

        char* profile_workspace = nullptr;
        size_t tmp_workspace_size = mProfiler->getWorkspaceSize(mMaxDimM);
        auto const cu_malloc_status = cudaMalloc(&profile_workspace, tmp_workspace_size);
        TORCH_CHECK(cu_malloc_status == cudaSuccess, "Can't allocate tmp workspace for MOE GEMM tactics profiling.");

        for (auto const& m : num_token_buckets)
        {
            ProfileId best_profile_id = runProfileM(m, profile_workspace, stream);
            mMNKProfileMap->getMProfileMap(gemm_id_moe)->insert({m, best_profile_id});
        }

        auto const cu_free = cudaFree(profile_workspace);
        TORCH_CHECK(cu_free == cudaSuccess, "Can't free tmp workspace for MOE GEMM profiling.");
    }

    ProfileId runProfileM(int64_t const m, char* profile_workspace, cudaStream_t stream)
    {
        mProfiler->prepare(m, profile_workspace, stream);
        float best_time = std::numeric_limits<float>::max();
        ProfileId best_profile_id{0};
        for (int i = 0; i < static_cast<int>(mAllProfiles.size()); ++i)
        {
            auto const& profile = mAllProfiles[i];
            float candidate_time = std::numeric_limits<float>::max();
            try
            {
                candidate_time = runSingleProfile(m, profile, profile_workspace, stream);
            }
            catch (std::exception const& e)
            {
                std::ostringstream msg;
                msg << "Cannot profile configuration " << i << ": " << profile.toString() << "\n (for"
                    << " m=" << m << ")"
                    << ", reason: \"" << e.what() << "\". Skipped";
                cudaGetLastError(); // Reset the last cudaError to cudaSuccess.

                std::cout << "Error: " << msg.str() << std::endl;
                continue;
            }

            if (candidate_time < best_time)
            {
                best_time = candidate_time;
                best_profile_id = i;
            }
        }
        return best_profile_id;
    }

    float runSingleProfile(int64_t const m, Profile const& profile, char* profile_workspace, cudaStream_t stream)
    {
        constexpr int warmup = 3;
        constexpr int runs = 5;

        // warmup
        for (int i = 0; i < warmup; ++i)
        {
            mProfiler->runProfiler(m, profile, profile_workspace, stream);
        }

        cudaEvent_t start;
        cudaEvent_t stop;
        common::check_cuda_error(cudaEventCreate(&start));
        common::check_cuda_error(cudaEventCreate(&stop));
        common::check_cuda_error(cudaStreamSynchronize(stream));
        common::check_cuda_error(cudaEventRecord(start, stream));

        // profile
        for (int i = 0; i < runs; ++i)
        {
            mProfiler->runProfiler(m, profile, profile_workspace, stream);
        }

        common::check_cuda_error(cudaEventRecord(stop, stream));
        common::check_cuda_error(cudaEventSynchronize(stop));
        float elapsed;
        common::check_cuda_error(cudaEventElapsedTime(&elapsed, start, stop));
        common::check_cuda_error(cudaEventDestroy(start));
        common::check_cuda_error(cudaEventDestroy(stop));
        return elapsed / runs;
    }

    void setRunnerProfiles(torch::optional<c10::ArrayRef<int64_t>> profile_ids)
    {
        if (mUseFp8BlockScaling)
        {
            auto config = tensorrt_llm::cutlass_extensions::CutlassGemmConfig(
                tensorrt_llm::cutlass_extensions::CutlassTileConfigSM90::CtaShape128x16x128B,
                tensorrt_llm::cutlass_extensions::MainloopScheduleType::AUTO,
                tensorrt_llm::cutlass_extensions::EpilogueScheduleType::AUTO,
                tensorrt_llm::cutlass_extensions::ClusterShape::ClusterShape_1x1x1);
            mKernelRunner->setTactic(config, config);
            return;
        }

        auto best_gemm1_profile = mAllProfiles.front();
        auto best_gemm2_profile = mAllProfiles.front();
        if (profile_ids.has_value())
        {
            TORCH_CHECK(profile_ids.value().size() == 2, "Expecting 2 profile ids");
            best_gemm1_profile = mAllProfiles.at(profile_ids.value()[0]);
            best_gemm2_profile = mAllProfiles.at(profile_ids.value()[1]);
        }
        mKernelRunner->setTactic(best_gemm1_profile, best_gemm2_profile);
    }

    WorkspaceInfo getWorkspaceInfo(int64_t const num_rows, int64_t const hidden_size, int64_t const inter_size,
        int num_experts, int experts_per_token, tensorrt_llm::ActivationType activation_type,
        kernels::MOEParallelismConfig const& parallelismConfig)
    {
        size_t moe_workspace_size
            = mKernelRunner->getWorkspaceSize(num_rows, hidden_size, inter_size, num_experts, experts_per_token,
                activation_type, parallelismConfig, /* use_lora */ false, mUseFp8BlockScaling, mMinLatencyMode,
                /* hasExpertPrequantScales */ false);
        size_t src_to_dest_map_size = experts_per_token * num_rows * sizeof(int);

        std::vector<size_t> workspaces{moe_workspace_size, src_to_dest_map_size};

        size_t total_workspace_size = common::calculateTotalWorkspaceSize(workspaces.data(), workspaces.size());
        auto workspace = torch::empty({static_cast<long>(total_workspace_size)},
            torch::dtype(torch::kInt8).device(torch::kCUDA).requires_grad(false));

        WorkspaceInfo info{};
        info.workspace = workspace.data_ptr();
        info.src_to_dest_map = common::nextWorkspacePtr(static_cast<int8_t*>(workspace.data_ptr()), moe_workspace_size);

        return info;
    }

    kernels::QuantParams getQuantParams(int64_t const num_experts_on_rank, int64_t const hidden_size,
        int64_t const inter_size, torch::optional<c10::ArrayRef<torch::Tensor>> const& quant_scales) const
    {
        if (isFp8Quant())
        {
            TORCH_CHECK(quant_scales.has_value(), "Expecting quant scales for fp8 quantization");
            TORCH_CHECK(quant_scales.value().size() == 4, "Expecting 4 quant scales for fp8 quantization");

            auto const fc1_dequant = quant_scales.value()[0];
            auto const fc2_quant = quant_scales.value()[1];
            auto const fc2_dequant = quant_scales.value()[2];
            auto const fc1_input_dequant = quant_scales.value()[3];

            CHECK_INPUT(fc1_dequant, c10::ScalarType::Float);
            CHECK_INPUT(fc2_quant, c10::ScalarType::Float);
            CHECK_INPUT(fc2_dequant, c10::ScalarType::Float);
            CHECK_INPUT(fc1_input_dequant, c10::ScalarType::Float);
            TORCH_CHECK(fc1_dequant.dim() == 1, "fc1 dequant must be 1D");
            TORCH_CHECK(fc2_quant.dim() == 0, "fc2 quant must be a scalar tensor");
            TORCH_CHECK(fc2_dequant.dim() == 1, "fc2 quant must be 1D");
            TORCH_CHECK(fc1_input_dequant.dim() == 0, "fc1 input dequant must be a scalar tensor");
            TORCH_CHECK(
                fc1_dequant.sizes()[0] == num_experts_on_rank, "fc1 dequant size must be (num_experts_on_rank,)");
            TORCH_CHECK(
                fc2_dequant.sizes()[0] == num_experts_on_rank, "fc2 dequant size must be (num_experts_on_rank,)");

            return kernels::QuantParams::FP8(static_cast<float const*>(fc1_dequant.data_ptr()),
                static_cast<float const*>(fc2_quant.data_ptr()), static_cast<float const*>(fc2_dequant.data_ptr()),
                /* fp8 output quant scale */ nullptr, static_cast<float const*>(fc1_input_dequant.data_ptr()));
        }
        else if (isNvfp4Quant())
        {
            TORCH_CHECK(quant_scales.has_value(), "Expecting quant scales for nvfp4 quantization");
            TORCH_CHECK(quant_scales.value().size() == 6, "Expecting 6 quant scales for nvfp4 quantization");

            auto const fc1_act_global = quant_scales.value()[0];
            auto const fc1_weight_block = quant_scales.value()[1];
            auto const fc1_global = quant_scales.value()[2];
            auto const fc2_act_global = quant_scales.value()[3];
            auto const fc2_weight_block = quant_scales.value()[4];
            auto const fc2_global = quant_scales.value()[5];

            // The input for scale fc1_weight_block / fc2_weight_block is packed into INT32
            constexpr int FP8_PER_INT32 = 4;
            CHECK_INPUT(fc1_act_global, c10::ScalarType::Float);
            CHECK_INPUT(fc1_weight_block, c10::ScalarType::Int);
            CHECK_INPUT(fc1_global, c10::ScalarType::Float);
            CHECK_INPUT(fc2_act_global, c10::ScalarType::Float);
            CHECK_INPUT(fc2_weight_block, c10::ScalarType::Int);
            CHECK_INPUT(fc2_global, c10::ScalarType::Float);
            TORCH_CHECK(fc1_act_global.dim() == 0, "fc1 act global must be a scalar tensor");
            TORCH_CHECK(fc1_weight_block.dim() == 3, "fc1 weight block must be #D");
            TORCH_CHECK(fc1_global.dim() == 1, "fc1 global must be 1D");
            TORCH_CHECK(fc2_act_global.dim() == 0, "fc2 act global must be a scalar tensor");
            TORCH_CHECK(fc2_weight_block.dim() == 3, "fc2 weight block must be 3D");
            TORCH_CHECK(fc2_global.dim() == 1, "fc2 global must be 1D");
            TORCH_CHECK(fc1_weight_block.sizes()[0] == num_experts_on_rank
                    && fc1_weight_block.sizes()[1] == inter_size * 2
                    && fc1_weight_block.sizes()[2] * FP8_PER_INT32
                            * tensorrt_llm::TmaWarpSpecializedGroupedGemmInput::BlockScaleVectorSize
                        == hidden_size,
                "fc1 weight block size must be (num_experts_on_rank, inter_size * 2, hidden_size // 4 // "
                "block_scale_vector_size)");
            TORCH_CHECK(fc1_global.sizes()[0] == num_experts_on_rank, "fc1 global size must be (num_experts_on_rank,)");
            TORCH_CHECK(fc2_weight_block.sizes()[0] == num_experts_on_rank && fc2_weight_block.sizes()[1] == hidden_size
                    && fc2_weight_block.sizes()[2] * FP8_PER_INT32
                            * tensorrt_llm::TmaWarpSpecializedGroupedGemmInput::BlockScaleVectorSize
                        == inter_size,
                "fc2 weight block size must be (num_experts_on_rank, hidden_size, inter_size // 4 // "
                "block_scale_vector_size)");
            TORCH_CHECK(fc2_global.sizes()[0] == num_experts_on_rank, "fc2 global size must be (num_experts_on_rank,)");

            return kernels::QuantParams::FP4(static_cast<float const*>(fc1_act_global.data_ptr()),
                static_cast<tensorrt_llm::TmaWarpSpecializedGroupedGemmInput::ElementSF*>(fc1_weight_block.data_ptr()),
                static_cast<float const*>(fc1_global.data_ptr()), static_cast<float const*>(fc2_act_global.data_ptr()),
                static_cast<tensorrt_llm::TmaWarpSpecializedGroupedGemmInput::ElementSF*>(fc2_weight_block.data_ptr()),
                static_cast<float const*>(fc2_global.data_ptr()));
        }
        else if (mUseFp8BlockScaling)
        {
            auto& fc1_scales = quant_scales.value()[0];
            auto& fc2_scales = quant_scales.value()[1];
            return kernels::QuantParams::FP8BlockScaling(
                static_cast<float const*>(fc1_scales.data_ptr()), static_cast<float const*>(fc2_scales.data_ptr()));
        }
        else
        {
            return kernels::QuantParams{};
        }
    }

    bool isFp8Quant() const
    {
        return !mUseFp8BlockScaling && mActivationDtype == c10::ScalarType::Float8_e4m3fn
            && mWeightDtype == c10::ScalarType::Float8_e4m3fn;
    }

    bool isNvfp4Quant() const
    {
        return mWeightDtype == c10::ScalarType::Long;
    }
};

torch::Tensor fused_moe(torch::Tensor const& input, torch::Tensor const& token_selected_experts,
    torch::optional<torch::Tensor> token_final_scales, torch::Tensor const& fc1_expert_weights,
    torch::Tensor const& fc2_expert_weights, c10::ScalarType const& output_dtype,
    torch::optional<c10::ArrayRef<torch::Tensor>> quant_scales, torch::optional<torch::Tensor> input_sf,
    int64_t const tp_size, int64_t const tp_rank, int64_t const ep_size, int64_t const ep_rank,
    torch::optional<c10::ArrayRef<int64_t>> profile_ids, bool use_fp8_block_scaling)
{
    return FusedMoeRunner::getInstance(
        input.scalar_type(), fc1_expert_weights.scalar_type(), output_dtype, use_fp8_block_scaling, false)
        ->runMoe(input, token_selected_experts, token_final_scales, fc1_expert_weights, fc2_expert_weights,
            quant_scales, input_sf, tp_size, tp_rank, ep_size, ep_rank, profile_ids);
}

std::tuple<torch::Tensor, torch::Tensor, torch::Tensor, torch::Tensor> fused_moe_min_latency(torch::Tensor const& input,
    torch::Tensor const& token_selected_experts, torch::optional<torch::Tensor> token_final_scales,
    torch::Tensor const& fc1_expert_weights, torch::Tensor const& fc2_expert_weights,
    c10::ScalarType const& output_dtype, torch::optional<c10::ArrayRef<torch::Tensor>> quant_scales,
    torch::optional<torch::Tensor> input_sf, int64_t const tp_size, int64_t const tp_rank, int64_t const ep_size,
    int64_t const ep_rank, torch::optional<c10::ArrayRef<int64_t>> profile_ids, bool use_fp8_block_scaling)
{
    return FusedMoeRunner::getInstance(
        input.scalar_type(), fc1_expert_weights.scalar_type(), output_dtype, use_fp8_block_scaling, true)
        ->runMoeMinLantency(input, token_selected_experts, token_final_scales, fc1_expert_weights, fc2_expert_weights,
            quant_scales, input_sf, tp_size, tp_rank, ep_size, ep_rank, profile_ids);
}

} // namespace torch_ext

TORCH_LIBRARY(trtllm, m)
{
    m.class_<torch_ext::FusedMoeRunner>("FusedMoeProfiler")
        .def_static("get_instance", &torch_ext::FusedMoeRunner::getInstance)
        .def("run_profile", &torch_ext::FusedMoeRunner::runProfile)
        .def("get_profile_ids", &torch_ext::FusedMoeRunner::getProfileIds);
}

TORCH_LIBRARY_FRAGMENT(trtllm, m)
{
    m.def(
        "fused_moe(Tensor input, Tensor token_selected_experts, "
        "Tensor? token_final_scales, Tensor fc1_expert_weights, Tensor fc2_expert_weights, "
        "ScalarType output_dtype, "
        "Tensor[]? quant_scales=None, "
        "Tensor? input_sf=None, "
        "int tp_size=1, int tp_rank=0, int ep_size=1, int ep_rank=0, int[]? profile_ids=None, "
        "bool use_fp8_block_scaling=False) -> Tensor");
    m.def(
        "fused_moe_min_latency(Tensor input, Tensor token_selected_experts, "
        "Tensor? token_final_scales, Tensor fc1_expert_weights, Tensor fc2_expert_weights, "
        "ScalarType output_dtype, "
        "Tensor[]? quant_scales=None, "
        "Tensor? input_sf=None, "
        "int tp_size=1, int tp_rank=0, int ep_size=1, int ep_rank=0, int[]? profile_ids=None, "
        "bool use_fp8_block_scaling=False) -> (Tensor, Tensor, Tensor, Tensor)");
}

TORCH_LIBRARY_IMPL(trtllm, CUDA, m)
{
    m.impl("fused_moe", &torch_ext::fused_moe);
    m.impl("fused_moe_min_latency", &torch_ext::fused_moe_min_latency);
}
