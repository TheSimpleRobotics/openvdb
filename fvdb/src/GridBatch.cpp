// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0
//
#include "GridBatch.h"

#include "FVDB.h"
#include "detail/GridBatchImpl.h"
#include "detail/autograd/Autograd.h"
#include "detail/build/Build.h"
#include "detail/io/IO.h"
#include "detail/ops/Ops.h"

namespace fvdb {

GridBatch::GridBatch(TorchDeviceOrString device, bool isMutable) {
    detail::RAIIDeviceGuard guard(device.value());
    mImpl = c10::make_intrusive<detail::GridBatchImpl>(device.value(), isMutable);
}

GridBatch::GridBatch() {
    mImpl = c10::make_intrusive<detail::GridBatchImpl>(
        detail::build::buildEmptyGrid(torch::kCPU, false), nanovdb::Vec3d(1.0),
        nanovdb::Vec3d(0.0));
}

std::pair<JaggedTensor, GridBatch>
GridBatch::max_pool(Vec3iOrScalar pool_factor, const JaggedTensor &data, Vec3iOrScalar stride,
                    torch::optional<GridBatch> coarse_grid) const {
    detail::RAIIDeviceGuard guard(device());
    TORCH_CHECK_VALUE(
        data.ldim() == 1,
        "Expected data to have 1 list dimension, i.e. be a single list of coordinate values, but got",
        data.ldim(), "list dimensions");

    nanovdb::Coord pool_factor_coord = pool_factor.value();
    nanovdb::Coord stride_coord      = stride.value();

    for (int i = 0; i < 3; i += 1) {
        if (stride_coord[i] == 0) {
            stride_coord[i] = pool_factor_coord[i];
        }
    }

    c10::intrusive_ptr<detail::GridBatchImpl> coarse_grid_impl;
    if (coarse_grid.has_value()) {
        coarse_grid_impl = coarse_grid.value().impl();
    } else {
        coarse_grid_impl = coarsened_grid(stride_coord).impl();
    }

    torch::Tensor pool_data = detail::autograd::MaxPoolGrid::apply(
        impl(), coarse_grid_impl, pool_factor_coord, stride_coord, data.jdata())[0];

    return std::make_pair(coarse_grid_impl->jaggedTensor(pool_data, false),
                          GridBatch(coarse_grid_impl));
}

std::pair<JaggedTensor, GridBatch>
GridBatch::avg_pool(Vec3iOrScalar pool_factor, const JaggedTensor &data, Vec3iOrScalar stride,
                    torch::optional<GridBatch> coarse_grid) const {
    detail::RAIIDeviceGuard guard(device());
    TORCH_CHECK_VALUE(
        data.ldim() == 1,
        "Expected data to have 1 list dimension, i.e. be a single list of coordinate values, but got",
        data.ldim(), "list dimensions");

    nanovdb::Coord pool_factor_coord = pool_factor.value();
    nanovdb::Coord stride_coord      = stride.value();

    for (int i = 0; i < 3; i += 1) {
        if (stride_coord[i] == 0) {
            stride_coord[i] = pool_factor_coord[i];
        }
    }

    c10::intrusive_ptr<detail::GridBatchImpl> coarse_grid_impl;
    if (coarse_grid.has_value()) {
        coarse_grid_impl = coarse_grid.value().impl();
    } else {
        coarse_grid_impl = coarsened_grid(stride_coord).impl();
    }

    torch::Tensor pool_data = detail::autograd::AvgPoolGrid::apply(
        impl(), coarse_grid_impl, pool_factor_coord, stride_coord, data.jdata())[0];

    return std::make_pair(coarse_grid_impl->jaggedTensor(pool_data, false),
                          GridBatch(coarse_grid_impl));
}

std::pair<JaggedTensor, GridBatch>
GridBatch::subdivide(Vec3iOrScalar subdiv_factor, const JaggedTensor &data,
                     const torch::optional<JaggedTensor> mask,
                     torch::optional<GridBatch>          fine_grid) const {
    detail::RAIIDeviceGuard guard(device());
    TORCH_CHECK_VALUE(
        data.ldim() == 1,
        "Expected data to have 1 list dimension, i.e. be a single list of coordinate values, but got",
        data.ldim(), "list dimensions");
    if (mask.has_value()) {
        TORCH_CHECK_VALUE(
            mask.value().ldim() == 1,
            "Expected mask to have 1 list dimension, i.e. be a single list of coordinate values, but got",
            mask.value().ldim(), "list dimensions");
    }
    const nanovdb::Coord upsampleFactorCoord = subdiv_factor.value();

    c10::intrusive_ptr<detail::GridBatchImpl> fineGrid;
    if (fine_grid.has_value()) {
        fineGrid = fine_grid.value().impl();
    } else {
        fineGrid = subdivided_grid(subdiv_factor, mask).impl();
    }

    torch::Tensor subdivData = detail::autograd::UpsampleGrid::apply(
        impl(), fineGrid, upsampleFactorCoord, data.jdata())[0];

    return std::make_pair(fineGrid->jaggedTensor(subdivData, false), GridBatch(fineGrid));
}

JaggedTensor
GridBatch::read_from_dense(const torch::Tensor &dense_data, const Vec3iBatch &dense_origins) const {
    detail::RAIIDeviceGuard guard(device());
    torch::Tensor           retData =
        detail::autograd::ReadFromDense::apply(impl(), dense_data, dense_origins)[0];
    return impl()->jaggedTensor(retData, false);
}

torch::Tensor
GridBatch::write_to_dense(const JaggedTensor                &sparse_data,
                          const torch::optional<Vec3iBatch> &min_coord,
                          const torch::optional<Vec3i>      &grid_size) const {
    detail::RAIIDeviceGuard guard(device());
    TORCH_CHECK_VALUE(
        sparse_data.ldim() == 1,
        "Expected sparse_data to have 1 list dimension, i.e. be a single list of coordinate values, but got",
        sparse_data.ldim(), "list dimensions");
    return detail::autograd::ReadIntoDense::apply(impl(), sparse_data.jdata(), min_coord,
                                                  grid_size)[0];
}

JaggedTensor
GridBatch::fill_from_grid(const JaggedTensor &other_features, const GridBatch &other_grid,
                          float default_value) const {
    detail::RAIIDeviceGuard guard(device());
    TORCH_CHECK_VALUE(
        other_features.ldim() == 1,
        "Expected features to have 1 list dimension, i.e. be a single list of coordinate values, but got",
        other_features.ldim(), "list dimensions");
    torch::Tensor retData = detail::autograd::FillFromGrid::apply(
        other_grid.impl(), impl(), other_features.jdata(), default_value)[0];

    return impl()->jaggedTensor(retData, false);
}

JaggedTensor
GridBatch::grid_to_world(const JaggedTensor &ijk) const {
    detail::RAIIDeviceGuard guard(device());
    TORCH_CHECK_VALUE(
        ijk.ldim() == 1,
        "Expected ijk to have 1 list dimension, i.e. be a single list of coordinate values, but got",
        ijk.ldim(), "list dimensions");
    torch::Tensor ret = detail::autograd::TransformPoints::apply(
        impl(), ijk, ijk.jdata(), true /*isInverse*/, false /*isDual*/)[0];

    return ijk.jagged_like(ret);
}

JaggedTensor
GridBatch::world_to_grid(const JaggedTensor &points) const {
    detail::RAIIDeviceGuard guard(device());
    TORCH_CHECK_VALUE(
        points.ldim() == 1,
        "Expected points to have 1 list dimension, i.e. be a single list of coordinate values, but got",
        points.ldim(), "list dimensions");
    torch::Tensor ret = detail::autograd::TransformPoints::apply(
        impl(), points, points.jdata(), false /* isInverse*/, false /*isDual*/)[0];

    return points.jagged_like(ret);
}

torch::Tensor
GridBatch::grid_to_world_matrices(const torch::Dtype &dtype) const {
    detail::RAIIDeviceGuard    guard(device());
    std::vector<torch::Tensor> retTorch;
    for (int64_t bi = 0; bi < grid_count(); ++bi) {
        retTorch.emplace_back(impl()->gridToWorldMatrix(bi));
    }

    return torch::stack(retTorch, 0).toType(dtype);
}

torch::Tensor
GridBatch::world_to_grid_matrices(const torch::Dtype &dtype) const {
    detail::RAIIDeviceGuard    guard(device());
    std::vector<torch::Tensor> retTorch;
    for (int64_t bi = 0; bi < grid_count(); ++bi) {
        retTorch.emplace_back(impl()->worldToGridMatrix(bi));
    }

    return torch::stack(retTorch, 0).toType(dtype);
}

JaggedTensor
GridBatch::sample_trilinear(const JaggedTensor &points, const JaggedTensor &voxel_data) const {
    detail::RAIIDeviceGuard guard(device());
    TORCH_CHECK_VALUE(
        points.ldim() == 1,
        "Expected points to have 1 list dimension, i.e. be a single list of coordinate values, but got",
        points.ldim(), "list dimensions");
    TORCH_CHECK_VALUE(
        voxel_data.ldim() == 1,
        "Expected voxel_data to have 1 list dimension, i.e. be a single list of coordinate values, but got",
        voxel_data.ldim(), "list dimensions");
    torch::Tensor ret = detail::autograd::SampleGridTrilinear::apply(
        impl(), points, voxel_data.jdata(), false /*returnGrad*/)[0];
    return points.jagged_like(ret);
}

std::vector<JaggedTensor>
GridBatch::sample_trilinear_with_grad(const JaggedTensor &points,
                                      const JaggedTensor &voxel_data) const {
    detail::RAIIDeviceGuard guard(device());
    TORCH_CHECK_VALUE(
        points.ldim() == 1,
        "Expected points to have 1 list dimension, i.e. be a single list of coordinate values, but got",
        points.ldim(), "list dimensions");
    TORCH_CHECK_VALUE(
        voxel_data.ldim() == 1,
        "Expected voxel_data to have 1 list dimension, i.e. be a single list of coordinate values, but got",
        voxel_data.ldim(), "list dimensions");
    std::vector<torch::Tensor> ret = detail::autograd::SampleGridTrilinear::apply(
        impl(), points, voxel_data.jdata(), true /*returnGrad*/);

    return { points.jagged_like(ret[0]), points.jagged_like(ret[1]) };
}

JaggedTensor
GridBatch::sample_bezier(const JaggedTensor &points, const JaggedTensor &voxel_data) const {
    detail::RAIIDeviceGuard guard(device());
    TORCH_CHECK_VALUE(
        points.ldim() == 1,
        "Expected points to have 1 list dimension, i.e. be a single list of coordinate values, but got",
        points.ldim(), "list dimensions");
    TORCH_CHECK_VALUE(
        voxel_data.ldim() == 1,
        "Expected voxel_data to have 1 list dimension, i.e. be a single list of coordinate values, but got",
        voxel_data.ldim(), "list dimensions");
    torch::Tensor ret = detail::autograd::SampleGridBezier::apply(
        impl(), points, voxel_data.jdata(), false /*returnGrad*/)[0];
    return points.jagged_like(ret);
}

std::vector<JaggedTensor>
GridBatch::sample_bezier_with_grad(const JaggedTensor &points,
                                   const JaggedTensor &voxel_data) const {
    detail::RAIIDeviceGuard guard(device());
    TORCH_CHECK_VALUE(
        points.ldim() == 1,
        "Expected points to have 1 list dimension, i.e. be a single list of coordinate values, but got",
        points.ldim(), "list dimensions");
    TORCH_CHECK_VALUE(
        voxel_data.ldim() == 1,
        "Expected voxel_data to have 1 list dimension, i.e. be a single list of coordinate values, but got",
        voxel_data.ldim(), "list dimensions");
    auto ret = detail::autograd::SampleGridBezier::apply(impl(), points, voxel_data.jdata(),
                                                         true /*returnGrad*/);
    return { points.jagged_like(ret[0]), points.jagged_like(ret[1]) };
}

JaggedTensor
GridBatch::splat_trilinear(const JaggedTensor &points, const JaggedTensor &points_data) const {
    detail::RAIIDeviceGuard guard(device());
    TORCH_CHECK_VALUE(
        points.ldim() == 1,
        "Expected points to have 1 list dimension, i.e. be a single list of coordinate values, but got",
        points.ldim(), "list dimensions");
    TORCH_CHECK_VALUE(
        points_data.ldim() == 1,
        "Expected points_data to have 1 list dimension, i.e. be a single list of coordinate values, but got",
        points_data.ldim(), "list dimensions");
    torch::Tensor ret =
        detail::autograd::SplatIntoGridTrilinear::apply(impl(), points, points_data.jdata())[0];
    if (grid_count() == 1) {
        return JaggedTensor(ret);
    } else {
        return impl()->jaggedTensor(ret, true);
    }
}

JaggedTensor
GridBatch::splat_bezier(const JaggedTensor &points, const JaggedTensor &points_data) const {
    detail::RAIIDeviceGuard guard(device());
    TORCH_CHECK_VALUE(
        points.ldim() == 1,
        "Expected points to have 1 list dimension, i.e. be a single list of coordinate values, but got",
        points.ldim(), "list dimensions");
    TORCH_CHECK_VALUE(
        points_data.ldim() == 1,
        "Expected points_data to have 1 list dimension, i.e. be a single list of coordinate values, but got",
        points_data.ldim(), "list dimensions");
    torch::Tensor ret =
        detail::autograd::SplatIntoGridBezier::apply(impl(), points, points_data.jdata())[0];
    if (grid_count() == 1) {
        return JaggedTensor(ret);
    } else {
        return impl()->jaggedTensor(ret, true);
    }
}

torch::Tensor
GridBatch::voxel_size_at(int64_t bi, const torch::Dtype &dtype) const {
    detail::RAIIDeviceGuard guard(device());
    torch::Tensor           retTorch =
        torch::empty({ 3 }, torch::TensorOptions().device(this->device()).dtype(dtype));
    const nanovdb::Vec3d &voxSize = impl()->voxelSize(bi);
    retTorch[0]                   = voxSize[0];
    retTorch[1]                   = voxSize[1];
    retTorch[2]                   = voxSize[2];
    return retTorch;
}

torch::Tensor
GridBatch::voxel_sizes(const torch::Dtype &dtype) const {
    detail::RAIIDeviceGuard guard(device());
    torch::Tensor           retTorch = torch::empty(
        { grid_count(), 3 }, torch::TensorOptions().device(this->device()).dtype(dtype));
    for (int64_t bi = 0; bi < grid_count(); bi += 1) {
        const nanovdb::Vec3d voxSize = impl()->voxelSize(bi);
        retTorch[bi][0]              = voxSize[0];
        retTorch[bi][1]              = voxSize[1];
        retTorch[bi][2]              = voxSize[2];
    }
    return retTorch;
}

torch::Tensor
GridBatch::origin_at(int64_t bi, const torch::Dtype &dtype) const {
    detail::RAIIDeviceGuard guard(device());
    const nanovdb::Vec3d   &voxelOrigin = impl()->voxelOrigin(bi);
    torch::Tensor           retTorch =
        torch::empty({ 3 }, torch::TensorOptions().device(this->device()).dtype(dtype));
    retTorch[0] = voxelOrigin[0];
    retTorch[1] = voxelOrigin[1];
    retTorch[2] = voxelOrigin[2];
    return retTorch;
}

torch::Tensor
GridBatch::origins(const torch::Dtype &dtype) const {
    detail::RAIIDeviceGuard guard(device());
    torch::Tensor           retTorch = torch::empty(
        { grid_count(), 3 }, torch::TensorOptions().device(this->device()).dtype(dtype));
    for (int64_t bi = 0; bi < grid_count(); bi += 1) {
        const nanovdb::Vec3d &voxOrigin = impl()->voxelOrigin(bi);
        retTorch[bi][0]                 = voxOrigin[0];
        retTorch[bi][1]                 = voxOrigin[1];
        retTorch[bi][2]                 = voxOrigin[2];
    }
    return retTorch;
}

torch::Tensor
GridBatch::num_voxels() const {
    detail::RAIIDeviceGuard guard(device());
    torch::Tensor           retTorch = torch::empty(
        { grid_count() }, torch::TensorOptions().device(torch::kCPU).dtype(torch::kInt64));
    auto acc = retTorch.accessor<int64_t, 1>();

    for (int64_t bi = 0; bi < grid_count(); bi += 1) {
        acc[bi] = num_voxels_at(bi);
    }
    return retTorch.to(device());
}

torch::Tensor
GridBatch::num_enabled_voxels() const {
    detail::RAIIDeviceGuard guard(device());
    if (!is_mutable()) {
        return num_voxels();
    }
    torch::Tensor retTorch = torch::empty(
        { grid_count() }, torch::TensorOptions().device(torch::kCPU).dtype(torch::kInt64));
    auto acc = retTorch.accessor<int64_t, 1>();

    for (int64_t bi = 0; bi < grid_count(); bi += 1) {
        acc[bi] = num_enabled_voxels_at(bi);
    }
    return retTorch.to(device());
}

int64_t
GridBatch::num_enabled_voxels_at(int64_t bi) const {
    detail::RAIIDeviceGuard guard(device());
    if (!is_mutable()) {
        return num_voxels_at(bi);
    }
    return FVDB_DISPATCH_KERNEL_DEVICE(device(), [&]() {
        return fvdb::detail::ops::dispatchCountEnabledVoxels<DeviceTag>(*impl(), bi);
    });
}

torch::Tensor
GridBatch::cum_voxels() const {
    detail::RAIIDeviceGuard guard(device());
    torch::Tensor           retTorch = torch::empty(
        { grid_count() }, torch::TensorOptions().device(torch::kCPU).dtype(torch::kInt64));
    auto acc = retTorch.accessor<int64_t, 1>();

    for (int64_t bi = 0; bi < grid_count(); bi += 1) {
        acc[bi] = cum_voxels_at(bi);
    }
    return retTorch.to(device());
}

torch::Tensor
GridBatch::cum_enabled_voxels() const {
    detail::RAIIDeviceGuard guard(device());
    if (!is_mutable()) {
        return cum_voxels();
    }
    torch::Tensor retTorch = torch::empty(
        { grid_count() }, torch::TensorOptions().device(torch::kCPU).dtype(torch::kInt64));
    auto acc = retTorch.accessor<int64_t, 1>();

    for (int64_t bi = 0; bi < grid_count(); bi += 1) {
        acc[bi] = cum_enabled_voxels_at(bi);
    }
    return retTorch.to(device());
}

int64_t
GridBatch::cum_enabled_voxels_at(int64_t bi) const {
    detail::RAIIDeviceGuard guard(device());
    int64_t                 nCum = 0;
    for (int64_t b = 0; b < bi; ++b) {
        nCum += num_enabled_voxels_at(b);
    }
    return nCum;
}

torch::Tensor
GridBatch::num_bytes() const {
    detail::RAIIDeviceGuard guard(device());
    torch::Tensor           retTorch = torch::empty(
        { grid_count() }, torch::TensorOptions().device(torch::kCPU).dtype(torch::kInt64));
    auto acc = retTorch.accessor<int64_t, 1>();

    for (int64_t bi = 0; bi < grid_count(); bi += 1) {
        acc[bi] = impl()->numBytes(bi);
    }
    return retTorch.to(device());
}

torch::Tensor
GridBatch::num_leaf_nodes() const {
    detail::RAIIDeviceGuard guard(device());
    torch::Tensor           retTorch = torch::empty(
        { grid_count() }, torch::TensorOptions().device(torch::kCPU).dtype(torch::kInt64));
    auto acc = retTorch.accessor<int64_t, 1>();

    for (int64_t bi = 0; bi < grid_count(); bi += 1) {
        acc[bi] = impl()->numLeaves(bi);
    }
    return retTorch.to(device());
}

void
GridBatch::disable_ijk(const JaggedTensor &ijk) {
    detail::RAIIDeviceGuard guard(device());
    TORCH_CHECK_VALUE(
        ijk.ldim() == 1,
        "Expected ijk to have 1 list dimension, i.e. be a single list of coordinate values, but got",
        ijk.ldim(), "list dimensions");
    FVDB_DISPATCH_KERNEL_DEVICE(device(), [&]() {
        fvdb::detail::ops::dispatchSetMaskedIjk<DeviceTag>(*impl(), ijk, false);
    });
}

void
GridBatch::enable_ijk(const JaggedTensor &ijk) {
    detail::RAIIDeviceGuard guard(device());
    TORCH_CHECK_VALUE(
        ijk.ldim() == 1,
        "Expected ijk to have 1 list dimension, i.e. be a single list of coordinate values, but got",
        ijk.ldim(), "list dimensions");
    FVDB_DISPATCH_KERNEL_DEVICE(device(), [&]() {
        fvdb::detail::ops::dispatchSetMaskedIjk<DeviceTag>(*impl(), ijk, true);
    });
}

void
GridBatch::set_from_mesh(const JaggedTensor &mesh_vertices, const JaggedTensor &mesh_faces,
                         const Vec3dBatchOrScalar &voxel_sizes, const Vec3dBatch &origins) {
    detail::RAIIDeviceGuard guard(device());
    TORCH_CHECK_VALUE(
        mesh_vertices.ldim() == 1,
        "Expected mesh_vertices to have 1 list dimension, i.e. be a single list of coordinate values, but got",
        mesh_vertices.ldim(), "list dimensions");
    TORCH_CHECK_VALUE(
        mesh_faces.ldim() == 1,
        "Expected mesh_faces to have 1 list dimension, i.e. be a single list of coordinate values, but got",
        mesh_faces.ldim(), "list dimensions");
    TORCH_CHECK_TYPE(mesh_vertices.is_floating_point(),
                     "mesh_vertices must have a floating point type");
    TORCH_CHECK_VALUE(
        mesh_vertices.rdim() == 2,
        std::string("Expected mesh_vertices to have 2 dimensions (shape (n, 3)) but got ") +
            std::to_string(mesh_vertices.rdim()) + " dimensions");
    TORCH_CHECK_VALUE(mesh_vertices.rsize(1) == 3,
                      "Expected 3 dimensional mesh_vertices but got mesh_vertices.rshape[1] = " +
                          std::to_string(mesh_vertices.rsize(1)));

    TORCH_CHECK_TYPE(!mesh_faces.is_floating_point(), "mesh_faces must have an integer type");
    TORCH_CHECK_VALUE(
        mesh_faces.rdim() == 2,
        std::string("Expected mesh_faces to have 2 dimensions (shape (n, 3)) but got ") +
            std::to_string(mesh_faces.rdim()) + " dimensions");
    TORCH_CHECK_VALUE(mesh_faces.rsize(1) == 3,
                      "Expected 3 dimensional mesh_faces but got mesh_faces.rshape[1] = " +
                          std::to_string(mesh_faces.rsize(1)));

    TORCH_CHECK_VALUE(mesh_vertices.num_outer_lists() == mesh_faces.num_outer_lists(),
                      "Expected same number of vertex and face sets got len(mesh_vertices) = ",
                      mesh_vertices.num_outer_lists(),
                      " and len(mesh_faces) = ", mesh_faces.num_outer_lists());
    const int64_t numGrids = mesh_vertices.joffsets().size(0) - 1;
    TORCH_CHECK(numGrids == mesh_vertices.num_outer_lists(),
                "If this happens, Francis' paranoia was justified. File a bug");
    TORCH_CHECK_VALUE(numGrids <= MAX_GRIDS_PER_BATCH, "Cannot create a grid with more than ",
                      MAX_GRIDS_PER_BATCH, " grids in a batch. ", "You passed in ", numGrids,
                      " mesh sets.");

    const std::vector<nanovdb::Vec3d> voxSizesVec =
        voxel_sizes.value(numGrids, true /* onlyPositive */, "voxel_sizes");
    const std::vector<nanovdb::Vec3d> voxOriginsVec =
        origins.value(numGrids, false /* onlyPositive */, "voxel_origins");

    std::vector<detail::VoxelCoordTransform> transforms;
    transforms.reserve(numGrids);
    for (int64_t i = 0; i < numGrids; i += 1) {
        transforms.push_back(
            detail::primalVoxelTransformForSizeAndOrigin(voxSizesVec[i], voxOriginsVec[i]));
    }

    mImpl = c10::make_intrusive<detail::GridBatchImpl>(
        detail::build::buildGridFromMesh(is_mutable(), mesh_vertices, mesh_faces, transforms),
        voxSizesVec, voxOriginsVec);
}

void
GridBatch::set_from_points(const JaggedTensor &points, const Vec3i &pad_min, const Vec3i &pad_max,
                           const Vec3dBatchOrScalar &voxel_sizes, const Vec3dBatch &origins) {
    detail::RAIIDeviceGuard guard(device());
    TORCH_CHECK_VALUE(
        points.ldim() == 1,
        "Expected points to have 1 list dimension, i.e. be a single list of coordinate values, but got",
        points.ldim(), "list dimensions");
    TORCH_CHECK_TYPE(points.is_floating_point(), "points must have a floating point type");
    TORCH_CHECK_VALUE(points.rdim() == 2,
                      std::string("Expected points to have 2 dimensions (shape (n, 3)) but got ") +
                          std::to_string(points.rdim()) + " dimensions");
    TORCH_CHECK_VALUE(points.rsize(1) == 3,
                      "Expected 3 dimensional points but got points.rshape[1] = " +
                          std::to_string(points.rsize(1)));
    impl()->checkDevice(points);
    TORCH_CHECK(
        points.num_tensors() == points.num_outer_lists(),
        "If this happens, Francis' paranoia about tensors and points was justified. File a bug");
    TORCH_CHECK_VALUE(points.num_outer_lists() <= MAX_GRIDS_PER_BATCH,
                      "Cannot create a grid with more than ", MAX_GRIDS_PER_BATCH,
                      " grids in a batch. ", "You passed in ", points.num_outer_lists(),
                      " points sets.");

    const nanovdb::Coord padMin = pad_min.value();
    const nanovdb::Coord padMax = pad_max.value();

    const int64_t numGrids = points.joffsets().size(0) - 1;
    TORCH_CHECK(
        numGrids == points.num_outer_lists(),
        "If this happens, Francis' paranoia about grids and points was justified. File a bug");

    const std::vector<nanovdb::Vec3d> voxSizesVec =
        voxel_sizes.value(numGrids, true /* onlyPositive */, "voxel_sizes");
    const std::vector<nanovdb::Vec3d> voxOriginsVec =
        origins.value(numGrids, false /* onlyPositive */, "voxel_origins");

    std::vector<detail::VoxelCoordTransform> transforms;
    transforms.reserve(numGrids);
    for (int64_t i = 0; i < numGrids; i += 1) {
        transforms.push_back(
            detail::primalVoxelTransformForSizeAndOrigin(voxSizesVec[i], voxOriginsVec[i]));
    }

    mImpl = c10::make_intrusive<detail::GridBatchImpl>(
        detail::build::buildPaddedGridFromPoints(is_mutable(), points, transforms, padMin, padMax),
        voxSizesVec, voxOriginsVec);
}

void
GridBatch::set_from_nearest_voxels_to_points(const JaggedTensor       &points,
                                             const Vec3dBatchOrScalar &voxel_sizes,
                                             const Vec3dBatch         &origins) {
    detail::RAIIDeviceGuard guard(device());
    TORCH_CHECK_VALUE(
        points.ldim() == 1,
        "Expected points to have 1 list dimension, i.e. be a single list of coordinate values, but got",
        points.ldim(), "list dimensions");
    TORCH_CHECK_TYPE(points.is_floating_point(), "points must have a floating point type");
    TORCH_CHECK_VALUE(points.rdim() == 2,
                      std::string("Expected points to have 2 dimensions (shape (n, 3)) but got ") +
                          std::to_string(points.rdim()) + " dimensions");
    TORCH_CHECK_VALUE(points.rsize(1) == 3,
                      "Expected 3 dimensional points but got points.shape[1] = " +
                          std::to_string(points.rsize(1)));
    impl()->checkDevice(points);
    TORCH_CHECK(points.num_tensors() == points.num_outer_lists(),
                "If this happens, Francis' paranoia was justified. File a bug");
    TORCH_CHECK_VALUE(points.num_outer_lists() <= MAX_GRIDS_PER_BATCH,
                      "Cannot create a grid with more than ", MAX_GRIDS_PER_BATCH,
                      " grids in a batch. ", "You passed in ", points.num_outer_lists(),
                      " point sets.");

    const int64_t numGrids = points.joffsets().size(0) - 1;
    TORCH_CHECK(numGrids == points.num_outer_lists(),
                "If this happens, Francis' paranoia was justified. File a bug");

    const std::vector<nanovdb::Vec3d> voxSizesVec =
        voxel_sizes.value(numGrids, true /* onlyPositive */, "voxel_sizes");
    const std::vector<nanovdb::Vec3d> voxOriginsVec =
        origins.value(numGrids, false /* onlyPositive */, "voxel_origins");

    std::vector<detail::VoxelCoordTransform> transforms;
    transforms.reserve(numGrids);
    for (int64_t i = 0; i < numGrids; i += 1) {
        transforms.push_back(
            detail::primalVoxelTransformForSizeAndOrigin(voxSizesVec[i], voxOriginsVec[i]));
    }

    mImpl = c10::make_intrusive<detail::GridBatchImpl>(
        detail::build::buildNearestNeighborGridFromPoints(is_mutable(), points, transforms),
        voxSizesVec, voxOriginsVec);
}

void
GridBatch::set_from_ijk(const JaggedTensor &coords, const Vec3i &pad_min, const Vec3i &pad_max,
                        const Vec3dBatchOrScalar &voxel_sizes, const Vec3dBatch &origins) {
    detail::RAIIDeviceGuard guard(device());
    TORCH_CHECK_VALUE(
        coords.ldim() == 1,
        "Expected coords to have 1 list dimension, i.e. be a single list of coordinate values, but got",
        coords.ldim(), "list dimensions");
    TORCH_CHECK_TYPE(at::isIntegralType(coords.scalar_type(), false),
                     "coords must have an integer type");
    TORCH_CHECK_VALUE(coords.rdim() == 2,
                      std::string("Expected points to have 2 dimensions (shape (n, 3)) but got ") +
                          std::to_string(coords.rdim()) + " dimensions");
    TORCH_CHECK_VALUE(coords.rsize(1) == 3,
                      "Expected 3 dimensional coords but got points.rshape[1] = " +
                          std::to_string(coords.rsize(1)));
    impl()->checkDevice(coords);
    TORCH_CHECK(coords.num_tensors() == coords.num_outer_lists(),
                "If this happens, Francis' paranoia was justified. File a bug");
    TORCH_CHECK_VALUE(coords.num_outer_lists() <= MAX_GRIDS_PER_BATCH,
                      "Cannot create a grid with more than ", MAX_GRIDS_PER_BATCH,
                      " grids in a batch. ", "You passed in ", coords.num_outer_lists(),
                      " coordinate sets.");

    const nanovdb::Coord &padMin = pad_min.value();
    const nanovdb::Coord &padMax = pad_max.value();

    const int64_t numGrids = coords.joffsets().size(0) - 1;
    TORCH_CHECK(numGrids == coords.num_outer_lists(),
                "If this happens, Francis' paranoia was justified. File a bug");

    const std::vector<nanovdb::Vec3d> voxSizesVec =
        voxel_sizes.value(numGrids, true /* onlyPositive */, "voxel_sizes");
    const std::vector<nanovdb::Vec3d> voxOriginsVec =
        origins.value(numGrids, false /* onlyPositive */, "voxel_origins");

    mImpl = c10::make_intrusive<detail::GridBatchImpl>(
        detail::build::buildPaddedGridFromCoords(is_mutable(), coords, padMin, padMax), voxSizesVec,
        voxOriginsVec);
}

void
GridBatch::set_from_dense_grid(const int64_t num_grids, const Vec3i &dense_dims,
                               const Vec3i &ijk_min, const Vec3dBatchOrScalar &voxel_sizes,
                               const Vec3dBatch &origins, torch::optional<torch::Tensor> mask) {
    detail::RAIIDeviceGuard guard(device());
    TORCH_CHECK_VALUE(num_grids >= 0, "num_grids must be non-negative");

    const nanovdb::Coord &size = dense_dims.value();

    const nanovdb::Coord &ijk_min_value = ijk_min.value();

    if (mask.has_value()) {
        impl()->checkDevice(mask.value());
        TORCH_CHECK_VALUE(mask.value().dtype() == torch::kBool,
                          "mask must be a boolean type or None");
        TORCH_CHECK_VALUE(mask.value().dim() == 3, "mask must be 3 dimensional");
        TORCH_CHECK_VALUE(mask.value().size(0) == size[0], "mask must have shape (w, h, d) = size");
        TORCH_CHECK_VALUE(mask.value().size(1) == size[1], "mask must have shape (w, h, d) = size");
        TORCH_CHECK_VALUE(mask.value().size(2) == size[2], "mask must have shape (w, h, d) = size");
    }

    TORCH_CHECK_VALUE(size[0] >= 0 && size[1] >= 0 && size[2] >= 0, "size must be non-negative");

    std::vector<nanovdb::Vec3d> voxSizesVec =
        voxel_sizes.value(num_grids, true /* onlyPositive */, "voxel_sizes");
    std::vector<nanovdb::Vec3d> voxOriginsVec =
        origins.value(num_grids, false /* onlyPositive */, "voxel_origins");

    TORCH_CHECK_VALUE(num_grids <= MAX_GRIDS_PER_BATCH, "Cannot create a grid with more than ",
                      MAX_GRIDS_PER_BATCH, " grids in a batch. ", "You requested ", num_grids,
                      " grids.");
    TORCH_CHECK((size_t)num_grids == voxSizesVec.size(),
                "If this happens, Francis' paranoia was justified. File a bug");
    TORCH_CHECK((size_t)num_grids == voxOriginsVec.size(),
                "If this happens, Francis' paranoia was justified. File a bug");

    mImpl = c10::make_intrusive<detail::GridBatchImpl>(
        detail::build::buildDenseGrid(device(), is_mutable(), num_grids, size, ijk_min_value, mask),
        voxSizesVec, voxOriginsVec);
}

GridBatch
GridBatch::dual_grid(bool exclude_border) const {
    detail::RAIIDeviceGuard guard(device());
    GridBatch               ret = GridBatch(device(), is_mutable());
    if (grid_count() == 0) {
        return ret;
    }
    ret.buildDualFromPrimalGrid(*this, exclude_border);
    return ret;
}

GridBatch
GridBatch::coarsened_grid(Vec3iOrScalar branch_factor) const {
    detail::RAIIDeviceGuard guard(device());
    nanovdb::Coord          branchFactorCoord = branch_factor.value();
    for (int i = 0; i < 3; i += 1) {
        TORCH_CHECK_VALUE(branchFactorCoord[i] > 0,
                          "branch_factor must be strictly positive. Got [" +
                              std::to_string(branchFactorCoord[0]) + ", " +
                              std::to_string(branchFactorCoord[1]) + ", " +
                              std::to_string(branchFactorCoord[2]) + "]");
    }
    GridBatch ret(device(), is_mutable());
    if (grid_count() == 0) {
        return ret;
    }
    ret.buildCoarseFromFineGrid(*this, branchFactorCoord);
    return ret;
}

GridBatch
GridBatch::subdivided_grid(Vec3iOrScalar                       subdiv_factor,
                           const torch::optional<JaggedTensor> mask) const {
    detail::RAIIDeviceGuard guard(device());

    if (mask.has_value()) {
        TORCH_CHECK_VALUE(
            mask.value().ldim() == 1,
            "Expected mask to have 1 list dimension, i.e. be a single list of coordinate values, but got",
            mask.value().ldim(), "list dimensions");
    }
    const nanovdb::Coord subdivFactorCoord = subdiv_factor.value();
    for (int i = 0; i < 3; i += 1) {
        TORCH_CHECK_VALUE(subdivFactorCoord[i] > 0,
                          "subdiv_factor must be strictly positive. Got [" +
                              std::to_string(subdivFactorCoord[0]) + ", " +
                              std::to_string(subdivFactorCoord[1]) + ", " +
                              std::to_string(subdivFactorCoord[2]) + "]");
    }

    GridBatch ret = GridBatch(device(), is_mutable());
    if (grid_count() == 0) {
        return ret;
    }
    ret.buildFineFromCoarseGrid(*this, mask, subdivFactorCoord);
    return ret;
}

GridBatch
GridBatch::clipped_grid(const Vec3iBatch &ijk_min, const Vec3iBatch &ijk_max) const {
    detail::RAIIDeviceGuard guard(device());
    JaggedTensor            activeVoxelMask = FVDB_DISPATCH_KERNEL_DEVICE(device(), [&]() {
        return fvdb::detail::ops::dispatchActiveVoxelsInBoundsMask<DeviceTag>(*impl(), ijk_min,
                                                                                         ijk_max, false);
    });

    JaggedTensor activeVoxelCoords = FVDB_DISPATCH_KERNEL_DEVICE(device(), [&]() {
        return fvdb::detail::ops::dispatchActiveGridCoords<DeviceTag>(*impl(), false);
    });

    // active voxel coords masked by the voxels in bounds
    JaggedTensor activeVoxelMaskCoords = activeVoxelCoords.rmask(activeVoxelMask.jdata());

    // construct grid from ijk's clipped from original grid
    GridBatch clippedGrid = gridbatch_from_ijk(activeVoxelMaskCoords, Vec3i(), Vec3i(),
                                               voxel_sizes(), origins(), is_mutable());

    return clippedGrid;
}

std::pair<JaggedTensor, GridBatch>
GridBatch::clip(const JaggedTensor &features, const Vec3iBatch &ijk_min,
                const Vec3iBatch &ijk_max) const {
    detail::RAIIDeviceGuard guard(device());
    TORCH_CHECK_VALUE(
        features.ldim() == 1,
        "Expected features to have 1 list dimension, i.e. be a single list of coordinate values, but got",
        features.ldim(), "list dimensions");

    impl()->checkDevice(features);
    TORCH_CHECK(features.rsize(0) == total_voxels(), "Value count of features does not match grid");
    TORCH_CHECK(features.num_outer_lists() == grid_count(),
                "Batch size of features does not match grid.");
    TORCH_CHECK(torch::equal(features.joffsets(), impl()->voxelOffsets(false)),
                "Offsets of features does not match grid.");

    JaggedTensor activeVoxelMask = FVDB_DISPATCH_KERNEL_DEVICE(device(), [&]() {
        return fvdb::detail::ops::dispatchActiveVoxelsInBoundsMask<DeviceTag>(*impl(), ijk_min,
                                                                              ijk_max, false);
    });

    JaggedTensor activeVoxelCoords = FVDB_DISPATCH_KERNEL_DEVICE(device(), [&]() {
        return fvdb::detail::ops::dispatchActiveGridCoords<DeviceTag>(*impl(), false);
    });

    // active voxel coords masked by the voxels in bounds
    JaggedTensor activeVoxelMaskCoords = activeVoxelCoords.rmask(activeVoxelMask.jdata());

    // construct grid from ijk's clipped from original grid
    GridBatch clippedGrid = gridbatch_from_ijk(activeVoxelMaskCoords, Vec3i(), Vec3i(),
                                               voxel_sizes(), origins(), is_mutable());
    // features clipped to voxels in bounds
    JaggedTensor clippedFeatures = features.rmask(activeVoxelMask.jdata());

    return std::make_pair(clippedFeatures, clippedGrid);
}

std::vector<JaggedTensor>
GridBatch::marching_cubes(const JaggedTensor &field, double level) const {
    detail::RAIIDeviceGuard guard(device());
    TORCH_CHECK_VALUE(
        field.ldim() == 1,
        "Expected field to have 1 list dimension, i.e. be a single list of coordinate values, but got",
        field.ldim(), "list dimensions");
    TORCH_CHECK_TYPE(field.is_floating_point(), "field must have a floating point type");
    TORCH_CHECK_VALUE(field.numel() == total_voxels(), "Value count not match!");
    TORCH_CHECK_VALUE(field.num_outer_lists() == grid_count(), "Batch size not match!");
    torch::Tensor fieldJdata = field.jdata();
    if (fieldJdata.dim() == 0) {
        fieldJdata = fieldJdata.unsqueeze(0);
    }
    if (fieldJdata.dim() != 1) {
        fieldJdata = fieldJdata.squeeze();
    }
    TORCH_CHECK(fieldJdata.dim() == 1,
                std::string("Expected field to have 1 effective dimension but got ") +
                    std::to_string(field.rdim()) + " dimensions");
    impl()->checkDevice(field);
    return FVDB_DISPATCH_KERNEL_DEVICE(device(), [&]() {
        return fvdb::detail::ops::dispatchMarchingCubes<DeviceTag>(*impl(), fieldJdata, level);
    });
}

JaggedTensor
GridBatch::sparse_conv_halo(const JaggedTensor &input, const torch::Tensor &weight,
                            int variant) const {
    detail::RAIIDeviceGuard guard(device());
    TORCH_CHECK_VALUE(
        input.ldim() == 1,
        "Expected input to have 1 list dimension, i.e. be a single list of coordinate values, but got",
        input.ldim(), "list dimensions");
    TORCH_CHECK_TYPE(input.is_floating_point(), "input must have a floating point type");
    TORCH_CHECK_VALUE(input.rsize(0) == total_voxels(), "Value count not match!");
    TORCH_CHECK_VALUE(input.num_outer_lists() == grid_count(), "Batch size not match!");
    impl()->checkDevice(input);
    torch::Tensor ret =
        detail::autograd::SparseConvolutionHalo::apply(impl(), input.jdata(), weight, variant)[0];
    return input.jagged_like(ret);
}

GridBatch
GridBatch::conv_grid(Vec3iOrScalar kernel_size, Vec3iOrScalar stride) const {
    detail::RAIIDeviceGuard guard(device());
    TORCH_CHECK_VALUE(Vec3iOrScalar(0).value() < kernel_size.value(),
                      "kernel_size must be strictly positive. Got " + kernel_size.toString());
    TORCH_CHECK_VALUE(Vec3iOrScalar(0).value() < stride.value(),
                      "stride must be strictly positive. Got " + stride.toString());
    GridBatch ret = GridBatch(device(), is_mutable());
    if (grid_count() == 0) {
        return ret;
    }
    std::vector<nanovdb::Vec3d> voxS, voxO;
    impl()->gridVoxelSizesAndOrigins(voxS, voxO);
    ret.mImpl = c10::make_intrusive<detail::GridBatchImpl>(
        detail::build::buildConvGridFromGrid(ret.is_mutable(), *impl(), kernel_size.value(),
                                             stride.value()),
        voxS, voxO);
    ret.impl()->setCoarseTransformFromFineGrid(
        *impl(), nanovdb::Coord(stride.value().x(), stride.value().y(), stride.value().z()));
    return ret;
}

void
GridBatch::buildCoarseFromFineGrid(const GridBatch &fineGrid, nanovdb::Coord branchFactor) {
    detail::RAIIDeviceGuard     guard(device());
    std::vector<nanovdb::Vec3d> voxS, voxO;
    fineGrid.impl()->gridVoxelSizesAndOrigins(voxS, voxO);
    mImpl = c10::make_intrusive<detail::GridBatchImpl>(
        detail::build::buildCoarseGridFromFineGrid(is_mutable(), *fineGrid.impl(), branchFactor),
        voxS, voxO);
    impl()->setCoarseTransformFromFineGrid(*fineGrid.impl(), branchFactor);
}

void
GridBatch::buildFineFromCoarseGrid(const GridBatch                     &coarseGrid,
                                   const torch::optional<JaggedTensor> &subdivMask,
                                   nanovdb::Coord                       subdivFactor) {
    detail::RAIIDeviceGuard guard(device());
    if (subdivMask.has_value()) {
        TORCH_CHECK_VALUE(
            subdivMask.value().ldim() == 1,
            "Expected subdiv_mask to have 1 list dimension, i.e. be a single list of coordinate values, but got",
            subdivMask.value().ldim(), "list dimensions");
        impl()->checkDevice(subdivMask.value());
        TORCH_CHECK(subdivMask.value().jdata().sizes().size() == 1,
                    "subdivision mask must have 1 dimension");
        TORCH_CHECK(subdivMask.value().jdata().size(0) == coarseGrid.total_voxels(),
                    "subdivision mask must be either empty tensor or have one entry per voxel");
        TORCH_CHECK(subdivMask.value().scalar_type() == torch::kBool,
                    "subdivision mask must be a boolean tensor");
    }

    std::vector<nanovdb::Vec3d> voxS, voxO;
    coarseGrid.impl()->gridVoxelSizesAndOrigins(voxS, voxO);
    mImpl = c10::make_intrusive<detail::GridBatchImpl>(
        detail::build::buildFineGridFromCoarseGrid(is_mutable(), *coarseGrid.impl(), subdivMask,
                                                   subdivFactor),
        voxS, voxO);
    impl()->setFineTransformFromCoarseGrid(*coarseGrid.impl(), subdivFactor);
}

void
GridBatch::buildDualFromPrimalGrid(const GridBatch &primalGrid, bool excludeBorder) {
    detail::RAIIDeviceGuard     guard(device());
    std::vector<nanovdb::Vec3d> voxS, voxO;
    primalGrid.impl()->gridVoxelSizesAndOrigins(voxS, voxO);
    mImpl = c10::make_intrusive<detail::GridBatchImpl>(
        detail::build::buildPaddedGridFromGrid(is_mutable(), *primalGrid.impl(), 0, 1,
                                               excludeBorder),
        voxS, voxO);
    impl()->setPrimalTransformFromDualGrid(*primalGrid.impl());
}

std::vector<JaggedTensor>
GridBatch::voxels_along_rays(const JaggedTensor &ray_origins, const JaggedTensor &ray_directions,
                             int64_t max_vox, double eps, bool return_ijk, bool cumulative) const {
    detail::RAIIDeviceGuard guard(device());
    TORCH_CHECK_VALUE(
        ray_origins.ldim() == 1,
        "Expected ray_origins to have 1 list dimension, i.e. be a single list of coordinate values, but got",
        ray_origins.ldim(), "list dimensions");
    TORCH_CHECK_VALUE(
        ray_directions.ldim() == 1,
        "Expected ray_directions to have 1 list dimension, i.e. be a single list of coordinate values, but got",
        ray_directions.ldim(), "list dimensions");
    return FVDB_DISPATCH_KERNEL_DEVICE(device(), [&]() {
        return fvdb::detail::ops::dispatchVoxelsAlongRays<DeviceTag>(
            *impl(), ray_origins, ray_directions, max_vox, eps, return_ijk, cumulative);
    });
}

JaggedTensor
GridBatch::segments_along_rays(const JaggedTensor &ray_origins, const JaggedTensor &ray_directions,
                               int64_t max_segments, double eps, bool ignore_masked) const {
    detail::RAIIDeviceGuard guard(device());
    TORCH_CHECK_VALUE(
        ray_origins.ldim() == 1,
        "Expected ray_origins to have 1 list dimension, i.e. be a single list of coordinate values, but got",
        ray_origins.ldim(), "list dimensions");
    TORCH_CHECK_VALUE(
        ray_directions.ldim() == 1,
        "Expected ray_directions to have 1 list dimension, i.e. be a single list of coordinate values, but got",
        ray_directions.ldim(), "list dimensions");
    return FVDB_DISPATCH_KERNEL_DEVICE(device(), [&]() {
        return fvdb::detail::ops::dispatchSegmentsAlongRays<DeviceTag>(
            *impl(), ray_origins, ray_directions, max_segments, eps, ignore_masked);
    });
}

JaggedTensor
GridBatch::ray_implicit_intersection(const JaggedTensor &ray_origins,
                                     const JaggedTensor &ray_directions,
                                     const JaggedTensor &gridScalars, double eps) const {
    detail::RAIIDeviceGuard guard(device());
    TORCH_CHECK_VALUE(
        ray_origins.ldim() == 1,
        "Expected ray_origins to have 1 list dimension, i.e. be a single list of coordinate values, but got",
        ray_origins.ldim(), "list dimensions");
    TORCH_CHECK_VALUE(
        ray_directions.ldim() == 1,
        "Expected ray_directions to have 1 list dimension, i.e. be a single list of coordinate values, but got",
        ray_directions.ldim(), "list dimensions");
    TORCH_CHECK_VALUE(
        gridScalars.ldim() == 1,
        "Expected grid_scalars to have 1 list dimension, i.e. be a single list of coordinate values, but got",
        gridScalars.ldim(), "list dimensions");
    return FVDB_DISPATCH_KERNEL_DEVICE(device(), [&]() {
        return fvdb::detail::ops::dispatchRayImplicitIntersection<DeviceTag>(
            *impl(), ray_origins, ray_directions, gridScalars, eps);
    });
}

JaggedTensor
GridBatch::uniform_ray_samples(const JaggedTensor &ray_origins, const JaggedTensor &ray_directions,
                               const JaggedTensor &t_min, const JaggedTensor &t_max,
                               double step_size, double cone_angle, bool include_end_segments,
                               bool return_midpoint, double eps) const {
    detail::RAIIDeviceGuard guard(device());
    TORCH_CHECK_VALUE(
        ray_origins.ldim() == 1,
        "Expected ray_origins to have 1 list dimension, i.e. be a single list of coordinate values, but got",
        ray_origins.ldim(), "list dimensions");
    TORCH_CHECK_VALUE(
        ray_directions.ldim() == 1,
        "Expected ray_directions to have 1 list dimension, i.e. be a single list of coordinate values, but got",
        ray_directions.ldim(), "list dimensions");
    TORCH_CHECK_VALUE(
        t_min.ldim() == 1,
        "Expected t_min to have 1 list dimension, i.e. be a single list of coordinate values, but got",
        t_min.ldim(), "list dimensions");
    TORCH_CHECK_VALUE(
        t_max.ldim() == 1,
        "Expected t_max to have 1 list dimension, i.e. be a single list of coordinate values, but got",
        t_max.ldim(), "list dimensions");
    return FVDB_DISPATCH_KERNEL_DEVICE(device(), [&]() {
        return fvdb::detail::ops::dispatchUniformRaySamples<DeviceTag>(
            *impl(), ray_origins, ray_directions, t_min, t_max, step_size, cone_angle,
            include_end_segments, return_midpoint, eps);
    });
}

JaggedTensor
GridBatch::neighbor_indexes(const JaggedTensor &ijk, int32_t extent, int32_t bitshift) const {
    detail::RAIIDeviceGuard guard(device());
    TORCH_CHECK_VALUE(
        ijk.ldim() == 1,
        "Expected ijk to have 1 list dimension, i.e. be a single list of coordinate values, but got",
        ijk.ldim(), "list dimensions");
    TORCH_CHECK_VALUE(extent >= 0, "extent must be >= 0");
    nanovdb::Coord extentMin(-extent, -extent, -extent);
    nanovdb::Coord extentMax(extent, extent, extent);
    return FVDB_DISPATCH_KERNEL_DEVICE(device(), [&]() {
        return fvdb::detail::ops::dispatchVoxelNeighborhood<DeviceTag>(*impl(), ijk, extentMin,
                                                                       extentMax, bitshift);
    });
}

JaggedTensor
GridBatch::points_in_active_voxel(const JaggedTensor &points, bool ignore_disabled) const {
    detail::RAIIDeviceGuard guard(device());
    TORCH_CHECK_VALUE(
        points.ldim() == 1,
        "Expected points to have 1 list dimension, i.e. be a single list of coordinate values, but got",
        points.ldim(), "list dimensions");
    return FVDB_DISPATCH_KERNEL_DEVICE(device(), [&]() {
        return fvdb::detail::ops::dispatchPointsInGrid<DeviceTag>(*impl(), points, ignore_disabled);
    });
}

JaggedTensor
GridBatch::cubes_intersect_grid(const JaggedTensor &cube_centers, const Vec3dOrScalar &cube_min,
                                const Vec3dOrScalar &cube_max, bool ignore_disabled) const {
    detail::RAIIDeviceGuard guard(device());
    TORCH_CHECK_VALUE(
        cube_centers.ldim() == 1,
        "Expected cube_centers to have 1 list dimension, i.e. be a single list of coordinate values, but got",
        cube_centers.ldim(), "list dimensions");
    return FVDB_DISPATCH_KERNEL_DEVICE(device(), [&]() {
        return fvdb::detail::ops::dispatchCubesIntersectGrid<DeviceTag>(
            *impl(), cube_centers, cube_min, cube_max, ignore_disabled);
    });
}

JaggedTensor
GridBatch::cubes_in_grid(const JaggedTensor &cube_centers, const Vec3dOrScalar &cube_min,
                         const Vec3dOrScalar &cube_max, bool ignore_disabled) const {
    detail::RAIIDeviceGuard guard(device());
    TORCH_CHECK_VALUE(
        cube_centers.ldim() == 1,
        "Expected cube_centers to have 1 list dimension, i.e. be a single list of coordinate values, but got",
        cube_centers.ldim(), "list dimensions");
    return FVDB_DISPATCH_KERNEL_DEVICE(device(), [&]() {
        return fvdb::detail::ops::dispatchCubesInGrid<DeviceTag>(*impl(), cube_centers, cube_min,
                                                                 cube_max, ignore_disabled);
    });
}

JaggedTensor
GridBatch::enabled_mask() const {
    detail::RAIIDeviceGuard guard(device());
    return FVDB_DISPATCH_KERNEL_DEVICE(device(), [&]() {
        return fvdb::detail::ops::dispatchEnabledMask<DeviceTag>(*impl(), false);
    });
}

JaggedTensor
GridBatch::disabled_mask() const {
    detail::RAIIDeviceGuard guard(device());
    return FVDB_DISPATCH_KERNEL_DEVICE(device(), [&]() {
        return fvdb::detail::ops::dispatchEnabledMask<DeviceTag>(*impl(), true);
    });
}

JaggedTensor
GridBatch::coords_in_active_voxel(const JaggedTensor &ijk, bool ignore_disabled) const {
    detail::RAIIDeviceGuard guard(device());
    TORCH_CHECK_VALUE(
        ijk.ldim() == 1,
        "Expected ijk to have 1 list dimension, i.e. be a single list of coordinate values, but got",
        ijk.ldim(), "list dimensions");
    return FVDB_DISPATCH_KERNEL_DEVICE(device(), [&]() {
        return fvdb::detail::ops::dispatchCoordsInGrid<DeviceTag>(*impl(), ijk, ignore_disabled);
    });
}

JaggedTensor
GridBatch::ijk_to_index(const JaggedTensor &ijk, bool cumulative) const {
    detail::RAIIDeviceGuard guard(device());
    TORCH_CHECK_VALUE(
        ijk.ldim() == 1,
        "Expected ijk to have 1 list dimension, i.e. be a single list of coordinate values, but got",
        ijk.ldim(), "list dimensions");
    return FVDB_DISPATCH_KERNEL_DEVICE(device(), [&]() {
        return fvdb::detail::ops::dispatchIjkToIndex<DeviceTag>(*impl(), ijk, cumulative);
    });
}

JaggedTensor
GridBatch::ijk_to_inv_index(const JaggedTensor &ijk, bool cumulative) const {
    detail::RAIIDeviceGuard guard(device());
    TORCH_CHECK_VALUE(
        ijk.ldim() == 1,
        "Expected ijk to have 1 list dimension, i.e. be a single list of coordinate values, but got",
        ijk.ldim(), "list dimensions");
    return FVDB_DISPATCH_KERNEL_DEVICE(device(), [&]() {
        return fvdb::detail::ops::dispatchIjkToInvIndex<DeviceTag>(*impl(), ijk, cumulative);
    });
}

JaggedTensor
GridBatch::ijk() const {
    detail::RAIIDeviceGuard guard(device());
    return FVDB_DISPATCH_KERNEL_DEVICE(this->device(), [&]() {
        return fvdb::detail::ops::dispatchActiveGridCoords<DeviceTag>(*impl(), true);
    });
}

JaggedTensor
GridBatch::ijk_enabled() const {
    detail::RAIIDeviceGuard guard(device());
    return FVDB_DISPATCH_KERNEL_DEVICE(this->device(), [&]() {
        return fvdb::detail::ops::dispatchActiveGridCoords<DeviceTag>(*impl(), false);
    });
}

const torch::Tensor
GridBatch::bbox() const {
    detail::RAIIDeviceGuard guard(device());
    const int64_t           bs = grid_count();
    torch::Tensor           ret =
        torch::zeros({ bs, 2, 3 }, torch::TensorOptions().device(device()).dtype(torch::kInt32));
    for (int64_t i = 0; i < bs; ++i) {
        const nanovdb::CoordBBox &bbox = impl()->bbox(i);
        ret[i][0][0]                   = bbox.min()[0];
        ret[i][0][1]                   = bbox.min()[1];
        ret[i][0][2]                   = bbox.min()[2];
        ret[i][1][0]                   = bbox.max()[0];
        ret[i][1][1]                   = bbox.max()[1];
        ret[i][1][2]                   = bbox.max()[2];
    }
    return ret;
}

const torch::Tensor
GridBatch::bbox_at(int64_t bi) const {
    detail::RAIIDeviceGuard guard(device());
    torch::Tensor           ret =
        torch::zeros({ 2, 3 }, torch::TensorOptions().device(device()).dtype(torch::kInt32));
    const nanovdb::CoordBBox &bbox = impl()->bbox(bi);
    ret[0][0]                      = bbox.min()[0];
    ret[0][1]                      = bbox.min()[1];
    ret[0][2]                      = bbox.min()[2];
    ret[1][0]                      = bbox.max()[0];
    ret[1][1]                      = bbox.max()[1];
    ret[1][2]                      = bbox.max()[2];
    return ret;
}

const torch::Tensor
GridBatch::dual_bbox() const {
    detail::RAIIDeviceGuard guard(device());
    const int64_t           bs = grid_count();
    torch::Tensor           ret =
        torch::zeros({ bs, 2, 3 }, torch::TensorOptions().device(device()).dtype(torch::kInt32));
    for (int64_t i = 0; i < bs; ++i) {
        const nanovdb::CoordBBox &bbox = impl()->dualBbox(i);
        ret[i][0][0]                   = bbox.min()[0];
        ret[i][0][1]                   = bbox.min()[1];
        ret[i][0][2]                   = bbox.min()[2];
        ret[i][1][0]                   = bbox.max()[0];
        ret[i][1][1]                   = bbox.max()[1];
        ret[i][1][2]                   = bbox.max()[2];
    }
    return ret;
}

const torch::Tensor
GridBatch::dual_bbox_at(int64_t bi) const {
    detail::RAIIDeviceGuard guard(device());
    torch::Tensor           ret =
        torch::zeros({ 2, 3 }, torch::TensorOptions().device(device()).dtype(torch::kInt32));
    const nanovdb::CoordBBox &bbox = impl()->dualBbox(bi);
    ret[0][0]                      = bbox.min()[0];
    ret[0][1]                      = bbox.min()[1];
    ret[0][2]                      = bbox.min()[2];
    ret[1][0]                      = bbox.max()[0];
    ret[1][1]                      = bbox.max()[1];
    ret[1][2]                      = bbox.max()[2];
    return ret;
}

const torch::Tensor
GridBatch::total_bbox() const {
    detail::RAIIDeviceGuard   guard(device());
    const nanovdb::CoordBBox &bbox = impl()->totalBBox();
    return torch::tensor({ { bbox.min()[0], bbox.min()[1], bbox.min()[2] },
                           { bbox.max()[0], bbox.max()[1], bbox.max()[2] } },
                         torch::TensorOptions().device(device()).dtype(torch::kInt32));
}

std::vector<JaggedTensor>
GridBatch::viz_edge_network(bool returnVoxelCoordinates) const {
    detail::RAIIDeviceGuard guard(device());
    return FVDB_DISPATCH_KERNEL_DEVICE(device(), [&]() {
        return fvdb::detail::ops::dispatchGridEdgeNetwork<DeviceTag>(*impl(),
                                                                     returnVoxelCoordinates);
    });
}

} // namespace fvdb