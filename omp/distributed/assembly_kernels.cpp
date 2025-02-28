// SPDX-FileCopyrightText: 2017 - 2024 The Ginkgo authors
//
// SPDX-License-Identifier: BSD-3-Clause

#include "core/distributed/assembly_kernels.hpp"

#include <algorithm>

#include <omp.h>

#include <ginkgo/core/base/exception_helpers.hpp>

#include "core/base/allocator.hpp"
#include "core/base/device_matrix_data_kernels.hpp"
#include "core/components/prefix_sum_kernels.hpp"
#include "reference/distributed/partition_helpers.hpp"


namespace gko {
namespace kernels {
namespace omp {
namespace assembly {


template <typename ValueType, typename LocalIndexType, typename GlobalIndexType>
void count_non_owning_entries(
    std::shared_ptr<const DefaultExecutor> exec,
    const device_matrix_data<ValueType, GlobalIndexType>& input,
    const experimental::distributed::Partition<LocalIndexType, GlobalIndexType>*
        row_partition,
    comm_index_type local_part, array<comm_index_type>& send_count,
    array<GlobalIndexType>& send_positions,
    array<GlobalIndexType>& original_positions)
{
    auto num_input_elements = input.get_num_stored_elements();
    auto input_row_idxs = input.get_const_row_idxs();
    auto row_part_ids = row_partition->get_part_ids();
    array<comm_index_type> row_part_ids_per_entry{exec, num_input_elements};

    size_type row_range_id = 0;
#pragma omp parallel for firstprivate(row_range_id)
    for (size_type i = 0; i < input.get_num_stored_elements(); ++i) {
        auto global_row = input_row_idxs[i];
        row_range_id = find_range(global_row, row_partition, row_range_id);
        auto row_part_id = row_part_ids[row_range_id];
        row_part_ids_per_entry.get_data()[i] = row_part_id;
        if (row_part_id != local_part) {
#pragma omp atomic
            send_count.get_data()[row_part_id]++;
            original_positions.get_data()[i] = i;
        } else {
            original_positions.get_data()[i] = -1;
        }
    }

    auto comp = [&row_part_ids_per_entry, local_part](auto i, auto j) {
        comm_index_type a =
            i == -1 ? local_part : row_part_ids_per_entry.get_const_data()[i];
        comm_index_type b =
            j == -1 ? local_part : row_part_ids_per_entry.get_const_data()[j];
        return a < b;
    };
    std::stable_sort(original_positions.get_data(),
                     original_positions.get_data() + num_input_elements, comp);

#pragma omp parallel for
    for (size_type i = 0; i < num_input_elements; i++) {
        send_positions.get_data()[i] =
            original_positions.get_const_data()[i] == -1 ? 0 : 1;
    }

    components::prefix_sum_nonnegative(exec, send_positions.get_data(),
                                       num_input_elements);
}

GKO_INSTANTIATE_FOR_EACH_VALUE_AND_LOCAL_GLOBAL_INDEX_TYPE(
    GKO_DECLARE_COUNT_NON_OWNING_ENTRIES);


}  // namespace assembly
}  // namespace omp
}  // namespace kernels
}  // namespace gko
