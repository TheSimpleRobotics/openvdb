// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0
//
#ifndef FVDB_DETAIL_AUTOGRAD_SPARSECONVOLUTIONHALO_H
#define FVDB_DETAIL_AUTOGRAD_SPARSECONVOLUTIONHALO_H

#include <SparseConvPackInfo.h>
#include <detail/ops/Ops.h>

#include <torch/autograd.h>

namespace fvdb {
namespace detail {
namespace autograd {

struct SparseConvolutionHalo : public torch::autograd::Function<SparseConvolutionHalo> {
    using variable_list   = torch::autograd::variable_list;
    using AutogradContext = torch::autograd::AutogradContext;
    using Variable        = torch::autograd::Variable;

    static variable_list forward(AutogradContext *ctx, c10::intrusive_ptr<GridBatchImpl> grid,
                                 Variable inFeatures, Variable kernels, int variant);

    static variable_list backward(AutogradContext *ctx, variable_list grad_output);
};

} // namespace autograd
} // namespace detail
} // namespace fvdb

#endif // FVDB_DETAIL_AUTOGRAD_SPARSECONVOLUTIONHALO_H