// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0
//
#ifndef FVDB_DETAIL_GRIDBATCHIMPL_H
#define FVDB_DETAIL_GRIDBATCHIMPL_H

#include "TorchDeviceBuffer.h"
#include "VoxelCoordTransform.h"
#include "utils/Utils.h"

#include <JaggedTensor.h>

#include <nanovdb/GridHandle.h>
#include <nanovdb/NanoVDB.h>

#include <torch/all.h>

#include <vector>

#if !defined(__CUDACC__) && !defined(__restrict__)
#define __restrict__
#endif

namespace fvdb {
namespace detail {

class GridBatchImpl : public torch::CustomClassHolder {
  public:
    // Metadata about a single grid in the batch
    struct GridMetadata {
        uint32_t version = 1;    // Version of this struct

        int64_t  mCumLeaves = 0; // Cumulative number of leaf nodes in the batch up to this grid
        int64_t  mCumVoxels = 0; // Cumulative number of voxels in the batch up to this grid
        uint64_t mCumBytes = 0; // Cumulative number of bytes in the buffer of grids up to this grid
        VoxelCoordTransform mPrimalTransform; // Primal Transform of this grid (i.e. transform which
                                              // aligns origin with voxel center)
        VoxelCoordTransform mDualTransform;   // Dual Transform of this grid (i.e. transform which
                                              // aligns origin with voxel corner)
        nanovdb::Vec3d     mVoxelSize;        // Size of a single voxel in world space
        uint32_t           mNumLeaves;        // Number of leaf nodes in this grid
        int64_t            mNumVoxels;        // Number of voxels in this grid
        uint64_t           mNumBytes;         // Number of bytes in the buffer of this grid
        nanovdb::CoordBBox mBBox;             // Bounding box of this grid

        nanovdb::Vec3d
        voxelOrigin() const {
            return mPrimalTransform.applyInv<double>(0., 0., 0.);
        }

        __hostdev__ void
        setTransform(const nanovdb::Vec3d &voxSize, const nanovdb::Vec3d &voxOrigin) {
            mVoxelSize = voxSize;
            voxelTransformForSizeAndOrigin(voxSize, voxOrigin, mPrimalTransform, mDualTransform);
        }
    };

    // Metadata about the whole batch
    struct GridBatchMetadata {
        uint32_t version = 1; // Version of this struct

        // Total number of leaf nodes accross all grids
        int64_t mTotalLeaves = 0;

        // Total number of voxels accross all grids
        int64_t mTotalVoxels = 0;

        // Maximum number of voxels in any grid. Used to set thread count
        int64_t mMaxVoxels = 0;

        // Maximum number of leaf nodes in any grid. Used to set thread count
        uint32_t mMaxLeafCount = 0;

        // Bounding box enclosing all the grids in the batch
        nanovdb::CoordBBox mTotalBBox;

        // Is this a mutable grid?
        bool mIsMutable = false;

        // Is this grid contiguous
        bool mIsContiguous = true;
    };

  private:
    // Metadata for each grid in the batch. There is a seperate host and device version of these.
    // The caller of this class sets the host version and is responsible for syncing the device
    // version with the host version by calling syncMetadataToDeviceIfCUDA
    std::vector<GridMetadata> mHostGridMetadata;             // CPU only
    GridMetadata             *mDeviceGridMetadata = nullptr; // CUDA only

    GridBatchMetadata mBatchMetadata;                        // Metadata about the whole batch

    std::shared_ptr<nanovdb::GridHandle<TorchDeviceBuffer>> mGridHdl; // NanoVDB grid handle
    torch::Tensor mLeafBatchIndices; // Indices of leaf nodes in the batch shape = [total_leafs]
    torch::Tensor mBatchOffsets;     // Batch indices for grid (ignores disabled)
    torch::Tensor mListIndices; // List indices for grid (same as JaggedTensor, ignores disabled)

    // Write back changes to host metadata to the device if we're a cuda handle
    void syncMetadataToDeviceIfCUDA(bool blocking);

    inline std::pair<nanovdb::Vec3d, nanovdb::Vec3d>
    fineVoxSizeAndOrigin(int64_t bi, nanovdb::Coord subdivFactor) const {
        TORCH_CHECK(subdivFactor[0] > 0 && subdivFactor[1] > 0 && subdivFactor[2] > 0,
                    "Subdivision factor must be greater than 0");
        const nanovdb::Vec3d w = voxelSize(bi) / subdivFactor.asVec3d();
        const nanovdb::Vec3d tx =
            voxelOrigin(bi) - (subdivFactor.asVec3d() - nanovdb::Vec3d(1.0)) * w * 0.5;
        return std::make_pair(w, tx);
    }

    inline std::pair<nanovdb::Vec3d, nanovdb::Vec3d>
    coarseVoxSizeAndOrigin(int64_t bi, nanovdb::Coord branchingFactor) const {
        TORCH_CHECK(branchingFactor[0] > 0 && branchingFactor[1] > 0 && branchingFactor[2] > 0,
                    "Coarsening factor must be greater than 0");
        const nanovdb::Vec3d w = branchingFactor.asVec3d() * voxelSize(bi);
        const nanovdb::Vec3d tx =
            (branchingFactor.asVec3d() - nanovdb::Vec3d(1.0)) * voxelSize(bi) * 0.5 +
            voxelOrigin(bi);
        return std::make_pair(w, tx);
    }

    inline int64_t
    negativeToPositiveIndexWithRangecheck(int64_t bi) const {
        if (bi < 0) {
            bi += batchSize();
        }
        TORCH_CHECK_INDEX(bi >= 0 && bi < batchSize(), "Batch index ", bi,
                          " is out of range for grid batch of size " + std::to_string(batchSize()));
        return static_cast<int64_t>(bi);
    }

    void recomputeBatchOffsets();

    template <typename Indexable>
    c10::intrusive_ptr<GridBatchImpl>
    indexInternal(const Indexable &idx, int64_t size) const {
        if (size == 0) {
            return c10::make_intrusive<GridBatchImpl>(device(), isMutable());
        }
        TORCH_CHECK(size >= 0,
                    "Indexing with negative size is not supported (this should never happen)");
        c10::intrusive_ptr<GridBatchImpl> ret = c10::make_intrusive<GridBatchImpl>();
        ret->mGridHdl                         = mGridHdl;

        int64_t            cumVoxels    = 0;
        int64_t            cumLeaves    = 0;
        int64_t            maxVoxels    = 0;
        uint32_t           maxLeafCount = 0;
        int64_t            count        = 0;
        nanovdb::CoordBBox totalBbox;

        std::vector<torch::Tensor> leafBatchIdxs;

        // If the grid we're creating a view over is contiguous, we inherit that
        bool isContiguous = mBatchMetadata.mIsContiguous;
        for (size_t i = 0; i < size; i += 1) {
            int64_t bi = idx[i];
            bi         = negativeToPositiveIndexWithRangecheck(bi);

            // If indices are not contiguous or the grid we're viewing is not contiguous, then we're
            // no longer contiguous
            isContiguous = isContiguous && (bi == count);

            const uint32_t            numLeaves = mHostGridMetadata[bi].mNumLeaves;
            const int64_t             numVoxels = mHostGridMetadata[bi].mNumVoxels;
            const nanovdb::CoordBBox &bbox      = mHostGridMetadata[bi].mBBox;

            ret->mHostGridMetadata.push_back(mHostGridMetadata[bi]);
            ret->mHostGridMetadata[count].mCumLeaves = cumLeaves;
            ret->mHostGridMetadata[count].mCumVoxels = cumVoxels;

            if (count == 0) {
                totalBbox = bbox;
            } else {
                totalBbox.expand(bbox);
            }
            cumLeaves += numLeaves;
            cumVoxels += numVoxels;
            maxVoxels    = std::max(maxVoxels, numVoxels);
            maxLeafCount = std::max(maxLeafCount, numLeaves);
            leafBatchIdxs.push_back(
                torch::full({ numLeaves }, torch::Scalar(count),
                            torch::TensorOptions().dtype(fvdb::JIdxScalarType).device(device())));
            count += 1;
        }

        // If all the indices were contiguous and the grid we're viewing is contiguous, then we're
        // contiguous
        ret->mBatchMetadata.mIsContiguous = isContiguous && (count == batchSize());
        ret->mBatchMetadata.mTotalLeaves  = cumLeaves;
        ret->mBatchMetadata.mTotalVoxels  = cumVoxels;
        ret->mBatchMetadata.mMaxVoxels    = maxVoxels;
        ret->mBatchMetadata.mMaxLeafCount = maxLeafCount;
        ret->mBatchMetadata.mTotalBBox    = totalBbox;
        ret->mBatchMetadata.mIsMutable    = isMutable();

        if (leafBatchIdxs.size() > 0) {
            ret->mLeafBatchIndices = torch::cat(leafBatchIdxs, 0);
        }

        ret->syncMetadataToDeviceIfCUDA(false);
        ret->recomputeBatchOffsets();

        if (mListIndices.size(0) > 0) {
            TORCH_CHECK(false, "Nested lists of GridBatches are not supported yet");
        } else {
            ret->mListIndices = mListIndices;
        }
        return ret;
    }

  public:
    template <typename GridType> class Accessor {
        friend class GridBatchImpl;
        const GridBatchImpl::GridMetadata *__restrict__ mMetadata = nullptr; // 8 bytes
        const nanovdb::NanoGrid<GridType> *__restrict__ mGridPtr  = nullptr; // 8 bytes
        fvdb::JIdxType *__restrict__ mLeafBatchIndices            = nullptr; // 8 bytes
        int64_t  mTotalVoxels                                     = 0;       // 8 bytes
        int64_t  mTotalLeaves                                     = 0;       // 8 bytes
        int64_t  mMaxVoxels                                       = 0;       // 8 bytes
        uint32_t mMaxLeafCount                                    = 0;       // 4 bytes
        int64_t  mGridCount                                       = 0;       // 8 bytes

      private:
        __hostdev__ inline int64_t
        negativeToPositiveIndexWithRangecheck(int64_t bi) const {
            if (bi < 0) {
                bi += batchSize();
            }
            assert(bi >= 0 && bi < batchSize());
            return static_cast<int64_t>(bi);
        }

      public:
        __hostdev__ const nanovdb::NanoGrid<GridType>                   *
        grid(int64_t bi) const {
            bi = negativeToPositiveIndexWithRangecheck(bi);
            return reinterpret_cast<const nanovdb::NanoGrid<GridType> *>(
                reinterpret_cast<const char *>(mGridPtr) + mMetadata[bi].mCumBytes);
        }

        __hostdev__ nanovdb::CoordBBox
                    bbox(int64_t bi) const {
            bi = negativeToPositiveIndexWithRangecheck(bi);
            return grid(bi)->tree().bbox();
        }

        __hostdev__ nanovdb::CoordBBox
                    dualBbox(int64_t bi) const {
            bi = negativeToPositiveIndexWithRangecheck(bi);
            nanovdb::CoordBBox dualBbox(bbox(bi));
            dualBbox.mCoord[1] += nanovdb::Coord(1, 1, 1);
            return dualBbox;
        }

        __hostdev__ int64_t
        batchSize() const {
            return mGridCount;
        }

        __hostdev__ int64_t
        voxelOffset(int64_t bi) const {
            bi = negativeToPositiveIndexWithRangecheck(bi);
            return mMetadata[bi].mCumVoxels;
        }

        __hostdev__ int64_t
        leafOffset(int64_t bi) const {
            bi = negativeToPositiveIndexWithRangecheck(bi);
            return mMetadata[bi].mCumLeaves;
        }

        __hostdev__ int64_t
        maxVoxels() const {
            return mMaxVoxels;
        }

        __hostdev__ uint32_t
        maxLeafCount() const {
            return mMaxLeafCount;
        }

        __hostdev__ int64_t
        totalVoxels() const {
            return mTotalVoxels;
        }

        __hostdev__ int64_t
        totalLeaves() const {
            return mTotalLeaves;
        }

        __hostdev__ const VoxelCoordTransform &
        primalTransform(int64_t bi) const {
            bi = negativeToPositiveIndexWithRangecheck(bi);
            return mMetadata[bi].mPrimalTransform;
        }

        __hostdev__ const VoxelCoordTransform &
        dualTransform(int64_t bi) const {
            bi = negativeToPositiveIndexWithRangecheck(bi);
            return mMetadata[bi].mDualTransform;
        }

        __hostdev__ fvdb::JIdxType
                    leafBatchIndex(int64_t leaf_idx) const {
            return mLeafBatchIndices[leaf_idx];
        }
    };

    template <typename GridType>
    Accessor<GridType>
    hostAccessor() const {
        TORCH_CHECK(!isEmpty(), "Cannot access empty grid");
        Accessor<GridType> ret;
        ret.mMetadata = mHostGridMetadata.data();
        ret.mGridPtr  = mGridHdl->template grid<GridType>();
        TORCH_CHECK(ret.mGridPtr != nullptr, "Failed to get host grid pointer");
        ret.mTotalVoxels      = mBatchMetadata.mTotalVoxels;
        ret.mTotalLeaves      = mBatchMetadata.mTotalLeaves;
        ret.mMaxVoxels        = mBatchMetadata.mMaxVoxels;
        ret.mMaxLeafCount     = mBatchMetadata.mMaxLeafCount;
        ret.mGridCount        = static_cast<int64_t>(mHostGridMetadata.size());
        ret.mLeafBatchIndices = mLeafBatchIndices.data_ptr<fvdb::JIdxType>();

        return ret;
    }

    template <typename GridType>
    Accessor<GridType>
    deviceAccessor() const {
        TORCH_CHECK(!isEmpty(), "Cannot access empty grid");
        TORCH_CHECK(device().is_cuda(), "Cannot access device accessor on non-CUDA device");
        Accessor<GridType> ret;
        ret.mMetadata = mDeviceGridMetadata;
        ret.mGridPtr  = mGridHdl->template deviceGrid<GridType>();
        TORCH_CHECK(ret.mGridPtr != nullptr, "Failed to get device grid pointer");
        ret.mTotalVoxels      = mBatchMetadata.mTotalVoxels;
        ret.mTotalLeaves      = mBatchMetadata.mTotalLeaves;
        ret.mMaxVoxels        = mBatchMetadata.mMaxVoxels;
        ret.mMaxLeafCount     = mBatchMetadata.mMaxLeafCount;
        ret.mGridCount        = static_cast<int64_t>(mHostGridMetadata.size());
        ret.mLeafBatchIndices = mLeafBatchIndices.data_ptr<fvdb::JIdxType>();

        return ret;
    }

    GridBatchImpl() = default;

    GridBatchImpl(torch::Device device, bool isMutable);

    GridBatchImpl(nanovdb::GridHandle<TorchDeviceBuffer> &&gridHdl,
                  const std::vector<nanovdb::Vec3d>       &voxelSizes,
                  const std::vector<nanovdb::Vec3d>       &voxelOrigins);

    GridBatchImpl(nanovdb::GridHandle<TorchDeviceBuffer> &&gridHdl,
                  const nanovdb::Vec3d &globalVoxelSize, const nanovdb::Vec3d &globalVoxelOrigin);

    ~GridBatchImpl();

    // Cannot move make copies of this handle. There is only one owner of the underlying buffer.
    // This class should only be created and copied through c10::intrusive_ptr
    GridBatchImpl &operator=(GridBatchImpl &&other) = delete;
    GridBatchImpl(GridBatchImpl &&other)            = delete;
    GridBatchImpl(GridBatchImpl &other)             = delete;
    GridBatchImpl &operator=(GridBatchImpl &other)  = delete;

    torch::Tensor voxelOffsets(bool ignoreDisabledVoxels) const;

    torch::Tensor jlidx(bool ignoreDisabledVoxels = true) const;

    torch::Tensor jidx(bool ignoreDisabledVoxels) const;

    int64_t
    totalLeaves() const {
        return mBatchMetadata.mTotalLeaves;
    }

    int64_t
    totalVoxels() const {
        return mBatchMetadata.mTotalVoxels;
    }

    int64_t totalEnabledVoxels(bool ignoreDisabledVoxels) const;

    int64_t
    maxVoxelsPerGrid() const {
        return mBatchMetadata.mMaxVoxels;
    }

    int64_t
    maxLeavesPerGrid() const {
        return static_cast<int64_t>(mBatchMetadata.mMaxLeafCount);
    }

    int64_t
    batchSize() const {
        return static_cast<int64_t>(mHostGridMetadata.size());
    }

    uint64_t
    totalBytes() const {
        uint64_t sum = 0;
        for (const auto &grid: mHostGridMetadata) {
            sum += grid.mNumBytes;
        }
        return sum;
    }

    const nanovdb::GridHandle<TorchDeviceBuffer> &
    nanoGridHandle() const {
        return *mGridHdl;
    }

    bool
    isMutable() const {
        return mBatchMetadata.mIsMutable;
    }

    const c10::Device
    device() const {
        return mGridHdl->buffer().device();
    }

    bool
    isEmpty() const {
        return mGridHdl->buffer().isEmpty();
    }

    uint32_t
    numLeaves(int64_t bi) const {
        bi = negativeToPositiveIndexWithRangecheck(bi);
        return mHostGridMetadata[bi].mNumLeaves;
    }

    int64_t
    numVoxels(int64_t bi) const {
        bi = negativeToPositiveIndexWithRangecheck(bi);
        return mHostGridMetadata[bi].mNumVoxels;
    }

    int64_t
    cumVoxels(int64_t bi) const {
        bi = negativeToPositiveIndexWithRangecheck(bi);
        return mHostGridMetadata[bi].mCumVoxels;
    }

    uint64_t
    numBytes(int64_t bi) const {
        bi = negativeToPositiveIndexWithRangecheck(bi);
        return mHostGridMetadata[bi].mNumBytes;
    }

    uint64_t
    cumBytes(int64_t bi) const {
        bi = negativeToPositiveIndexWithRangecheck(bi);
        return mHostGridMetadata[bi].mCumBytes;
    }

    const VoxelCoordTransform &
    primalTransform(int64_t bi) const {
        bi = negativeToPositiveIndexWithRangecheck(bi);
        return mHostGridMetadata[bi].mPrimalTransform;
    }

    const VoxelCoordTransform &
    dualTransform(int64_t bi) const {
        bi = negativeToPositiveIndexWithRangecheck(bi);
        return mHostGridMetadata[bi].mDualTransform;
    }

    void
    gridVoxelSizesAndOrigins(std::vector<nanovdb::Vec3d> &outVoxelSizes,
                             std::vector<nanovdb::Vec3d> &outVoxelOrigins) const {
        outVoxelSizes.clear();
        outVoxelOrigins.clear();
        for (int64_t i = 0; i < batchSize(); ++i) {
            outVoxelSizes.emplace_back(mHostGridMetadata[i].mVoxelSize);
            outVoxelOrigins.emplace_back(mHostGridMetadata[i].voxelOrigin());
        }
    }

    const nanovdb::CoordBBox &
    totalBBox() const {
        return mBatchMetadata.mTotalBBox;
    }

    const nanovdb::CoordBBox &
    bbox(int64_t bi) const {
        checkNonEmptyGrid();
        bi = negativeToPositiveIndexWithRangecheck(bi);
        return mHostGridMetadata[bi].mBBox;
    }

    const nanovdb::CoordBBox
    dualBbox(int64_t bi) const {
        bi                          = negativeToPositiveIndexWithRangecheck(bi);
        nanovdb::CoordBBox dualBbox = bbox(bi);
        dualBbox.mCoord[1] += nanovdb::Coord(1, 1, 1);
        return dualBbox;
    }

    const nanovdb::Vec3d &
    voxelSize(int64_t bi) const {
        bi = negativeToPositiveIndexWithRangecheck(bi);
        return mHostGridMetadata[bi].mVoxelSize;
    }

    const nanovdb::Vec3d
    voxelOrigin(int64_t bi) const {
        bi = negativeToPositiveIndexWithRangecheck(bi);
        return mHostGridMetadata[bi].voxelOrigin();
    }

    torch::Tensor worldToGridMatrix(int64_t bi) const;

    torch::Tensor gridToWorldMatrix(int64_t bi) const;

    c10::intrusive_ptr<GridBatchImpl> clone(torch::Device device, bool blocking = false) const;

    void
    checkNonEmptyGrid() const {
        TORCH_CHECK(!isEmpty(), "Empty grid");
    }

    void
    checkDevice(const torch::Tensor &t) const {
        torch::Device hdlDevice = mGridHdl->buffer().device();
        TORCH_CHECK(hdlDevice == t.device(), "All tensors must be on the same device (" +
                                                 hdlDevice.str() + ") as index grid but got " +
                                                 t.device().str());
    }

    void
    checkDevice(const JaggedTensor &t) const {
        torch::Device hdlDevice = mGridHdl->buffer().device();
        TORCH_CHECK(hdlDevice == t.device(), "All tensors must be on the same device (" +
                                                 hdlDevice.str() + ") as index grid but got " +
                                                 t.device().str());
    }

    JaggedTensor jaggedTensor(const torch::Tensor &data, bool ignoreDisabledVoxels) const;

    void setGlobalPrimalTransform(const VoxelCoordTransform &transform, bool syncToDevice = true);
    void setGlobalDualTransform(const VoxelCoordTransform &transform, bool syncToDevice = true);
    void setGlobalVoxelSize(const nanovdb::Vec3d &voxelSize, bool syncToDevice = true);
    void setGlobalVoxelOrigin(const nanovdb::Vec3d &voxelOrigin, bool syncToDevice = true);
    void setGlobalVoxelSizeAndOrigin(const nanovdb::Vec3d &voxelSize,
                                     const nanovdb::Vec3d &voxelOrigin, bool syncToDevice = true);

    void setFineTransformFromCoarseGrid(const GridBatchImpl &coarseBatch,
                                        nanovdb::Coord       subdivisionFactor);
    void setCoarseTransformFromFineGrid(const GridBatchImpl &fineBatch,
                                        nanovdb::Coord       coarseningFactor);
    void setPrimalTransformFromDualGrid(const GridBatchImpl &dualBatch);

    void setGrid(nanovdb::GridHandle<TorchDeviceBuffer> &&gridHdl, const torch::Tensor listIndices,
                 const std::vector<nanovdb::Vec3d> &voxelSizes,
                 const std::vector<nanovdb::Vec3d> &voxelOrigins, bool blocking = false);

    c10::intrusive_ptr<GridBatchImpl> index(int64_t bi) const;
    c10::intrusive_ptr<GridBatchImpl> index(ssize_t start, ssize_t stop, ssize_t step) const;
    c10::intrusive_ptr<GridBatchImpl> index(const torch::Tensor &indices) const;
    c10::intrusive_ptr<GridBatchImpl> index(const std::vector<int64_t> &indices) const;
    c10::intrusive_ptr<GridBatchImpl> index(const std::vector<bool> &indices) const;

    static c10::intrusive_ptr<GridBatchImpl>
    concatenate(const std::vector<c10::intrusive_ptr<GridBatchImpl>> &elements);

    static c10::intrusive_ptr<GridBatchImpl> contiguous(c10::intrusive_ptr<GridBatchImpl> input);

    bool
    isContiguous() const {
        return mBatchMetadata.mIsContiguous;
    }

    // Return a CPU int8 tensor with this grid packed inside it
    torch::Tensor serialize() const;

    // Load a CPU int8 tensor into a grid batch handle
    static c10::intrusive_ptr<GridBatchImpl> deserialize(const torch::Tensor &serialized);

  private:
    // We're going to version serialization. These are v0
    torch::Tensor                            serializeV0() const;
    static c10::intrusive_ptr<GridBatchImpl> deserializeV0(const torch::Tensor &serialized);
};

template <typename GridType> using BatchGridAccessor = typename GridBatchImpl::Accessor<GridType>;

} // namespace detail
} // namespace fvdb

#endif // FVDB_DETAIL_GRIDBATCHIMPL_H