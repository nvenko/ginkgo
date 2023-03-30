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

#include <ginkgo/core/base/memory.hpp>


#include <memory>
#include <type_traits>


#include <gtest/gtest.h>


#include <ginkgo/core/base/exception.hpp>
#include <ginkgo/core/base/exception_helpers.hpp>
#include <ginkgo/core/base/executor.hpp>


#include "dpcpp/test/utils.hpp"


namespace {


class Memory : public ::testing::Test {
protected:
    Memory()
        : exec{gko::DpcppExecutor::create(0, gko::OmpExecutor::create())},
          host_exec_with_unified{gko::OmpExecutor::create(
              std::make_shared<gko::DpcppUnifiedAllocator>(exec->get_queue()))},
          exec_with_unified{gko::DpcppExecutor::create(
              exec->get_queue(), host_exec_with_unified,
              std::make_shared<gko::DpcppUnifiedAllocator>(exec->get_queue()))}
    {}

    std::shared_ptr<gko::DpcppExecutor> exec;
    std::shared_ptr<gko::OmpExecutor> host_exec_with_unified;
    std::shared_ptr<gko::DpcppExecutor> exec_with_unified;
};


TEST_F(Memory, DeviceAllocationWorks)
{
    gko::array<int> data{exec, {1, 2}};

    GKO_ASSERT_ARRAY_EQ(data, I<int>({1, 2}));
}


TEST_F(Memory, UnifiedDeviceAllocationWorks)
{
    gko::array<int> data{exec_with_unified, {1, 2}};
    exec->synchronize();

    ASSERT_EQ(data.get_const_data()[0], 1);
    ASSERT_EQ(data.get_const_data()[1], 2);
}


TEST_F(Memory, HostUnifiedAllocationWorks)
{
    gko::array<int> data{host_exec_with_unified, {1, 2}};

    ASSERT_EQ(data.get_const_data()[0], 1);
    ASSERT_EQ(data.get_const_data()[1], 2);
}


}  // namespace
