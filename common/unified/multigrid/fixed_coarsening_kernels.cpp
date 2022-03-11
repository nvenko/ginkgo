/*******************************<GINKGO LICENSE>******************************
Copyright (c) 2017-2022, the Ginkgo authors
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

#include "core/multigrid/fixed_coarsening_kernels.hpp"


#include <ginkgo/core/base/math.hpp>


#include "common/unified/base/kernel_launch.hpp"
#include "common/unified/base/kernel_launch_reduction.hpp"
#include "core/components/prefix_sum_kernels.hpp"


namespace gko {
namespace kernels {
namespace GKO_DEVICE_NAMESPACE {
/**
 * @brief The FixedCoarsening namespace.
 *
 * @ingroup fixed_coarsening
 */
namespace fixed_coarsening {


template <typename ValueType, typename IndexType>
void fill_restrict_op(std::shared_ptr<const DefaultExecutor> exec,
                      const Array<IndexType>* coarse_rows,
                      matrix::Csr<ValueType, IndexType>* restrict_op)
{
    run_kernel(
        exec,
        [] GKO_KERNEL(auto tidx, const auto coarse_data,
                      auto restrict_col_idxs) {
            if (coarse_data[tidx] >= 0) {
                restrict_col_idxs[coarse_data[tidx]] = tidx;
            }
        },
        coarse_rows->get_num_elems(), coarse_rows->get_const_data(),
        restrict_op->get_col_idxs());
}

GKO_INSTANTIATE_FOR_EACH_VALUE_AND_INDEX_TYPE(
    GKO_DECLARE_FIXED_COARSENING_FILL_RESTRICT_OP);


template <typename IndexType>
void fill_coarse_indices(std::shared_ptr<const DefaultExecutor> exec,
                         const Array<IndexType>* coarse_rows,
                         Array<IndexType>* coarse_row_map)
{
    IndexType num_elems = coarse_row_map->get_num_elems();
    run_kernel(
        exec,
        [] GKO_KERNEL(auto tidx, const auto coarse_data, auto coarse_map_data,
                      auto c_size, auto f_size) {
            if (tidx == 0) {
                auto idx = 0;
                for (auto i = 0; i < f_size; i++) {
                    if (i == coarse_data[idx]) {
                        coarse_map_data[i] = idx;
                        idx++;
                    }
                }
            }
        },
        num_elems, coarse_rows->get_const_data(), coarse_row_map->get_data(),
        coarse_row_map->get_num_elems(), num_elems);
}

GKO_INSTANTIATE_FOR_EACH_INDEX_TYPE(
    GKO_DECLARE_FIXED_COARSENING_FILL_COARSE_INDICES);


}  // namespace fixed_coarsening
}  // namespace GKO_DEVICE_NAMESPACE
}  // namespace kernels
}  // namespace gko
