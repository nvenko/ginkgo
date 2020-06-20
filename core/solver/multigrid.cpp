/*******************************<GINKGO LICENSE>******************************
Copyright (c) 2017-2020, the Ginkgo authors
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

#include <ginkgo/core/solver/multigrid.hpp>


#include <ginkgo/core/base/exception.hpp>
#include <ginkgo/core/base/exception_helpers.hpp>
#include <ginkgo/core/base/executor.hpp>
#include <ginkgo/core/base/math.hpp>
#include <ginkgo/core/base/name_demangling.hpp>
#include <ginkgo/core/base/utils.hpp>
#include <ginkgo/core/matrix/dense.hpp>


#include "core/components/fill_array.hpp"
#include "core/solver/ir_kernels.hpp"
#include "core/solver/multigrid_kernels.hpp"


namespace gko {
namespace solver {
namespace multigrid {


GKO_REGISTER_OPERATION(initialize, ir::initialize);
GKO_REGISTER_OPERATION(fill_array, components::fill_array);
GKO_REGISTER_OPERATION(kcycle_step_1, multigrid::kcycle_step_1);
GKO_REGISTER_OPERATION(kcycle_step_2, multigrid::kcycle_step_2);
GKO_REGISTER_OPERATION(kcycle_check_stop, multigrid::kcycle_check_stop);


}  // namespace multigrid


namespace {


template <typename ValueType>
void handle_list(
    std::shared_ptr<const Executor> &exec, size_type index,
    std::shared_ptr<const LinOp> &matrix,
    std::vector<std::shared_ptr<const LinOpFactory>> &smoother_list,
    gko::Array<ValueType> &relaxation_array,
    std::vector<std::shared_ptr<LinOp>> &smoother,
    std::vector<std::shared_ptr<matrix::Dense<ValueType>>> &relaxation,
    std::shared_ptr<matrix::Dense<ValueType>> &one)
{
    auto list_size = smoother_list.size();
    if (list_size != 0) {
        auto temp_index = list_size == 1 ? 0 : index;
        GKO_ENSURE_IN_BOUNDS(temp_index, list_size);
        auto item = smoother_list.at(temp_index);
        if (item == nullptr) {
            smoother.emplace_back(nullptr);
        } else {
            smoother.emplace_back(give(item->generate(matrix)));
        }
    } else {
        smoother.emplace_back(nullptr);
    }
    auto array_size = relaxation_array.get_num_elems();
    if (array_size != 0) {
        auto temp_index = array_size == 1 ? 0 : index;
        GKO_ENSURE_IN_BOUNDS(temp_index, array_size);
        auto data = relaxation_array.get_data();
        relaxation.emplace_back(give(matrix::Dense<ValueType>::create(
            exec, gko::dim<2>{1},
            Array<ValueType>::view(exec, 1, data + temp_index), 1)));
    } else {
        // default is one;
        relaxation.emplace_back(one);
    }
}


template <typename ValueType>
struct MultigridState {
    using vec = matrix::Dense<ValueType>;
    using norm_vec = matrix::Dense<remove_complex<ValueType>>;
    MultigridState(std::shared_ptr<const Executor> exec_in,
                   const LinOp *system_matrix_in,
                   const Multigrid<ValueType> *multigrid_in,
                   const size_type nrhs_in, const vec *one_in,
                   const vec *neg_one_in, const size_type k_base_in = 1,
                   const remove_complex<ValueType> rel_tol_in = 1)
        : exec{std::move(exec_in)},
          system_matrix(system_matrix_in),
          multigrid(multigrid_in),
          nrhs(nrhs_in),
          one(one_in),
          neg_one(neg_one_in),
          k_base(k_base_in),
          rel_tol(rel_tol_in)
    {
        auto current_nrows = system_matrix->get_size()[0];
        auto rstr_prlg_list = multigrid->get_rstr_prlg_list();
        auto list_size = rstr_prlg_list.size();
        auto cycle = multigrid->get_cycle();
        r_list.reserve(list_size);
        g_list.reserve(list_size);
        e_list.reserve(list_size);
        if (cycle == multigrid_cycle::kfcg || cycle == multigrid_cycle::kgcr) {
            auto k_num = rstr_prlg_list.size() / k_base;
            alpha_list.reserve(k_num);
            beta_list.reserve(k_num);
            gamma_list.reserve(k_num);
            rho_list.reserve(k_num);
            zeta_list.reserve(k_num);
            v_list.reserve(k_num);
            w_list.reserve(k_num);
            d_list.reserve(k_num);
            old_norm_list.reserve(k_num);
            new_norm_list.reserve(k_num);
        }
        // Allocate memory first such that repeating allocation in each iter.
        for (int i = 0; i < rstr_prlg_list.size(); i++) {
            auto next_nrows =
                rstr_prlg_list.at(i)->get_coarse_operator()->get_size()[0];
            r_list.emplace_back(vec::create(exec, dim<2>{current_nrows, nrhs}));
            g_list.emplace_back(vec::create(exec, dim<2>{next_nrows, nrhs}));
            e_list.emplace_back(vec::create(exec, dim<2>{next_nrows, nrhs}));
            if ((cycle == multigrid_cycle::kfcg ||
                 cycle == multigrid_cycle::kgcr) &&
                i % k_base == 0) {
                auto scaler_size = dim<2>{1, nrhs};
                auto vector_size = dim<2>{next_nrows, nrhs};
                // 1 x nrhs
                alpha_list.emplace_back(vec::create(exec, scaler_size));
                beta_list.emplace_back(vec::create(exec, scaler_size));
                gamma_list.emplace_back(vec::create(exec, scaler_size));
                rho_list.emplace_back(vec::create(exec, scaler_size));
                zeta_list.emplace_back(vec::create(exec, scaler_size));
                // next level's nrows x nrhs
                v_list.emplace_back(vec::create(exec, vector_size));
                w_list.emplace_back(vec::create(exec, vector_size));
                d_list.emplace_back(vec::create(exec, vector_size));
                // 1 x nrhs norm_vec
                old_norm_list.emplace_back(norm_vec::create(exec, scaler_size));
                new_norm_list.emplace_back(norm_vec::create(exec, scaler_size));
            }
            current_nrows = next_nrows;
        }
    }

    void run_cycle(multigrid_cycle cycle, size_type level,
                   const std::shared_ptr<const LinOp> &matrix,
                   const matrix::Dense<ValueType> *b,
                   matrix::Dense<ValueType> *x)
    {
        auto r = r_list.at(level);
        auto g = g_list.at(level);
        auto e = e_list.at(level);
        // get rstr_prlg
        auto rstr_prlg = multigrid->get_rstr_prlg_list().at(level);
        auto total_level = multigrid->get_rstr_prlg_list().size();
        // get the pre_smoother
        auto pre_smoother = multigrid->get_pre_smoother_list().at(level);
        auto pre_relaxation = multigrid->get_pre_relaxation_list().at(level);
        // get the mid_smoother
        auto mid_smoother = multigrid->get_mid_smoother_list().at(level);
        auto mid_relaxation = multigrid->get_mid_relaxation_list().at(level);
        // get the post_smoother
        auto post_smoother = multigrid->get_post_smoother_list().at(level);
        auto post_relaxation = multigrid->get_post_relaxation_list().at(level);
        // move the residual computation in level zero to out-of-cycle
        if (level != 0) {
            r->copy_from(b);
            matrix->apply(neg_one, x, one, r.get());
        }
        // x += relaxation * Smoother(r)
        if (pre_smoother) {
            pre_smoother->apply(pre_relaxation.get(), r.get(), one, x);
            // compute residual
            r->copy_from(b);  // n * b
            matrix->apply(neg_one, x, one, r.get());
        }
        // first cycle
        rstr_prlg->restrict_apply(r.get(), g.get());
        // next level or solve it
        if (level + 1 == total_level) {
            multigrid->get_coarsest_solver()->apply(g.get(), e.get());
        } else {
            this->run_cycle(cycle, level + 1, rstr_prlg->get_coarse_operator(),
                            g.get(), e.get());
        }
        // additional work for non-v_cycle
        if (cycle == multigrid_cycle::f || cycle == multigrid_cycle::w) {
            // second cycle - f_cycle, w_cycle
            // prolong
            rstr_prlg->prolong_applyadd(e.get(), x);
            // compute residual
            r->copy_from(b);  // n * b
            matrix->apply(neg_one, x, one, r.get());
            // mid-smooth
            if (mid_smoother) {
                mid_smoother->apply(mid_relaxation.get(), r.get(), one, x);
                // compute residual
                r->copy_from(b);  // n * b
                matrix->apply(neg_one, x, one, r.get());
            }

            rstr_prlg->restrict_apply(r.get(), g.get());
            // next level or solve it
            if (level + 1 == total_level) {
                multigrid->get_coarsest_solver()->apply(g.get(), e.get());
            } else {
                if (cycle == multigrid_cycle::f) {
                    // f_cycle call v_cycle in the second cycle
                    this->run_cycle(multigrid_cycle::v, level + 1,
                                    rstr_prlg->get_coarse_operator(), g.get(),
                                    e.get());
                } else {
                    this->run_cycle(cycle, level + 1,
                                    rstr_prlg->get_coarse_operator(), g.get(),
                                    e.get());
                }
            }
        } else if ((cycle == multigrid_cycle::kfcg ||
                    cycle == multigrid_cycle::kgcr) &&
                   level % k_base == 0) {
            // otherwise, use v_cycle
            // do some work in coarse level - do not need prolong
            bool is_fcg = cycle == multigrid_cycle::kfcg;
            auto k_idx = level / k_base;
            auto alpha = alpha_list.at(k_idx);
            auto beta = beta_list.at(k_idx);
            auto gamma = gamma_list.at(k_idx);
            auto rho = rho_list.at(k_idx);
            auto zeta = zeta_list.at(k_idx);
            auto v = v_list.at(k_idx);
            auto w = w_list.at(k_idx);
            auto d = d_list.at(k_idx);
            auto old_norm = old_norm_list.at(k_idx);
            auto new_norm = new_norm_list.at(k_idx);
            auto matrix = rstr_prlg->get_coarse_operator();

            // first iteration
            matrix->apply(e.get(), v.get());
            std::shared_ptr<const matrix::Dense<ValueType>> t = is_fcg ? e : v;
            t->compute_dot(v.get(), rho.get());
            t->compute_dot(g.get(), alpha.get());

            if (!std::isnan(rel_tol) && rel_tol >= 0) {
                // calculate the r norm
                g->compute_norm2(old_norm.get());
            }
            // kcycle_step_1 update g, d
            // temp = alpha/rho
            // g = g - temp * v
            // d = e = temp * e
            exec->run(multigrid::make_kcycle_step_1(
                alpha.get(), rho.get(), v.get(), g.get(), d.get(), e.get()));
            // check ||new_r|| <= t * ||old_r|| only when t > 0 && t != inf
            bool is_stop = true;
            if (!std::isnan(rel_tol) && rel_tol >= 0) {
                // calculate the updated r norm
                g->compute_norm2(new_norm.get());
                // is_stop = true when all new_norm <= t * old_norm.
                exec->run(multigrid::make_kcycle_check_stop(
                    old_norm.get(), new_norm.get(), rel_tol, is_stop));
            }
            // rel_tol < 0: run two iteraion
            // rel_tol is nan: run one iteraions
            // others: new_norm <= rel_tol * old_norm -> run second iteraion.
            if (rel_tol < 0 || (rel_tol >= 0 && !is_stop)) {
                // second iteration
                // Apply on d for keeping the answer on e
                if (level + 1 == total_level) {
                    multigrid->get_coarsest_solver()->apply(g.get(), d.get());
                } else {
                    this->run_cycle(cycle, level + 1,
                                    rstr_prlg->get_coarse_operator(), g.get(),
                                    d.get());
                }
                matrix->apply(d.get(), w.get());
                t = is_fcg ? d : w;
                t->compute_dot(v.get(), gamma.get());
                t->compute_dot(w.get(), beta.get());
                t->compute_dot(g.get(), zeta.get());
                // kcycle_step_2 update e
                // scaler_d = zeta/(beta - gamma^2/rho)
                // scaler_e = 1 - gamma/alpha*scaler_d
                // e = scaler_e * e + scaler_d * d
                exec->run(multigrid::make_kcycle_step_2(
                    alpha.get(), rho.get(), gamma.get(), beta.get(), zeta.get(),
                    d.get(), e.get()));
            }
        }
        // prolong
        rstr_prlg->prolong_applyadd(e.get(), x);

        // post-smooth
        if (post_smoother) {
            r->copy_from(b);
            matrix->apply(neg_one, x, one, r.get());
            post_smoother->apply(post_relaxation.get(), r.get(), one, x);
        }
    }

    // current level's nrows x nrhs
    std::vector<std::shared_ptr<vec>> r_list;
    // next level's nrows x nrhs
    std::vector<std::shared_ptr<vec>> g_list;
    std::vector<std::shared_ptr<vec>> e_list;
    // Kcycle usage
    // 1 x nrhs
    std::vector<std::shared_ptr<vec>> alpha_list;
    std::vector<std::shared_ptr<vec>> beta_list;
    std::vector<std::shared_ptr<vec>> gamma_list;
    std::vector<std::shared_ptr<vec>> rho_list;
    std::vector<std::shared_ptr<vec>> zeta_list;
    std::vector<std::shared_ptr<norm_vec>> old_norm_list;
    std::vector<std::shared_ptr<norm_vec>> new_norm_list;
    // next level's nrows x nrhs
    std::vector<std::shared_ptr<vec>> v_list;
    std::vector<std::shared_ptr<vec>> w_list;
    std::vector<std::shared_ptr<vec>> d_list;
    std::shared_ptr<const Executor> exec;
    const LinOp *system_matrix;
    const Multigrid<ValueType> *multigrid;
    size_type nrhs;
    const vec *one;
    const vec *neg_one;
    size_type k_base;
    remove_complex<ValueType> rel_tol;
};


}  // namespace


template <typename ValueType>
void Multigrid<ValueType>::generate()
{
    // generate coarse matrix until reaching max_level or min_coarse_rows
    auto num_rows = system_matrix_->get_size()[0];
    size_type level = 0;
    auto matrix = system_matrix_;
    auto exec = this->get_executor();
    // Always generate smoother and relaxation with size = level.
    while (level < parameters_.max_levels &&
           num_rows > parameters_.min_coarse_rows) {
        auto index = rstr_prlg_index_(level, lend(matrix));
        GKO_ENSURE_IN_BOUNDS(index, parameters_.rstr_prlg.size());
        auto rstr_prlg_factory = parameters_.rstr_prlg.at(index);
        // coarse generate
        auto rstr = rstr_prlg_factory->generate(matrix);
        if (rstr->get_coarse_operator()->get_size()[0] == num_rows) {
            // do not reduce dimension
            break;
        }
        rstr_prlg_list_.emplace_back(give(rstr));
        // pre_smooth_generate
        handle_list(exec, index, matrix, parameters_.pre_smoother,
                    parameters_.pre_relaxation, pre_smoother_list_,
                    pre_relaxation_list_, one_op_);
        // mid_smooth_generate
        if (parameters_.mid_case == multigrid_mid_uses::mid) {
            handle_list(exec, index, matrix, parameters_.mid_smoother,
                        parameters_.mid_relaxation, mid_smoother_list_,
                        mid_relaxation_list_, one_op_);
        }
        // post_smooth_generate
        if (!parameters_.post_uses_pre) {
            handle_list(exec, index, matrix, parameters_.post_smoother,
                        parameters_.post_relaxation, post_smoother_list_,
                        post_relaxation_list_, one_op_);
        }
        matrix = rstr_prlg_list_.back()->get_coarse_operator();
        num_rows = matrix->get_size()[0];
        level++;
    }
    if (parameters_.post_uses_pre) {
        post_smoother_list_ = pre_smoother_list_;
        post_relaxation_list_ = pre_relaxation_list_;
    }
    if (parameters_.mid_case == multigrid_mid_uses::pre) {
        mid_smoother_list_ = pre_smoother_list_;
        mid_relaxation_list_ = pre_relaxation_list_;
    } else if (parameters_.mid_case == multigrid_mid_uses::post) {
        mid_smoother_list_ = post_smoother_list_;
        mid_relaxation_list_ = post_relaxation_list_;
    }
    // Generate at least one level
    GKO_ASSERT_EQ(level > 0, true);
    // generate coarsest solver
    if (parameters_.coarsest_solver.size() == 0) {
        // default is identity
        coarsest_solver_ =
            matrix::Identity<ValueType>::create(exec, matrix->get_size()[0]);
    } else {
        auto temp_index = solver_index_(level, lend(matrix));
        GKO_ENSURE_IN_BOUNDS(temp_index, parameters_.coarsest_solver.size());
        auto solver = parameters_.coarsest_solver.at(temp_index);
        if (solver == nullptr) {
            // default is identity
            coarsest_solver_ = matrix::Identity<ValueType>::create(
                exec, matrix->get_size()[0]);
        } else {
            coarsest_solver_ = solver->generate(matrix);
        }
    }
}


template <typename ValueType>
void Multigrid<ValueType>::apply_impl(const LinOp *b, LinOp *x) const
{
    auto exec = this->get_executor();
    constexpr uint8 RelativeStoppingId{1};
    Array<stopping_status> stop_status(exec, b->get_size()[1]);
    bool one_changed{};
    auto dense_x = gko::as<matrix::Dense<ValueType>>(x);
    auto dense_b = gko::as<matrix::Dense<ValueType>>(b);
    auto state = MultigridState<ValueType>(
        exec, system_matrix_.get(), this, b->get_size()[1], one_op_.get(),
        neg_one_op_.get(), parameters_.kcycle_base, parameters_.kcycle_rel_tol);
    exec->run(multigrid::make_initialize(&stop_status));
    // compute the residual at the r_list(0);
    auto r = state.r_list.at(0);
    r->copy_from(dense_b);
    system_matrix_->apply(neg_one_op_.get(), x, one_op_.get(), r.get());
    auto stop_criterion = stop_criterion_factory_->generate(
        system_matrix_, std::shared_ptr<const LinOp>(b, [](const LinOp *) {}),
        x, r.get());
    int iter = -1;
    while (true) {
        ++iter;
        this->template log<log::Logger::iteration_complete>(this, iter, r.get(),
                                                            dense_x);
        if (stop_criterion->update()
                .num_iterations(iter)
                .residual(r.get())
                .solution(dense_x)
                .check(RelativeStoppingId, true, &stop_status, &one_changed)) {
            break;
        }
        for (size_type i = 0; i < state.e_list.size(); i++) {
            auto e = state.e_list.at(i);
            exec->run(multigrid::make_fill_array(e->get_values(),
                                                 e->get_num_stored_elements(),
                                                 zero<ValueType>()));
        }
        state.run_cycle(cycle_, 0, system_matrix_, dense_b, dense_x);
        r->copy_from(dense_b);
        system_matrix_->apply(neg_one_op_.get(), x, one_op_.get(), r.get());
    }
}


template <typename ValueType>
void Multigrid<ValueType>::apply_impl(const LinOp *alpha, const LinOp *b,
                                      const LinOp *beta, LinOp *x) const
{
    auto dense_x = as<matrix::Dense<ValueType>>(x);

    auto x_clone = dense_x->clone();
    this->apply(b, x_clone.get());
    dense_x->scale(beta);
    dense_x->add_scaled(alpha, x_clone.get());
}


#define GKO_DECLARE_MULTIGRID(_type) class Multigrid<_type>
GKO_INSTANTIATE_FOR_EACH_VALUE_TYPE(GKO_DECLARE_MULTIGRID);


}  // namespace solver
}  // namespace gko
