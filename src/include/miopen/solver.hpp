/*******************************************************************************
*
* MIT License
*
* Copyright (c) 2017 Advanced Micro Devices, Inc.
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in all
* copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
* SOFTWARE.
*
*******************************************************************************/

#ifndef GUARD_MIOPEN_SOLVER_HPP_
#define GUARD_MIOPEN_SOLVER_HPP_

#include <miopen/config.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <ostream>

#include <miopen/logger.hpp>
#include <miopen/find_controls.hpp>
#include <miopen/db_record.hpp>
#include <miopen/mlo_internal.hpp>
#include <miopen/legacy_exhaustive_search.hpp>
#include <miopen/env.hpp>
#include <miopen/type_name.hpp>
#include <miopen/miopen.h>

namespace miopen {

MIOPEN_DECLARE_ENV_VAR(MIOPEN_DEBUG_AMD_ASM_KERNELS_PERF_FILTERING)

namespace solver {

/// Describes a kernel source and whatever information required in order
/// to build and run it (the former is unused for binary kernels).
struct KernelInfo
{
    std::string comp_options;
    std::vector<size_t> l_wk;
    std::vector<size_t> g_wk;
    std::string kernel_file;
    std::string kernel_name;
    friend std::ostream& operator<<(std::ostream& os, const KernelInfo& k);
};

/// Information required to build and run a kernel (or a set of kernels),
/// which is expected to perform computatons as per the problem config.
///
/// TODO: Currently best suits a subset of existing solvers,
/// namely some OpenCL-written forward direct convolutions.
/// Shall be refactored (possibly, to a class hierarchy).
struct ConvSolution
{
    std::vector<KernelInfo> construction_params; // impl may consist of multiple kernels.
    miopenStatus_t status;

    size_t workspce_sz;
    int grp_tile1;       // total number ALUs per group
    int grp_tile0;       // total number ALUs per group
    int in_tile1;        // size of in-tile in local memory
    int in_tile0;        // size of in-tile in local memory
    int out_pix_tile1;   // # of generated pixels per output per wk-item  (ALU)
    int out_pix_tile0;   // # of generated pixels per output per wk-item  (ALU)
    int n_out_pix_tiles; // # output pixel tiles per wk-item (ALU)
    int n_in_data_tiles; // # of blocks of different inputs in LDS
    int n_stacks;        // # of diff stacks (part of batch).

    ConvSolution(miopenStatus_t status_ = miopenStatusSuccess)
        : status(status_),
          workspce_sz(0),
          grp_tile1(-1),
          grp_tile0(-1),
          in_tile1(-1),
          in_tile0(-1),
          out_pix_tile1(-1),
          out_pix_tile0(-1),
          n_out_pix_tiles(-1),
          n_in_data_tiles(-1),
          n_stacks(-1)
    {
    }

    inline bool Succeeded() const { return status == miopenStatusSuccess; }
};

template <class Solver>
std::string ComputeSolverDbId(Solver)
{
    const auto& name = get_type_name<Solver>();
    auto idx         = name.find_last_of(':');
    return name.substr(idx + 1);
}

// This will retrieve the id of the solver to write to the database. By
// default it uses the class name. If the class is renamed, this function can
// overriden to keep the name to avoid DB corruption.
template <class Solver>
const std::string& SolverDbId(Solver solver)
{
    static const auto result = ComputeSolverDbId(solver);
    return result;
}

template <class Solver, class Context>
auto FindSolutionImpl(rank<1>, Solver s, const Context& context, DbRecord& dbRecord)
    -> decltype(s.GetSolution(context, s.Search(context)))
{
    const FindEnforce enforce;
    MIOPEN_LOG_I(SolverDbId(s));
    if(enforce.IsDbClean(context))
    {
        if(dbRecord.Remove(SolverDbId(s)))
            MIOPEN_LOG_W("Perf Db: record removed: " << SolverDbId(s) << ", enforce: " << enforce);
    }
    else
    {
        if((context.do_search || enforce.IsSearch(context)) && enforce.IsDbUpdate(context))
        {
            MIOPEN_LOG_W("Perf Db: load skipped: " << SolverDbId(s) << ", enforce: " << enforce);
        }
        else
        {
            using PerformanceConfig = decltype(s.GetPerformanceConfig(context));
            PerformanceConfig config{};
            if(dbRecord.Load(SolverDbId(s), config))
            {
                MIOPEN_LOG_I("Perf Db: record loaded: " << SolverDbId(s));
                if(s.IsValidPerformanceConfig(context, config))
                {
                    return s.GetSolution(context, config);
                }
                MIOPEN_LOG_E("Invalid config loaded from Perf Db: " << SolverDbId(s) << ": "
                                                                    << config);
            }
        }

        if(context.do_search || enforce.IsSearch(context)) // TODO: Make it a customization point
        {
            MIOPEN_LOG_I("Starting search: " << SolverDbId(s) << ", enforce: " << enforce);
            try
            {
                auto c = s.Search(context);
                dbRecord.Store(SolverDbId(s), c);
                return s.GetSolution(context, c);
            }
            catch(const miopen::Exception& ex)
            {
                MIOPEN_LOG_E("Search failed for: " << SolverDbId(s) << ": " << ex.what());
            }
        }
    }
    return s.GetSolution(context, s.GetPerformanceConfig(context));
}

template <class Solver, class Context>
auto FindSolutionImpl(rank<0>, Solver s, const Context& context, DbRecord&)
    -> decltype(s.GetSolution(context))
{
    MIOPEN_LOG_I("Not searchable: " << SolverDbId(s));
    return s.GetSolution(context);
}

/// Finds optimized Solution. Generic method.
///
/// Given the specific problem config, finds (hopefully) optimal
/// solution-specific parameters and returns the Solution object.
/// Could take long if an exhaustive search is requested/performed.
/// May read/write perfDb.
template <class Solver, class Context>
ConvSolution FindSolution(Solver s, const Context& context, DbRecord& dbRecord)
{
    static_assert(std::is_empty<Solver>{} && std::is_trivially_constructible<Solver>{},
                  "Solver must be stateless");
    // TODO: This assumes all solutions are ConvSolution
    return FindSolutionImpl(rank<1>{}, s, context, dbRecord);
}

/// \todo Copypasted from GenericSearch; generalize.
inline void InitRandomly_(std::vector<float>& vec, const double offset, const double factor)
{
    float* p = vec.data();
    for(int i = 0; i < vec.size(); ++i)
        *p++ = static_cast<float>((rand() * (1.0 / RAND_MAX) + offset) * factor);
}

/// \todo Copypasted from GenericSearch; generalize.
inline void InitRandomly_(std::vector<float>& vec)
{
    float* p = vec.data();
    for(int i = 0; i < vec.size(); ++i)
        *p++ = static_cast<float>(rand() * (1.0 / RAND_MAX));
}

// Search for a solution among many solvers
template <class... Solvers, class Context>
auto SearchForSolution(const Context& search_params, miopen::DbRecord dbRecord) ->
    typename std::common_type<decltype(FindSolution(Solvers{}, search_params, dbRecord))...>::type
{
    using Solution = typename std::common_type<decltype(
        FindSolution(Solvers{}, search_params, dbRecord))...>::type;
    Solution solution{miopenStatusUnknownError};

// Using const here causes gcc to ICE
#if(!defined(__GNUC__) || defined(__clang__))
    const
#endif
        auto no_perf_filtering = miopen::IsDisabled(MIOPEN_DEBUG_AMD_ASM_KERNELS_PERF_FILTERING{});

#if 1
    if(search_params.direction.IsBackwardWrW() &&
       search_params.kernel_stride0 <= 1) /// \todo Remove this w/a for subsampling cases
    {
        float best_time                = std::numeric_limits<float>::max();
        bool none_probing_failures_yet = true;
        MIOPEN_STATIC_FOR_EACH(
            solver,
            Solvers{},
            {
                Solution candidate{miopenStatusUnknownError};
                if(solver.IsApplicable(search_params) &&
                   (no_perf_filtering || solver.IsFast(search_params)))
                {
                    candidate = FindSolution(solver, search_params, dbRecord);
                    if(candidate.Succeeded())
                    {
                        MIOPEN_LLOG_I2(SolverDbId(solver) << ": Success."
                                                          << " construction_params.empty()="
                                                          << solution.construction_params.empty());
                    }
                    if(candidate.Succeeded() && candidate.construction_params.empty())
                    {
                        candidate = {miopenStatusInternalError};
                        MIOPEN_THROW_DEBUG(std::string("Internal error in solver: ") +
                                           SolverDbId(solver));
                    }
                    float elapsed_time;
                    if(candidate.Succeeded())
                    { // Return the fastest solution.
                        /// \todo Avoid multiple preparations of buffers
                        /// \todo Copypasted from GenericSearch; generalize.
                        auto& context = search_params; /// \todo Remove synonym
                        miopen::Handle profile_h;
                        std::vector<float> bot(context.bot_sz / sizeof(float));
                        std::vector<float> top(context.top_sz / sizeof(float));
                        std::vector<float> wei(context.weights_sz / sizeof(float));
                        std::vector<float> bias(context.bias_sz / sizeof(float));
                        if(!context.direction.IsForward())
                            InitRandomly_(bot);
                        if(!context.direction.IsBackwardData())
                            InitRandomly_(top);
                        if(!context.direction.IsBackwardWrW())
                            InitRandomly_(wei, -0.5, 0.001);
                        if(context.bias)
                            InitRandomly_(bias);
                        auto bot_ocl_buf  = profile_h.Write(bot);
                        auto top_ocl_buf  = profile_h.Write(top);
                        auto wei_ocl_buf  = profile_h.Write(wei);
                        auto bias_ocl_buf = context.bias ? profile_h.Write(bias) : nullptr;

                        profile_h.EnableProfiling(true);
                        auto rc = solver.RunAndMeasureSolution(profile_h,
                                                               bot_ocl_buf.get(),
                                                               top_ocl_buf.get(),
                                                               wei_ocl_buf.get(),
                                                               context.bias ? bias_ocl_buf.get()
                                                                            : nullptr,
                                                               context,
                                                               candidate,
                                                               elapsed_time);
                        profile_h.EnableProfiling(false);
                        /// (1) Select the fastest solution.
                        /// (2) There could be Solvers which do not have time measurement
                        ///     implemented, so those can't be timed.
                        /// Some related assumptions:
                        /// * (3) Any solution which does support time measurement is faster than
                        ///       any solution that can't be timed.
                        /// * (4) Among all solutions which can't be timed,
                        ///       the one which is constructed first is the fastest.
                        ///       This corresponds to the legacy (euristic) behavior.
                        ///
                        /// \todo Implement probing for all Solvers.
                        switch(rc)
                        {
                        case 0:
                            MIOPEN_LLOG_I2(SolverDbId(solver) << ": Timing OK " << elapsed_time);
                            if(elapsed_time < best_time) // (1)
                            {
                                MIOPEN_LLOG_I(SolverDbId(solver) << ": " << elapsed_time << " < "
                                                                 << best_time);
                                best_time = elapsed_time;
                                solution  = candidate;
                            }
                            break;
                        case -2: // (2)
                            MIOPEN_LLOG_W(SolverDbId(solver) << ": Timing not implemented");
                            if(none_probing_failures_yet)
                            { // (4)
                                none_probing_failures_yet = false;
                                if(best_time >= std::numeric_limits<float>::max())
                                { // (3)
                                    solution = candidate;
                                }
                            }
                            break;
                        default: MIOPEN_LLOG_E(SolverDbId(solver) << ": Timing failed"); break;
                        }
                    }
                }
                else
                {
                    MIOPEN_LLOG_I2(SolverDbId(solver) << ": N/A");
                }
            });
    }
    else
#endif
    {
        // clang-format off
        MIOPEN_STATIC_FOR_EACH(solver, Solvers{}, {
            if(!solution.Succeeded() && solver.IsApplicable(search_params) &&
               (no_perf_filtering || solver.IsFast(search_params)))
            {
                solution = FindSolution(solver, search_params, dbRecord);
                if(solution.Succeeded() && solution.construction_params.empty())
                {
                    MIOPEN_THROW(std::string("Internal error in solver: ") + SolverDbId(solver));
                }
            }
        });
        // clang-format on
    }

    return solution;
}

// Search for all applicable solutions among many solvers
template <class... Solvers, class Context, class Solution>
void SearchForAllSolutions(const Context& search_params,
                           miopen::DbRecord dbRecord,
                           std::vector<Solution>& ss)
{
    assert(ss.empty());
    assert(search_params.direction.IsBackwardWrW() && search_params.kernel_stride0 <= 1);

// Using const here causes gcc to ICE
#if(!defined(__GNUC__) || defined(__clang__))
    const
#endif
        auto no_perf_filtering = miopen::IsDisabled(MIOPEN_DEBUG_AMD_ASM_KERNELS_PERF_FILTERING{});

    MIOPEN_STATIC_FOR_EACH(solver,
                           Solvers{},
                           {
                               if(solver.IsApplicable(search_params) &&
                                  (no_perf_filtering || solver.IsFast(search_params)))
                               {
                                   const Solution s = FindSolution(solver, search_params, dbRecord);
                                   if(s.Succeeded())
                                   {
                                       if(s.construction_params.empty())
                                       {
                                           MIOPEN_LLOG_E(SolverDbId(solver) << ": Internal error.");
                                           MIOPEN_THROW_DEBUG(
                                               std::string("Internal error in solver: ") +
                                               SolverDbId(solver));
                                       }
                                       else
                                       {
                                           ss.push_back(s);
                                           MIOPEN_LLOG_I2(SolverDbId(solver) << ": Success.");
                                       }
                                   }
                               }
                               else
                               {
                                   MIOPEN_LLOG_I2(SolverDbId(solver) << ": N/A");
                               }
                           });
}

/// Base class for problem solvers.
///
/// Solvers are to be instantiated as const objects and shall not have any variable
/// internal state. Any non-const state information, if required, to be stored in the
/// solver-specific context objects.
///
/// There could be multiple solvers of the same algorithm for a problem config.
/// For example, ConvAsm3x3U and ConvOclDirectFwd3x3
/// are able to solve overlapping sets of 3x3 Direct convolution problems.
template <class Context>
struct SolverBase
{

    /// Initializes performance config to the default values.
    /// The function may involve some euristic to guess the best solution
    /// configuration. It is assumed that the function takes constant time
    /// to finish and does not run kernels to measure performance etc.
    /// The function shall always return valid config.
    /// Only implemented by SearchableSolvers
    /// PerformanceConfig GetPerformanceConfig(const ConvolutionContext&) const;

    /// Should return false if performance config is wrong for a problem.
    /// Main use is validation of values read from the perf db.
    /// Only implemented by SearchableSolvers
    /// bool IsValidPerformanceConfig(const Context&, const PerformanceConfig&) const
    /// {
    ///     return true; // Do not check by default.
    /// }

    /// Returns true if solution can work on given SW/HW platform (runtime/device)
    /// and provides correct result for the problem config.
    ///
    /// Every SolverBase which IsApplicable() for some problem config must be able to
    /// GetPerformanceConfig() so that GetSolution() would return valid
    /// solution for a problem (i.e. convolution). In other words, if a Solution
    /// says "I'm suitable" for a problem, it agrees to solve that problem correctly.
    bool IsApplicable(const Context&) const { return true; }

    /// Legacy euristic method which shall return false when a solution
    /// is known to be slower than some another solution for the same problem config.
    /// Intended to be used for performance optimization.
    /// Warning: Non-trivial implementations introduce implicit dependencies between solutions.
    bool IsFast(const Context&) const { return true; }

    /// Takes problem config, optimization parameters and other info
    /// and computes information required to build and run the kernel(s).
    /// ConvSolution GetSolution(const ConvolutionContext& params) const;

    /// Searchable solvers provide a GetSolution that takes a Context and PerformanceConfig
    /// ConvSolution GetSolution(const ConvolutionContext& params,
    ///                          const PerformanceConfig& config) const;

    /// Solver-specific method.
    /// \todo Default implementation. To be removed when implemented in all Solvers.
    int RunAndMeasureSolution(miopen::Handle&, // profile_h,
                              Data_t,          // bot_ocl_buf,
                              Data_t,          // top_ocl_buf,
                              Data_t,          // wei_ocl_buf,
                              Data_t,          // bias_ocl_buf,
                              const Context&,
                              const ConvSolution&,
                              float&) const
    {
        return -2; // Not implemented
    }
};

struct PerformanceConfigConvAsm3x3U : Serializable<PerformanceConfigConvAsm3x3U>
{
    int limit_wave_cnt;        // [0..9]
    int filters_per_wave;      // [1..8]
    int output_lines_per_wave; // [1..8]

    PerformanceConfigConvAsm3x3U(int lwc, int fpw, int olpw);
    PerformanceConfigConvAsm3x3U() : PerformanceConfigConvAsm3x3U(-1, -1, -1) {}
    PerformanceConfigConvAsm3x3U(bool) : PerformanceConfigConvAsm3x3U(0, 1, 1) {}

    template <class Self, class F>
    static void Visit(Self&& self, F f)
    {
        f(self.limit_wave_cnt, "limit_wave_cnt");
        f(self.filters_per_wave, "filters_per_wave");
        f(self.output_lines_per_wave, "output_lines_per_wave");
    }

    void EuristicInit(const ConvolutionContext& config);
    bool IsValidValue() const;
    bool SetNextValue();
    bool IsValid(const ConvolutionContext& config) const;
    bool operator==(const PerformanceConfigConvAsm3x3U& other) const;
    std::string ToString() const;
};

struct ConvAsm3x3U : SolverBase<ConvolutionContext>
{
    bool IsApplicable(const ConvolutionContext& params) const;
    bool IsFast(const ConvolutionContext& params) const;
    PerformanceConfigConvAsm3x3U GetPerformanceConfig(const ConvolutionContext&) const;
    bool IsValidPerformanceConfig(const ConvolutionContext&,
                                  const PerformanceConfigConvAsm3x3U&) const;
    PerformanceConfigConvAsm3x3U Search(const ConvolutionContext&) const;
    ConvSolution GetSolution(const ConvolutionContext& params,
                             const PerformanceConfigConvAsm3x3U& config,
                             bool disableConfigOverrideFromEnv = false) const;
    int RunAndMeasureSolution(miopen::Handle& profile_h,
                              Data_t bot_ocl_buf,
                              Data_t top_ocl_buf,
                              Data_t wei_ocl_buf,
                              Data_t bias_ocl_buf,
                              const ConvolutionContext& params,
                              const ConvSolution& solution,
                              float& elapsed_time) const;
};

struct ConvAsm5x10u2v2f1 : SolverBase<ConvolutionContext>
{
    bool IsApplicable(const ConvolutionContext& params) const;
    ConvSolution GetSolution(const ConvolutionContext& params) const;
};

struct ConvAsm5x10u2v2b1 : SolverBase<ConvolutionContext>
{
    bool IsApplicable(const ConvolutionContext& params) const;
    ConvSolution GetSolution(const ConvolutionContext& params) const;
};

struct ConvAsm7x7c3h224w224k64u2v2p3q3f1 : SolverBase<ConvolutionContext>
{
    bool IsApplicable(const ConvolutionContext& params) const;
    ConvSolution GetSolution(const ConvolutionContext& params) const;
};

struct ConvOclDirectFwd11x11 : SolverBase<ConvolutionContext>
{
    bool IsApplicable(const ConvolutionContext& params) const;
    ConvSolution GetSolution(const ConvolutionContext& params) const;
};

struct ConvOclDirectFwdGen : SolverBase<ConvolutionContext>
{
    bool IsApplicable(const ConvolutionContext& params) const;
    ConvSolution GetSolution(const ConvolutionContext& params) const;
};

struct ConvOclDirectFwd3x3 : SolverBase<ConvolutionContext>
{
    bool IsApplicable(const ConvolutionContext& params) const;
    ConvSolution GetSolution(const ConvolutionContext& params) const;
};

/// Holds common member functions for the Solvers which share the same
/// "legacy exhaustive search" machinery.
struct ConvOclDirectFwdLegacyExhaustiveSearch : SolverBase<ConvolutionContext>
{
    LegacyPerformanceConfig GetPerformanceConfig(const ConvolutionContext&) const;
    bool IsValidPerformanceConfig(const ConvolutionContext&, const LegacyPerformanceConfig&) const
    {
        return true; // Do not check by default.
    }
    LegacyPerformanceConfig Search(const ConvolutionContext&) const;
};

struct ConvOclDirectFwd : ConvOclDirectFwdLegacyExhaustiveSearch
{
    ConvSolution GetSolution(const ConvolutionContext& params,
                             const LegacyPerformanceConfig& searched_params) const;
};

struct ConvOclDirectFwd1x1 : ConvOclDirectFwdLegacyExhaustiveSearch
{
    bool IsApplicable(const ConvolutionContext& params) const;
    ConvSolution GetSolution(const ConvolutionContext& params,
                             const LegacyPerformanceConfig& searched_params) const;
};

struct ConvOclDirectFwdC : ConvOclDirectFwdLegacyExhaustiveSearch
{
    bool IsApplicable(const ConvolutionContext& params) const;
    ConvSolution GetSolution(const ConvolutionContext& params,
                             const LegacyPerformanceConfig& searched_params) const;
};

struct ConvBinWinograd3x3U : SolverBase<ConvolutionContext>
{
    bool IsApplicable(const ConvolutionContext& params) const;
    ConvSolution GetSolution(const ConvolutionContext& params) const;
};

struct ConvBinWinogradRxS : SolverBase<ConvolutionContext>
{
    bool IsApplicable(const ConvolutionContext& params) const;
    ConvSolution GetSolution(const ConvolutionContext& params) const;
};

struct PerformanceConfigAsmDirect3x3WrW : Serializable<PerformanceConfigAsmDirect3x3WrW>
{
    int limit_wave_cnt;   // [0..9]
    int reverse_inout;    // [0..1], 1 is allowed for stride=1x1 only.
    int chunk_size;       // {16,8}, Smaller values increase register pressure.
    int k_per_wave;       // {1,2,4,8} && ((chunk_size * k_per_wave) <= 64).
                          // Higher values increase register pressure.
    int pipe_lines_depth; // [1..16] && (pipe_lines_depth <= img_h).
                          // Higher values increase register pressure.
    int n_per_group;      // [1..8] && (n_per_group <= batch_size).

    PerformanceConfigAsmDirect3x3WrW(int lwc, int rio, int csz, int kpw, int pld, int npg);
    PerformanceConfigAsmDirect3x3WrW() : PerformanceConfigAsmDirect3x3WrW(-1, -1, -1, -1, -1, -1) {}
    PerformanceConfigAsmDirect3x3WrW(bool) : PerformanceConfigAsmDirect3x3WrW(0, 0, 8, 1, 1, 1) {}

    template <class Self, class F>
    static void Visit(Self&& self, F f)
    {
        f(self.limit_wave_cnt, "limit_wave_cnt");
        f(self.reverse_inout, "reverse_inout");
        f(self.chunk_size, "chunk_size");
        f(self.k_per_wave, "k_per_wave");
        f(self.pipe_lines_depth, "pipe_lines_depth");
        f(self.n_per_group, "n_per_group");
    }

    // clang-format off
    int GetLimitWaveCnt() const { return limit_wave_cnt; }
    int GetReverseInout() const { return reverse_inout; }
    int GetChunkSize() const { return chunk_size; }
    int GetKPerWave() const { return k_per_wave; }
    int GetPipeLinesDepth() const { return pipe_lines_depth; }
    int GetNPerGroup() const { return n_per_group; } 
    int GetCPerWave() const { assert(chunk_size); return 64 / chunk_size; } // clang-format on

    void EuristicInit(const ConvolutionContext& config);
    bool IsValidValue() const;
    bool SetNextValue();
    bool IsValid(const ConvolutionContext& config) const;
    bool operator==(const PerformanceConfigAsmDirect3x3WrW& other) const;
    std::string ToString() const;
};

struct ConvAsmBwdWrW3x3 : SolverBase<ConvolutionContext>
{
    PerformanceConfigAsmDirect3x3WrW GetPerformanceConfig(const ConvolutionContext&) const;
    bool IsValidPerformanceConfig(const ConvolutionContext&,
                                  const PerformanceConfigAsmDirect3x3WrW&) const;
    PerformanceConfigAsmDirect3x3WrW Search(const ConvolutionContext&) const;
    bool IsApplicable(const ConvolutionContext& params) const;
    bool IsFast(const ConvolutionContext& params) const;
    ConvSolution GetSolution(const ConvolutionContext& params,
                             const PerformanceConfigAsmDirect3x3WrW& config,
                             bool disableConfigOverrideFromEnv = false) const;
    int RunAndMeasureSolution(miopen::Handle& profile_h,
                              Data_t bot_ocl_buf,
                              Data_t top_ocl_buf,
                              Data_t wei_ocl_buf,
                              Data_t bias_ocl_buf,
                              const ConvolutionContext& params,
                              const ConvSolution& solution,
                              float& elapsed_time) const;
};

struct PerformanceConfigConvAsmBwdWrW1x1 : Serializable<PerformanceConfigConvAsmBwdWrW1x1>
{
    int c_per_gpr; // {1,2,4,8,16}
    int c_mult;    // {1,2,4,8,16}
    int k_per_gpr; // {1,2,4,8,16}
    int k_mult;    // {1,2,4,8,16}
    int read_size; // [1..4]
    int n_per_gpr; // {1,2,4}

    /// The following conditions must be met.
    ///
    /// Shader design-related constraints:
    /// - (A) (chunk_size * c_per_gpr) == 16
    /// - (B) k_per_gpr <= c_per_gpr
    /// - (C) (c_mult > 1 || k_mult > 1)
    ///         ? ((fwd_C % (c_per_gpr * c_mult) == 0) && (fwd_K % (k_per_gpr * k_mult) == 0))
    ///         : (true)
    ///
    /// Resource-related constraints:
    /// - (D) c_mult * k_mult * k_per_gpr + 9 + (c_mult + k_mult) * read_size * pipe_depth <= 256
    ///
    /// Where:
    /// - fwd_C := Num input channels for forward convolution (-c).
    ///   For backward, this is actually n_outputs.
    /// - fwd_K := Num output channels for forward convolution (-k).
    ///   For backward, this is actually n_inputs.

    PerformanceConfigConvAsmBwdWrW1x1(
        int c_per_gpr_, int c_mult_, int k_per_gpr_, int k_mult_, int read_size_, int n_per_gpr_);
    PerformanceConfigConvAsmBwdWrW1x1() : PerformanceConfigConvAsmBwdWrW1x1(-1, -1, -1, -1, -1, -1)
    {
    }
    PerformanceConfigConvAsmBwdWrW1x1(bool) : PerformanceConfigConvAsmBwdWrW1x1(1, 1, 1, 1, 1, 1) {}

    template <class Self, class F>
    static void Visit(Self&& self, F f)
    {
        f(self.c_per_gpr, "c_per_gpr");
        f(self.c_mult, "c_mult");
        f(self.k_per_gpr, "k_per_gpr");
        f(self.k_mult, "k_mult");
        f(self.read_size, "read_size");
        f(self.n_per_gpr, "n_per_gpr");
    }

    // clang-format off
    int GetNPerGpr() const { return n_per_gpr; }
    int GetPipeDepth() const { return 1; }
    int GetCPerGpr() const { return c_per_gpr; }
    int GetCMult() const { return c_mult; }
    int GetKPerGpr() const { return k_per_gpr; }
    int GetKMult() const { return k_mult; }
    int GetReadSize() const { return read_size; }
    int GetChunkSize() const { assert(c_per_gpr); return 16 / c_per_gpr; }
    int GetHwPerGpr() const { assert(n_per_gpr); return 4 / n_per_gpr; } // "hw" stands for "height-and-width".
    // clang-format on

    void EuristicInit(const ConvolutionContext& config);
    bool IsValidValue() const;
    bool SetNextValue();
    bool IsValid(const ConvolutionContext& config) const;
    bool operator==(const PerformanceConfigConvAsmBwdWrW1x1& other) const;
    std::string ToString() const;
};

struct ConvAsmBwdWrW1x1 : SolverBase<ConvolutionContext>
{
    PerformanceConfigConvAsmBwdWrW1x1 GetPerformanceConfig(const ConvolutionContext&) const;
    bool IsValidPerformanceConfig(const ConvolutionContext&,
                                  const PerformanceConfigConvAsmBwdWrW1x1&) const;
    PerformanceConfigConvAsmBwdWrW1x1 Search(const ConvolutionContext&) const;
    bool IsApplicable(const ConvolutionContext& params) const;
    bool IsFast(const ConvolutionContext& params) const;
    ConvSolution GetSolution(const ConvolutionContext& params,
                             const PerformanceConfigConvAsmBwdWrW1x1& config,
                             bool disableConfigOverrideFromEnv = false) const;
    int RunAndMeasureSolution(miopen::Handle& profile_h,
                              Data_t bot_ocl_buf,
                              Data_t top_ocl_buf,
                              Data_t wei_ocl_buf,
                              Data_t bias_ocl_buf,
                              const ConvolutionContext& params,
                              const ConvSolution& solution,
                              float& elapsed_time) const;
};

struct ConvOclBwdWrW2 : SolverBase<ConvolutionContext>
{
    bool IsApplicable(const ConvolutionContext& params) const;
    ConvSolution GetSolution(const ConvolutionContext& params) const;
};

struct ConvOclBwdWrW53 : SolverBase<ConvolutionContext>
{
    bool IsApplicable(const ConvolutionContext& params) const;
    ConvSolution GetSolution(const ConvolutionContext& params) const;
};

struct ConvOclBwdWrW1x1 : SolverBase<ConvolutionContext>
{
    bool IsApplicable(const ConvolutionContext& params) const;
    ConvSolution GetSolution(const ConvolutionContext& params) const;
    int RunAndMeasureSolution(miopen::Handle& profile_h,
                              Data_t bot_ocl_buf,
                              Data_t top_ocl_buf,
                              Data_t wei_ocl_buf,
                              Data_t bias_ocl_buf,
                              const ConvolutionContext&,
                              const ConvSolution& solution,
                              float& elapsed_time) const;
};

} // namespace solver
} // namespace miopen

#endif // GUARD_MIOPEN_SOLVER_HPP_
