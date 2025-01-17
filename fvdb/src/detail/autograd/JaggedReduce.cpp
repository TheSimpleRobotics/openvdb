// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0
//
#include "JaggedReduce.h"

#include <detail/ops/jagged/JaggedOps.h>
#include <detail/utils/Utils.h>

#include <nanovdb/NanoVDB.h>

namespace fvdb {
namespace detail {
namespace autograd {

static inline std::vector<int64_t>
list2vec(const c10::List<int64_t> list) {
    std::vector<int64_t> result;
    result.reserve(list.size());
    for (size_t i = 0; i < list.size(); i++)
        result.push_back(list[i]);
    return result;
}

JaggedSum::variable_list
JaggedSum::forward(JaggedSum::AutogradContext *ctx, JaggedSum::Variable jdata,
                   JaggedSum::Variable jidx, JaggedSum::Variable joffsets, int64_t dim_size) {
    TORCH_CHECK_VALUE(jdata.device() == jidx.device(), "jdata and jidx must be on the same device");
    TORCH_CHECK_VALUE(jdata.device() == joffsets.device(),
                      "jdata and joffsets must be on the same device");

    torch::Tensor outData = FVDB_DISPATCH_KERNEL_DEVICE(jdata.device(), [&]() {
        return ops::dispatchJaggedSum<DeviceTag>(jdata, jidx, joffsets, dim_size);
    });

    ctx->save_for_backward({ jidx });
    return variable_list({ outData });
}

JaggedSum::variable_list
JaggedSum::backward(JaggedSum::AutogradContext *ctx, JaggedSum::variable_list grad_output) {
    variable_list saved  = ctx->get_saved_variables();
    Variable      jidx   = saved.at(0);
    Variable      gradIn = grad_output.at(0).index({ jidx.to(torch::kInt32) });
    return { gradIn, torch::Tensor(), torch::Tensor(), torch::Tensor() };
}

JaggedMin::variable_list
JaggedMin::forward(JaggedMin::AutogradContext *ctx, JaggedMin::Variable jdata,
                   JaggedMin::Variable jidx, JaggedMin::Variable joffsets, int64_t dim_size) {
    TORCH_CHECK_VALUE(jdata.device() == jidx.device(), "jdata and jidx must be on the same device");
    TORCH_CHECK_VALUE(jdata.device() == joffsets.device(),
                      "jdata and joffsets must be on the same device");

    auto          minOut  = FVDB_DISPATCH_KERNEL_DEVICE(jdata.device(), [&]() {
        return ops::dispatchJaggedMin<DeviceTag>(jdata, jidx, joffsets, dim_size);
    });
    torch::Tensor minData = minOut[0];
    torch::Tensor minIdx  = minOut[1];
    ctx->save_for_backward({ minIdx, joffsets });
    ctx->saved_data["src_shape"] = jdata.sizes();
    return variable_list({ minData, minIdx });
}

JaggedMin::variable_list
JaggedMin::backward(JaggedMin::AutogradContext *ctx, JaggedMin::variable_list grad_output) {
    variable_list saved     = ctx->get_saved_variables();
    Variable      gradOut   = grad_output.at(0);
    Variable      minIdx    = saved.at(0);
    Variable      joffsets0 = saved.at(1).index({ torch::indexing::Slice(0, -1) });
    for (int i = 0; i < minIdx.dim() - 1; i += 1) {
        joffsets0 = joffsets0.unsqueeze(-1);
    }

    auto src_shape = list2vec(ctx->saved_data["src_shape"].toIntList());

    // For output that receives no input, propagate to position -1 will result in memory
    // out-of-bound error.
    //  Therefore, we need to add a dummy zero at the beginning of the index tensor.
    // src_shape[0] += 1;
    Variable gradIn = torch::zeros(src_shape, gradOut.options());
    gradIn.scatter_(0, minIdx + joffsets0, gradOut);
    // gradIn = gradIn.narrow(0, 1, src_shape[0] - 1);
    return { gradIn, torch::Tensor(), torch::Tensor(), torch::Tensor() };
}

JaggedMax::variable_list
JaggedMax::forward(JaggedMax::AutogradContext *ctx, JaggedMax::Variable jdata,
                   JaggedMax::Variable jidx, JaggedMax::Variable joffsets, int64_t dim_size) {
    TORCH_CHECK_VALUE(jdata.device() == jidx.device(), "jdata and jidx must be on the same device");
    TORCH_CHECK_VALUE(jdata.device() == joffsets.device(),
                      "jdata and joffsets must be on the same device");

    auto          maxOut  = FVDB_DISPATCH_KERNEL_DEVICE(jdata.device(), [&]() {
        return ops::dispatchJaggedMax<DeviceTag>(jdata, jidx, joffsets, dim_size);
    });
    torch::Tensor maxData = maxOut[0];
    torch::Tensor maxIdx  = maxOut[1];

    ctx->save_for_backward({ maxIdx, joffsets });
    ctx->saved_data["src_shape"] = jdata.sizes();
    return variable_list({ maxData, maxIdx });
}

JaggedMax::variable_list
JaggedMax::backward(JaggedMax::AutogradContext *ctx, JaggedMax::variable_list grad_output) {
    variable_list saved     = ctx->get_saved_variables();
    Variable      gradOut   = grad_output.at(0);
    Variable      maxIdx    = saved.at(0);
    Variable      joffsets0 = saved.at(1).index({ torch::indexing::Slice(0, -1) });
    for (int i = 0; i < maxIdx.dim() - 1; i += 1) {
        joffsets0 = joffsets0.unsqueeze(-1);
    }
    auto src_shape = list2vec(ctx->saved_data["src_shape"].toIntList());

    // For output that receives no input, propagate to position -1 will result in memory
    // out-of-bound error.
    //  Therefore, we need to add a dummy zero at the beginning of the index tensor.
    // src_shape[0] += 1;
    Variable gradIn = torch::zeros(src_shape, gradOut.options());
    gradIn.scatter_(0, maxIdx + joffsets0, gradOut);
    // gradIn = gradIn.narrow(0, 1, src_shape[0] - 1);
    return { gradIn, torch::Tensor(), torch::Tensor(), torch::Tensor() };
}

} // namespace autograd
} // namespace detail
} // namespace fvdb