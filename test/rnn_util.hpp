/*******************************************************************************
 *
 * MIT License
 *
 * Copyright (c) 2023 Advanced Micro Devices, Inc.
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

#ifndef MIOPEN_RNN_UTIL_H_
#define MIOPEN_RNN_UTIL_H_

#include <cfloat>
#include <cmath>
#include <initializer_list>
#include <set>
#include <vector>
#include <cstdlib>
#include "random.hpp"

#define RNN_MM_TRANSPOSE 1
#define RNN_MM_USEPARAGEMM 0

inline void createTensorDescArray(std::vector<miopen::TensorDescriptor>& td,
                                  std::vector<miopenTensorDescriptor_t>& ptd,
                                  const std::vector<int> bs,
                                  const int secondDim,
                                  miopenDataType_t dataType)
{

    std::transform(bs.begin(), bs.end(), std::back_inserter(td), [&](int x) {
        return miopen::TensorDescriptor(
            dataType, {static_cast<std::size_t>(x), static_cast<std::size_t>(secondDim)});
    });
    std::transform(td.begin(), td.end(), std::back_inserter(ptd), [](miopen::TensorDescriptor& x) {
        return &x;
    });
}

inline std::tuple<size_t, size_t>
GetTempPackedBuffersSize(std::vector<int> batchs, int in_vec, int out_vec)
{
    size_t total_batch = std::accumulate(batchs.begin(), batchs.end(), 0);

    size_t in_buff_size  = total_batch * in_vec;
    size_t out_buff_size = total_batch * out_vec;
    return {in_buff_size, out_buff_size};
}

inline size_t getSuperTensorSize(const std::vector<int>& bs,
                                 int seqLength,
                                 int inputSize,
                                 int hiddenSize,
                                 int maxPaddingVal,
                                 bool isBidirect,
                                 bool isInput,
                                 bool isPadded)
{
    return static_cast<size_t>(isPadded ? seqLength * maxPaddingVal
                                        : std::accumulate(bs.begin(), bs.end(), 0)) *
           static_cast<size_t>(isInput ? inputSize : hiddenSize * (isBidirect ? 2 : 1));
}

template <typename Tgpu>
void ChangeDataPadding(const std::vector<Tgpu>& src_array,
                       std::vector<Tgpu>& dst_array,
                       const std::vector<int>& batch_list,
                       int max_batch,
                       int sample_size,
                       bool is_src_packed)
{
    auto seq_len = batch_list.size();

    auto scr_ptr = &src_array[0];
    auto dst_ptr = &dst_array[0];

    for(int seq_id = 0; seq_id < seq_len; seq_id++)
    {
        auto packed_size = batch_list[seq_id] * sample_size;

        std::copy(scr_ptr, scr_ptr + packed_size, dst_ptr);

        if(is_src_packed)
        {
            dst_ptr += max_batch * sample_size;
            scr_ptr += packed_size;
        }
        else
        {
            scr_ptr += max_batch * sample_size;
            dst_ptr += packed_size;
        }
    }
}

// RNN VANILLA configs
inline std::vector<int> get_rnn_num_layers() { return {{1, 3}}; }

inline std::vector<int> get_rnn_batchSize() { return {{1, 17}}; }

inline std::vector<int> get_rnn_seq_len() { return {{1, 3, 51}}; }

inline std::vector<int> get_rnn_vector_len() { return {31}; }

inline std::vector<int> get_rnn_hidden_size() { return {127}; }

// LSTM configs
inline std::vector<int> get_lstm_num_layers() { return {{1, 3}}; }

inline std::vector<int> get_lstm_batchSize() { return {{1, 17}}; }

inline std::vector<int> get_lstm_seq_len() { return {{1, 25}}; }

inline std::vector<int> get_lstm_vector_len() { return {17}; }

inline std::vector<int> get_lstm_hidden_size() { return {67}; }

// GRU configs
inline std::vector<int> get_gru_num_layers() { return {{1, 3}}; }

inline std::vector<int> get_gru_batchSize() { return {{1, 17}}; }

inline std::vector<int> get_gru_seq_len() { return {{1, 23}}; }

inline std::vector<int> get_gru_vector_len() { return {13}; }

inline std::vector<int> get_gru_hidden_size() { return {67}; }

inline std::vector<std::vector<int>> generate_batchSeq(const int batchSize, const int seqLength)
{

    static constexpr int modval = 3;

    int currentval = batchSize;
    std::vector<int> batchSeq;
    batchSeq.reserve(seqLength);
    for(int i = 0; i < seqLength; i++)
    {
        if(i > 0)
        {
            int nvalue = currentval - prng::gen_0_to_B(modval);
            currentval = (nvalue < 1) ? 1 : nvalue;
            // printf("current value: %d\n", currentval);
        }
        // printf("adding a value to batch sequence: %d\n", currentval);
        batchSeq.push_back(currentval);
    }
    return {batchSeq};
}

inline int sumvc(const std::vector<int>& x) { return std::accumulate(x.begin(), x.end(), 0); }

template <typename T>
inline T activfunc(T x, int actvf)
{
    T alpha = static_cast<T>(1), beta0 = static_cast<T>(0), beta1 = static_cast<T>(1);
    if(actvf == 0)
    {
        return (x > 0) ? x : x * beta0;
    }
    else if(actvf == 2)
    {
        return static_cast<T>(1 / (1 + std::exp(-x)));
    }
    return static_cast<T>(alpha * std::tanh(beta1 * x));
}

template <typename T>
inline T dervactivfunc(T x, int actvf)
{
    if(actvf == 0)
    {
        return static_cast<T>(x > 0 ? 1 : 0);
    }
    else if(actvf == 2)
    {
        return static_cast<T>(std::exp(-x) / (1 + std::exp(-x)) / (1 + std::exp(-x)));
    }

    return static_cast<T>(1 / std::cosh(x) / std::cosh(x));
}

template <typename Dtype>
void RNN_mm_cpu(const Dtype* a_ptr,
                size_t a_cols,
                size_t a_rows,
                size_t a_stride,
                int a_flags,
                const Dtype* b_ptr,
                size_t b_cols,
                size_t b_rows,
                size_t b_stride,
                int b_flags,
                Dtype* c_ptr,
                size_t c_cols,
                size_t c_rows,
                size_t c_stride,
                int /*c_flags*/,
                double d_alpha,
                double d_beta)
{

    auto alpha = Dtype(d_alpha);
    auto beta  = Dtype(d_beta);
    if((!(a_flags & RNN_MM_TRANSPOSE) && !(b_flags & RNN_MM_TRANSPOSE) &&
        ((a_cols != b_rows) || (a_rows != c_rows) || (b_cols != c_cols))) ||
       ((a_flags & RNN_MM_TRANSPOSE) && (b_flags & RNN_MM_TRANSPOSE) &&
        ((a_rows != b_cols) || (a_cols != c_rows) || (b_rows != c_cols))) ||
       ((a_flags & RNN_MM_TRANSPOSE) && !(b_flags & RNN_MM_TRANSPOSE) &&
        ((a_rows != b_rows) || (a_cols != c_rows) || (b_cols != c_cols))) ||
       (!(a_flags & RNN_MM_TRANSPOSE) && (b_flags & RNN_MM_TRANSPOSE) &&
        ((a_cols != b_cols) || (a_rows != c_rows) || (b_rows != c_cols))))
    {
        std::cout << "MM_CPU ERROR: " << a_cols << ", " << a_rows << "   " << b_cols << ", "
                  << b_rows << "   " << c_cols << ", " << c_rows << std::endl;
        return;
    }

    size_t inner_loop = (!(a_flags & RNN_MM_TRANSPOSE)) ? a_cols : a_rows;
#if(!RNN_MM_USEPARAGEMM)
    if(!(a_flags & RNN_MM_TRANSPOSE) && !(b_flags & RNN_MM_TRANSPOSE))
    {

        for(size_t n = 0; n < c_rows; ++n)
        {
            for(size_t k = 0; k < c_cols; ++k)
            {
                double mm_e = 0;
                for(size_t m = 0; m < inner_loop; ++m)
                {
                    mm_e += a_ptr[n * a_stride + m] * b_ptr[m * b_stride + k];
                }
                c_ptr[n * c_stride + k] = beta * c_ptr[n * c_stride + k] + alpha * mm_e;
            }
        }
    }
    else if((a_flags & RNN_MM_TRANSPOSE) && !(b_flags & RNN_MM_TRANSPOSE))
    {
        for(size_t n = 0; n < c_rows; ++n)
        {
            for(size_t k = 0; k < c_cols; ++k)
            {

                double mm_e = 0;
                for(size_t m = 0; m < inner_loop; ++m)
                {
                    mm_e += a_ptr[m * a_stride + n] * b_ptr[m * b_stride + k];
#if 0
					if (
						(n == 0 && k == 33
						|| n == 1 && k == 32
						|| n == 3 && k == 1
						|| n == 4 && k == 0

						)
						&& a_ptr[m*a_stride + n] * b_ptr[m*b_stride + k] != 0
						)
					{
                        std::cout << "C:mm: " << n << ", " << k << ", " << m << ", " <<
							mm_e, a_ptr[m*a_stride + n] << ", " <<
                                b_ptr[m*b_stride + k] << ", " <<
                                a_ptr[m*a_stride + n] * b_ptr[m*b_stride + k] << std::endl;
					}
#endif
                }
                c_ptr[n * c_stride + k] = beta * c_ptr[n * c_stride + k] + alpha * mm_e;
            }
        }
    }
    else if(!(a_flags & RNN_MM_TRANSPOSE) && (b_flags & RNN_MM_TRANSPOSE))
    {
        for(size_t n = 0; n < c_rows; ++n)
        {
            for(size_t k = 0; k < c_cols; ++k)
            {
                double mm_e = 0;

                for(size_t m = 0; m < inner_loop; ++m)
                {
                    mm_e += a_ptr[n * a_stride + m] * b_ptr[k * b_stride + m];
#if 0
					if (n == 0 && k == 6 && a_ptr[n*a_stride + m] * b_ptr[k*b_stride + m] != 0)
					{
                        std::cout << m << ", " << mm_e << ", " << ", " << a_ptr[n*a_stride + m] << ", " << b_ptr[k*b_stride + m] << std::endl;
					}
#endif
                }
                c_ptr[n * c_stride + k] = beta * c_ptr[n * c_stride + k] + alpha * mm_e;
            }
        }
    }
    else
    {
        for(size_t n = 0; n < c_rows; ++n)
        {
            for(size_t k = 0; k < c_cols; ++k)
            {
                double mm_e = 0;
                for(size_t m = 0; m < inner_loop; ++m)
                {
                    c_ptr[n * c_stride + k] += a_ptr[m * a_stride + n] * b_ptr[k * b_stride + m];
                }
                c_ptr[n * c_stride + k] = beta * c_ptr[n * c_stride + k] + alpha * mm_e;
            }
        }
#else
    auto c_out = [&](int i, int j, double x) {
        c_ptr[i * c_stride + j] = beta * c_ptr[i * c_stride + j] + alpha * x;
    };

    if(!(a_flags & RNN_MM_TRANSPOSE) && !(b_flags & RNN_MM_TRANSPOSE))
    {
        gemm(c_rows,
             c_cols,
             inner_loop,
             with_stride(a_ptr, a_stride),
             with_stride(b_ptr, b_stride),
             c_out);
    }
    else if((a_flags & RNN_MM_TRANSPOSE) && !(b_flags & RNN_MM_TRANSPOSE))
    {
        gemm(c_rows,
             c_cols,
             inner_loop,
             miopen::flip(with_stride(a_ptr, a_stride)),
             with_stride(b_ptr, b_stride),
             c_out);
    }
    else if(!(a_flags & RNN_MM_TRANSPOSE) && (b_flags & RNN_MM_TRANSPOSE))
    {
        gemm(c_rows,
             c_cols,
             inner_loop,
             with_stride(a_ptr, a_stride),
             miopen::flip(with_stride(b_ptr, b_stride)),
             c_out);
    }
    else
    {
        gemm(c_rows,
             c_cols,
             inner_loop,
             miopen::flip(with_stride(a_ptr, a_stride)),
             miopen::flip(with_stride(b_ptr, b_stride)),
             c_out);
#endif
    }
}

#endif
