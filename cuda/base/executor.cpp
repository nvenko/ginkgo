/*******************************<GINKGO LICENSE>******************************
Copyright (c) 2017-2023, the Ginkgo authors
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

1. Redistributions of source code must retain the above copyright
notice, this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright
notice, this list of conditions and the following disclaimer in the
documentation and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its
contributors may be used to endorse or promote products derived from
this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
******************************<GINKGO LICENSE>*******************************/

#include <ginkgo/core/base/executor.hpp>


#include <iostream>
#include <stdexcept>
#include <thread>


#include <cuda_runtime.h>


#include <ginkgo/config.hpp>
#include <ginkgo/core/base/device.hpp>
#include <ginkgo/core/base/exception.hpp>
#include <ginkgo/core/base/exception_helpers.hpp>
#include <ginkgo/core/base/memory.hpp>


#include "cuda/base/config.hpp"
#include "cuda/base/cublas_bindings.hpp"
#include "cuda/base/cusparse_handle.hpp"
#include "cuda/base/scoped_device_id.hpp"


namespace gko {


#include "common/cuda_hip/base/executor.hpp.inc"


std::unique_ptr<CudaAllocatorBase> allocator_from_mode(int device_id,
                                                       allocation_mode mode)
{
    switch (mode) {
    case allocation_mode::device:
        return std::make_unique<CudaAllocator>();
    case allocation_mode::unified_global:
        return std::make_unique<CudaUnifiedAllocator>(device_id);
    case allocation_mode::unified_host:
        return std::make_unique<CudaUnifiedAllocator>(device_id);
    default:
        GKO_NOT_SUPPORTED(mode);
    }
}


std::shared_ptr<CudaExecutor> CudaExecutor::create(
    int device_id, std::shared_ptr<Executor> master, bool device_reset,
    allocation_mode alloc_mode, cudaStream_t stream)
{
    return std::shared_ptr<CudaExecutor>(
        new CudaExecutor(device_id, std::move(master),
                         allocator_from_mode(device_id, alloc_mode), stream));
}


std::shared_ptr<CudaExecutor> CudaExecutor::create(
    int device_id, std::shared_ptr<Executor> master,
    std::shared_ptr<CudaAllocatorBase> alloc, cudaStream_t stream)
{
    return std::shared_ptr<CudaExecutor>(new CudaExecutor(
        device_id, std::move(master), std::move(alloc), stream));
}


void CudaExecutor::populate_exec_info(const machine_topology* mach_topo)
{
    if (this->get_device_id() < this->get_num_devices() &&
        this->get_device_id() >= 0) {
        detail::cuda_scoped_device_id_guard g(this->get_device_id());
        GKO_ASSERT_NO_CUDA_ERRORS(
            cudaDeviceGetPCIBusId(&(this->get_exec_info().pci_bus_id.front()),
                                  13, this->get_device_id()));

        auto cuda_hwloc_obj =
            mach_topo->get_pci_device(this->get_exec_info().pci_bus_id);
        if (cuda_hwloc_obj) {
            this->get_exec_info().numa_node = cuda_hwloc_obj->closest_numa;
            this->get_exec_info().closest_pu_ids =
                cuda_hwloc_obj->closest_pu_ids;
        }
    }
}


void OmpExecutor::raw_copy_to(const CudaExecutor* dest, size_type num_bytes,
                              const void* src_ptr, void* dest_ptr) const
{
    if (num_bytes > 0) {
        detail::cuda_scoped_device_id_guard g(dest->get_device_id());
        GKO_ASSERT_NO_CUDA_ERRORS(cudaMemcpyAsync(dest_ptr, src_ptr, num_bytes,
                                                  cudaMemcpyHostToDevice,
                                                  dest->get_stream()));
        dest->synchronize();
    }
}


void CudaExecutor::raw_free(void* ptr) const noexcept
{
    detail::cuda_scoped_device_id_guard g(this->get_device_id());
    alloc_->deallocate(ptr);
}


void* CudaExecutor::raw_alloc(size_type num_bytes) const
{
    detail::cuda_scoped_device_id_guard g(this->get_device_id());
    return alloc_->allocate(num_bytes);
}


void CudaExecutor::raw_copy_to(const OmpExecutor*, size_type num_bytes,
                               const void* src_ptr, void* dest_ptr) const
{
    if (num_bytes > 0) {
        detail::cuda_scoped_device_id_guard g(this->get_device_id());
        GKO_ASSERT_NO_CUDA_ERRORS(cudaMemcpyAsync(dest_ptr, src_ptr, num_bytes,
                                                  cudaMemcpyDeviceToHost,
                                                  this->get_stream()));
        this->synchronize();
    }
}


void CudaExecutor::raw_copy_to(const HipExecutor* dest, size_type num_bytes,
                               const void* src_ptr, void* dest_ptr) const
{
#if GINKGO_HIP_PLATFORM_NVCC == 1
    if (num_bytes > 0) {
        detail::cuda_scoped_device_id_guard g(this->get_device_id());
        GKO_ASSERT_NO_CUDA_ERRORS(cudaMemcpyPeerAsync(
            dest_ptr, dest->get_device_id(), src_ptr, this->get_device_id(),
            num_bytes, this->get_stream()));
        this->synchronize();
    }
#else
    GKO_NOT_SUPPORTED(dest);
#endif
}


void CudaExecutor::raw_copy_to(const DpcppExecutor* dest, size_type num_bytes,
                               const void* src_ptr, void* dest_ptr) const
{
    GKO_NOT_SUPPORTED(dest);
}


void CudaExecutor::raw_copy_to(const CudaExecutor* dest, size_type num_bytes,
                               const void* src_ptr, void* dest_ptr) const
{
    if (num_bytes > 0) {
        detail::cuda_scoped_device_id_guard g(this->get_device_id());
        GKO_ASSERT_NO_CUDA_ERRORS(cudaMemcpyPeerAsync(
            dest_ptr, dest->get_device_id(), src_ptr, this->get_device_id(),
            num_bytes, this->get_stream()));
        this->synchronize();
    }
}


void CudaExecutor::synchronize() const
{
    detail::cuda_scoped_device_id_guard g(this->get_device_id());
    GKO_ASSERT_NO_CUDA_ERRORS(cudaStreamSynchronize(this->get_stream()));
}


scoped_device_id_guard CudaExecutor::get_scoped_device_id_guard() const
{
    return {this, this->get_device_id()};
}


int CudaExecutor::get_num_devices()
{
    int deviceCount = 0;
    auto error_code = cudaGetDeviceCount(&deviceCount);
    if (error_code == cudaErrorNoDevice) {
        return 0;
    }
    GKO_ASSERT_NO_CUDA_ERRORS(error_code);
    return deviceCount;
}


void CudaExecutor::set_gpu_property()
{
    if (this->get_device_id() < this->get_num_devices() &&
        this->get_device_id() >= 0) {
        detail::cuda_scoped_device_id_guard g(this->get_device_id());
        GKO_ASSERT_NO_CUDA_ERRORS(cudaDeviceGetAttribute(
            &this->get_exec_info().major, cudaDevAttrComputeCapabilityMajor,
            this->get_device_id()));
        GKO_ASSERT_NO_CUDA_ERRORS(cudaDeviceGetAttribute(
            &this->get_exec_info().minor, cudaDevAttrComputeCapabilityMinor,
            this->get_device_id()));
        GKO_ASSERT_NO_CUDA_ERRORS(cudaDeviceGetAttribute(
            &this->get_exec_info().num_computing_units,
            cudaDevAttrMultiProcessorCount, this->get_device_id()));
        auto max_threads_per_block = 0;
        GKO_ASSERT_NO_CUDA_ERRORS(cudaDeviceGetAttribute(
            &max_threads_per_block, cudaDevAttrMaxThreadsPerBlock,
            this->get_device_id()));
        std::vector<int> max_threads_per_block_dim(3, 0);
        GKO_ASSERT_NO_CUDA_ERRORS(cudaDeviceGetAttribute(
            &max_threads_per_block_dim[0], cudaDevAttrMaxBlockDimX,
            this->get_device_id()));
        GKO_ASSERT_NO_CUDA_ERRORS(cudaDeviceGetAttribute(
            &max_threads_per_block_dim[1], cudaDevAttrMaxBlockDimY,
            this->get_device_id()));
        GKO_ASSERT_NO_CUDA_ERRORS(cudaDeviceGetAttribute(
            &max_threads_per_block_dim[2], cudaDevAttrMaxBlockDimZ,
            this->get_device_id()));
        this->get_exec_info().max_workgroup_size = max_threads_per_block;
        this->get_exec_info().max_workitem_sizes = max_threads_per_block_dim;
        this->get_exec_info().num_pu_per_cu =
            convert_sm_ver_to_cores(this->get_exec_info().major,
                                    this->get_exec_info().minor) /
            kernels::cuda::config::warp_size;
        this->get_exec_info().max_subgroup_size =
            kernels::cuda::config::warp_size;
    }
}


void CudaExecutor::init_handles()
{
    if (this->get_device_id() < this->get_num_devices() &&
        this->get_device_id() >= 0) {
        const auto id = this->get_device_id();
        detail::cuda_scoped_device_id_guard g(id);
        this->cublas_handle_ = handle_manager<cublasContext>(
            kernels::cuda::cublas::init(this->get_stream()),
            [id](cublasHandle_t handle) {
                detail::cuda_scoped_device_id_guard g(id);
                kernels::cuda::cublas::destroy(handle);
            });
        this->cusparse_handle_ = handle_manager<cusparseContext>(
            kernels::cuda::cusparse::init(this->get_stream()),
            [id](cusparseHandle_t handle) {
                detail::cuda_scoped_device_id_guard g(id);
                kernels::cuda::cusparse::destroy(handle);
            });
    }
}


}  // namespace gko
