#pragma once

#include "intgemm_config.h"
#include "interleave.h"
#include "intrinsics.h"
#include "vec_traits.h"
#include "callbacks.h"

namespace intgemm {

INTGEMM_SSE2 static inline float MaxFloat32(__m128 a) {
  // Fold to just using the first 64 bits.
  __m128 second_half = _mm_shuffle_ps(a, a, 3 * 4 + 2);
  a = _mm_max_ps(a, second_half);
  // Fold to just using the first 32 bits.
  second_half = _mm_shuffle_ps(a, a, 1);
  a = _mm_max_ps(a, second_half);
  // This casting compiles to nothing.
  return *reinterpret_cast<float*>(&a);
}

INTGEMM_SSE2 static inline dvector_t<CPUType::SSE2, int> PermuteSummer(__m128i pack0123, __m128i pack4567) {
  // No op for 128 bits: already reduced fully.
  return { pack0123, pack4567 };
}

INTGEMM_AVX2 static inline float MaxFloat32(__m256 a) {
  return MaxFloat32(max_ps(_mm256_castps256_ps128(a), _mm256_extractf128_ps(a, 1)));
}

INTGEMM_AVX2 static inline __m256i PermuteSummer(__m256i pack0123, __m256i pack4567) {
  // This instruction generates 1s 2s 3s 4s 5f 6f 7f 8f
  __m256i rev = _mm256_permute2f128_si256(pack0123, pack4567, 0x21);
  // This instruction generates 1f 2f 3f 4f 5s 6s 7s 8s
  __m256i blended = _mm256_blend_epi32(pack0123, pack4567, 0xf0);
  return _mm256_add_epi32(rev, blended);
}

#ifdef INTGEMM_COMPILER_SUPPORTS_AVX512BW
/* Only INTGEMM_AVX512F is necessary but due to GCC 5.4 bug we have to set INTGEMM_AVX512BW */
INTGEMM_AVX512BW static inline __m256i PermuteSummer(__m512i pack0123, __m512i pack4567) {
  // Form [0th 128-bit register of pack0123, 0st 128-bit register of pack4567, 2nd 128-bit register of pack0123, 2nd 128-bit register of pack4567]
  __m512i mix0 = _mm512_mask_permutex_epi64(pack0123, 0xcc, pack4567, (0 << 4) | (1 << 6));
  // Form [1st 128-bit register of pack0123, 1st 128-bit register of pack4567, 3rd 128-bit register of pack0123, 3rd 128-bit register of pack4567]
  __m512i mix1 = _mm512_mask_permutex_epi64(pack4567, 0x33, pack0123, 2 | (3 << 2));
  __m512i added = _mm512_add_epi32(mix0, mix1);
  // Now we have 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7.
  // Fold register over itself.
  return _mm256_add_epi32(_mm512_castsi512_si256(added), _mm512_extracti64x4_epi64(added, 1));
}

// Find the maximum float.
static inline INTGEMM_AVX512F float MaxFloat32(__m512 a) {
  // _mm512_extractf32x8_ps is AVX512DQ but we don't care about masking.
  // So cast to pd, do AVX512F _mm512_extractf64x4_pd, then cast to ps.
  __m256 upper = _mm256_castpd_ps(_mm512_extractf64x4_pd(_mm512_castps_pd(a), 1));
  return MaxFloat32(max_ps(_mm512_castps512_ps256(a), upper));
}

#endif

// Quantize function used for SSSE3 and AVX2.
// Separate function for thread to work around gcc 7 bug that doesn't imbue
// target attributes across #pragma omp parallel.
#define INTGEMM_QUANTIZE_THREAD(target, Register, name) \
target static void QuantizeThread(const float *input, int8_t *output, float quant_mult, std::size_t count) { \
  name::QuantizeTile8 q(quant_mult); \
  _Pragma("omp for") \
  for (std::size_t i = 0; i < count; i += sizeof(Register)) { \
    *reinterpret_cast<Register*>(output + i) = q.Consecutive(input + i); \
  } \
}

#define INTGEMM_QUANTIZE(target, Register, name) \
target static void Quantize(const float *const input, int8_t *const output, float quant_mult, Index size) { \
  assert(reinterpret_cast<uintptr_t>(input) % sizeof(Register) == 0); \
  assert(reinterpret_cast<uintptr_t>(output) % sizeof(Register) == 0); \
  const std::size_t kBatch = sizeof(Register); \
  const std::size_t fast_end = size & ~(kBatch - 1); \
  _Pragma("omp parallel") \
  { \
    QuantizeThread(input, output, quant_mult, fast_end); \
  } \
  std::size_t overhang = size & (kBatch - 1); \
  if (!overhang) return; \
  name::QuantizeTile8 q(quant_mult); \
  /* Each does size(Register) / 32 == kBatch / 4 floats at a time.
   * If we're allowed to read one of them, then we can read the whole register.  */ \
  const float *inputs[4]; \
  std::size_t i; \
  for (i = 0; i < (overhang + (kBatch / 4) - 1) / (kBatch / 4); ++i) { \
    inputs[i] = &input[fast_end + i * (kBatch / 4)]; \
  } \
  /* These will be clipped off. */ \
  for (; i < 4; ++i) { \
    inputs[i] = &input[fast_end]; \
  } \
  Register result = q.Tile(inputs[0], inputs[1], inputs[2], inputs[3]); \
  std::memcpy(output + (size & ~(kBatch - 1)), &result, overhang); \
}

/* Take 4 registers with 32-bit values to be horizontally added.  Reduce them
 * to one register with 32-bit values in the pattern 1 2 3 4 1 2 3 4, leaving
 * the final addition (which crosses 128-bit lanes) to the caller. 
 */
#define INTGEMM_PACK0123(target, Register) \
target inline Register Pack0123(Register sum0, Register sum1, Register sum2, Register sum3) { \
  Interleave32(sum0, sum1); \
  Register pack01 = add_epi32(sum0, sum1); \
  Interleave32(sum2, sum3); \
  Register pack23 = add_epi32(sum2, sum3); \
  Interleave64(pack01, pack23); \
  return add_epi32(pack01, pack23); \
} \

INTGEMM_PACK0123(INTGEMM_SSE2, __m128i)
INTGEMM_PACK0123(INTGEMM_AVX2, __m256i)
#ifdef INTGEMM_COMPILER_SUPPORTS_AVX512BW
/* Only INTGEMM_AVX512F is necessary but due to GCC 5.4 bug we have to set INTGEMM_AVX512BW */
INTGEMM_PACK0123(INTGEMM_AVX512BW, __m512i)
#endif

template <typename Callback>
INTGEMM_SSE2 static inline void RunCallback(Callback& callback_impl, dvector_t<CPUType::SSE2, int> total, Index row_idx, Index col_idx, Index rows, Index cols) {
  callback_impl(total.first, callbacks::OutputBufferInfo(row_idx, col_idx, rows, cols));
  callback_impl(total.second, callbacks::OutputBufferInfo(row_idx, col_idx + 4, rows, cols));
}

template <typename Callback>
INTGEMM_AVX2 static inline void RunCallback(Callback& callback_impl, vector_t<CPUType::AVX2, int> total, Index row_idx, Index col_idx, Index rows, Index cols) {
  callback_impl(total, callbacks::OutputBufferInfo(row_idx, col_idx, rows, cols));
}

// 16-bit multiplier for INTGEMM_SSE2, INTGEMM_AVX2, and AVX512.
// C = A * B * unquant_mult
//
// This has been substantially revised from Jacob Devlin's SSE code which is:
// Copyright (c) 2017 Microsoft Corporation

// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:

// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

// A is a row-major quantized matrix (from PrepareA)
// B is a rearranged quantized matrix (from PrepareB)
// C is output in row-major form.
//
// All of A, B, and C must be in aligned to a multiple of the register size:
// INTGEMM_SSE2: 16 bytes
// INTGEMM_AVX2: 32 bytes
// AVX512: 64 bytes.
//
// A_rows can be anything non-negative.
// width must be a multiple of the register size.
// B_cols must be a multiple of 8.
// Multiply16
#define INTGEMM_MULTIPLY16(Register, target, cpu_type) \
template <typename Callback> target static void Multiply(const int16_t *A, const int16_t *B, Index A_rows, Index width, Index B_cols, Callback callback) { \
  assert(width % (sizeof(Register) / sizeof(int16_t)) == 0); \
  assert(B_cols % 8 == 0); \
  assert(reinterpret_cast<uintptr_t>(A) % sizeof(Register) == 0); \
  assert(reinterpret_cast<uintptr_t>(B) % sizeof(Register) == 0); \
  const int simd_width = width / (sizeof(Register) / sizeof(int16_t)); \
  auto callback_impl = callbacks::CallbackImpl<cpu_type, Callback>(callback); \
  const Register *B0_col = reinterpret_cast<const Register *>(B); \
  for (Index B0_colidx = 0; B0_colidx < B_cols; B0_col += 8 * simd_width, B0_colidx += 8) { \
    /* Process one row of A at a time.  Doesn't seem to be faster to do multiple rows of A at once.*/ \
    for (Index A_rowidx = 0; A_rowidx < A_rows; ++A_rowidx) { \
      const Register *A_row = reinterpret_cast<const Register*>(A + A_rowidx * width); \
      /* These will be packed 32-bit integers containing sums for each row of B multiplied by the row of A. \
         Iterate over shared (inner) dimension.*/ \
      int k = 0; \
      Register a = *(A_row + k); \
      Register sum0 = madd_epi16(a, *(B0_col + k * 8)); \
      Register sum1 = madd_epi16(a, *(B0_col + k * 8 + 1)); \
      Register sum2 = madd_epi16(a, *(B0_col + k * 8 + 2)); \
      Register sum3 = madd_epi16(a, *(B0_col + k * 8 + 3)); \
      Register sum4 = madd_epi16(a, *(B0_col + k * 8 + 4)); \
      Register sum5 = madd_epi16(a, *(B0_col + k * 8 + 5)); \
      Register sum6 = madd_epi16(a, *(B0_col + k * 8 + 6)); \
      Register sum7 = madd_epi16(a, *(B0_col + k * 8 + 7)); \
      for (int k = 1; k < simd_width; ++k) { \
        Register a = *(A_row + k); \
        /* Multiply 16-bit, horizontally add to packed 32-bit integers.*/ \
        Register mult0 = madd_epi16(a, *(B0_col + k * 8)); \
        Register mult1 = madd_epi16(a, *(B0_col + k * 8 + 1)); \
        Register mult2 = madd_epi16(a, *(B0_col + k * 8 + 2)); \
        Register mult3 = madd_epi16(a, *(B0_col + k * 8 + 3)); \
        Register mult4 = madd_epi16(a, *(B0_col + k * 8 + 4)); \
        Register mult5 = madd_epi16(a, *(B0_col + k * 8 + 5)); \
        Register mult6 = madd_epi16(a, *(B0_col + k * 8 + 6)); \
        Register mult7 = madd_epi16(a, *(B0_col + k * 8 + 7)); \
        /* Sum packed 32-bit integers with danger of overflow.  TODO: accumulate in 64-bit every so often.*/ \
        sum0 = add_epi32(sum0, mult0); \
        sum1 = add_epi32(sum1, mult1); \
        sum2 = add_epi32(sum2, mult2); \
        sum3 = add_epi32(sum3, mult3); \
        sum4 = add_epi32(sum4, mult4); \
        sum5 = add_epi32(sum5, mult5); \
        sum6 = add_epi32(sum6, mult6); \
        sum7 = add_epi32(sum7, mult7); \
      } \
      /* Reduce sums within 128-bit lanes.*/ \
      Register pack0123 = Pack0123(sum0, sum1, sum2, sum3); \
      Register pack4567 = Pack0123(sum4, sum5, sum6, sum7); \
      /*The specific implementation may need to reduce further.*/ \
      auto total = PermuteSummer(pack0123, pack4567); \
      RunCallback(callback_impl, total, A_rowidx, B0_colidx, A_rows, B_cols); \
    } \
  } \
} \

//An int8_prepbias version of the above code, using the add 127 technique
#define INTGEMM_PREPAREBIASFOR8(Register, target, cpu_type) \
  template <class Callback> target static void PrepareBias(const int8_t *B, Index width, Index B_cols, Callback callback) { \
  assert(width % (sizeof(Register) / sizeof(int8_t)) == 0); \
  assert(B_cols % 8 == 0); \
  assert(reinterpret_cast<uintptr_t>(B) % sizeof(Register) == 0); \
  const int simd_width = width / (sizeof(Register) / sizeof(int8_t)); \
  auto callback_impl = callbacks::CallbackImpl<cpu_type, Callback>(callback); \
  const Register *B0_col = reinterpret_cast<const Register *>(B); \
  const Register a = set1_epi8<Register>(1); \
  for (Index B0_colidx = 0; B0_colidx < B_cols; B0_col += 8 * simd_width, B0_colidx += 8) { \
    /*const Register *A_row = reinterpret_cast<const Register*>(A + A_rowidx * width);*/ \
    /* These will be packed 16-bit integers containing sums for each row of B multiplied by the row of A. \
       Iterate over shared (inner) dimension.*/ \
    int k = 0; \
    Register sum0 = maddubs_epi16(a, *(B0_col + k * 8)); \
    Register sum1 = maddubs_epi16(a, *(B0_col + k * 8 + 1)); \
    Register sum2 = maddubs_epi16(a, *(B0_col + k * 8 + 2)); \
    Register sum3 = maddubs_epi16(a, *(B0_col + k * 8 + 3)); \
    Register sum4 = maddubs_epi16(a, *(B0_col + k * 8 + 4)); \
    Register sum5 = maddubs_epi16(a, *(B0_col + k * 8 + 5)); \
    Register sum6 = maddubs_epi16(a, *(B0_col + k * 8 + 6)); \
    Register sum7 = maddubs_epi16(a, *(B0_col + k * 8 + 7)); \
    /* Upcast to 32-bit and horizontally add. Seems a bit faster if this is declared here.*/ \
    Register ones = set1_epi16<Register>(1); \
    sum0 = madd_epi16(sum0, ones); \
    sum1 = madd_epi16(sum1, ones); \
    sum2 = madd_epi16(sum2, ones); \
    sum3 = madd_epi16(sum3, ones); \
    sum4 = madd_epi16(sum4, ones); \
    sum5 = madd_epi16(sum5, ones); \
    sum6 = madd_epi16(sum6, ones); \
    sum7 = madd_epi16(sum7, ones); \
    for (int k = 1; k < simd_width; ++k) { \
      /*Register a = *(A_row + k);*/ \
      /* Multiply 8-bit, horizontally add to packed 16-bit integers.*/ \
      Register mult0 = maddubs_epi16(a, *(B0_col + k * 8)); \
      Register mult1 = maddubs_epi16(a, *(B0_col + k * 8 + 1)); \
      Register mult2 = maddubs_epi16(a, *(B0_col + k * 8 + 2)); \
      Register mult3 = maddubs_epi16(a, *(B0_col + k * 8 + 3)); \
      Register mult4 = maddubs_epi16(a, *(B0_col + k * 8 + 4)); \
      Register mult5 = maddubs_epi16(a, *(B0_col + k * 8 + 5)); \
      Register mult6 = maddubs_epi16(a, *(B0_col + k * 8 + 6)); \
      Register mult7 = maddubs_epi16(a, *(B0_col + k * 8 + 7)); \
      /* Upcast to 32-bit and horizontally add.*/ \
      mult0 = madd_epi16(mult0, ones); \
      mult1 = madd_epi16(mult1, ones); \
      mult2 = madd_epi16(mult2, ones); \
      mult3 = madd_epi16(mult3, ones); \
      mult4 = madd_epi16(mult4, ones); \
      mult5 = madd_epi16(mult5, ones); \
      mult6 = madd_epi16(mult6, ones); \
      mult7 = madd_epi16(mult7, ones); \
      /*Add in 32bit*/ \
      sum0 = add_epi32(sum0, mult0); \
      sum1 = add_epi32(sum1, mult1); \
      sum2 = add_epi32(sum2, mult2); \
      sum3 = add_epi32(sum3, mult3); \
      sum4 = add_epi32(sum4, mult4); \
      sum5 = add_epi32(sum5, mult5); \
      sum6 = add_epi32(sum6, mult6); \
      sum7 = add_epi32(sum7, mult7); \
      \
    } \
    /* Reduce sums within 128-bit lanes.*/ \
    Register pack0123 = Pack0123(sum0, sum1, sum2, sum3); \
    Register pack4567 = Pack0123(sum4, sum5, sum6, sum7); \
    /*The specific implementation may need to reduce further.*/ \
    auto total = PermuteSummer(pack0123, pack4567); \
    RunCallback(callback_impl, total, 0, B0_colidx, 1, B_cols); \
  } \
} \

//An int8 version of the above code, using the add 127 technique
#define INTGEMM_MULTIPLY8SHIFT(Register, target, cpu_type) \
  template <class Callback> target static void Multiply8Shift(const uint8_t *A, const int8_t *B, Index A_rows, Index width, Index B_cols, Callback callback) { \
  assert(width % (sizeof(Register) / sizeof(int8_t)) == 0); \
  assert(B_cols % 8 == 0); \
  assert(reinterpret_cast<uintptr_t>(A) % sizeof(Register) == 0); \
  assert(reinterpret_cast<uintptr_t>(B) % sizeof(Register) == 0); \
  const int simd_width = width / (sizeof(Register) / sizeof(int8_t)); \
  auto callback_impl = callbacks::CallbackImpl<cpu_type, Callback>(callback); \
  const Register *B0_col = reinterpret_cast<const Register *>(B); \
  for (Index B0_colidx = 0; B0_colidx < B_cols; B0_col += 8 * simd_width, B0_colidx += 8) { \
    /* Process one row of A at a time.  Doesn't seem to be faster to do multiple rows of A at once.*/ \
    for (Index A_rowidx = 0; A_rowidx < A_rows; ++A_rowidx) { \
      const Register *A_row = reinterpret_cast<const Register*>(A + A_rowidx * width); \
      /* These will be packed 16-bit integers containing sums for each row of B multiplied by the row of A. \
         Iterate over shared (inner) dimension.*/ \
      int k = 0; \
      Register a = *(A_row + k); \
      Register sum0 = maddubs_epi16(a, *(B0_col + k * 8)); \
      Register sum1 = maddubs_epi16(a, *(B0_col + k * 8 + 1)); \
      Register sum2 = maddubs_epi16(a, *(B0_col + k * 8 + 2)); \
      Register sum3 = maddubs_epi16(a, *(B0_col + k * 8 + 3)); \
      Register sum4 = maddubs_epi16(a, *(B0_col + k * 8 + 4)); \
      Register sum5 = maddubs_epi16(a, *(B0_col + k * 8 + 5)); \
      Register sum6 = maddubs_epi16(a, *(B0_col + k * 8 + 6)); \
      Register sum7 = maddubs_epi16(a, *(B0_col + k * 8 + 7)); \
      /* Upcast to 32-bit and horizontally add. Seems a bit faster if this is declared here.*/ \
      Register ones = set1_epi16<Register>(1); \
      sum0 = madd_epi16(sum0, ones); \
      sum1 = madd_epi16(sum1, ones); \
      sum2 = madd_epi16(sum2, ones); \
      sum3 = madd_epi16(sum3, ones); \
      sum4 = madd_epi16(sum4, ones); \
      sum5 = madd_epi16(sum5, ones); \
      sum6 = madd_epi16(sum6, ones); \
      sum7 = madd_epi16(sum7, ones); \
      for (int k = 1; k < simd_width; ++k) { \
        Register a = *(A_row + k); \
        /* Multiply 8-bit, horizontally add to packed 16-bit integers.*/ \
        Register mult0 = maddubs_epi16(a, *(B0_col + k * 8)); \
        Register mult1 = maddubs_epi16(a, *(B0_col + k * 8 + 1)); \
        Register mult2 = maddubs_epi16(a, *(B0_col + k * 8 + 2)); \
        Register mult3 = maddubs_epi16(a, *(B0_col + k * 8 + 3)); \
        Register mult4 = maddubs_epi16(a, *(B0_col + k * 8 + 4)); \
        Register mult5 = maddubs_epi16(a, *(B0_col + k * 8 + 5)); \
        Register mult6 = maddubs_epi16(a, *(B0_col + k * 8 + 6)); \
        Register mult7 = maddubs_epi16(a, *(B0_col + k * 8 + 7)); \
        /* Upcast to 32-bit and horizontally add.*/ \
        mult0 = madd_epi16(mult0, ones); \
        mult1 = madd_epi16(mult1, ones); \
        mult2 = madd_epi16(mult2, ones); \
        mult3 = madd_epi16(mult3, ones); \
        mult4 = madd_epi16(mult4, ones); \
        mult5 = madd_epi16(mult5, ones); \
        mult6 = madd_epi16(mult6, ones); \
        mult7 = madd_epi16(mult7, ones); \
        /*Add in 32bit*/ \
        sum0 = add_epi32(sum0, mult0); \
        sum1 = add_epi32(sum1, mult1); \
        sum2 = add_epi32(sum2, mult2); \
        sum3 = add_epi32(sum3, mult3); \
        sum4 = add_epi32(sum4, mult4); \
        sum5 = add_epi32(sum5, mult5); \
        sum6 = add_epi32(sum6, mult6); \
        sum7 = add_epi32(sum7, mult7); \
         \
      } \
      /* Reduce sums within 128-bit lanes.*/ \
      Register pack0123 = Pack0123(sum0, sum1, sum2, sum3); \
      Register pack4567 = Pack0123(sum4, sum5, sum6, sum7); \
      /*The specific implementation may need to reduce further.*/ \
      auto total = PermuteSummer(pack0123, pack4567); \
      RunCallback(callback_impl, total, A_rowidx, B0_colidx, A_rows, B_cols); \
    } \
  } \
} \

/* 8-bit matrix multiply used by AVX and AVX2.
 * These have two peculiar properties:
 * 1. The sign instructions don't exist in AVX512.
 * 2. 16 registers means gcc's register allocation failed so I wrote it in my
 *    own asm.
 * 3. They support 3-argument vpsignb and vpmaddubsw.
 *
 * Fun fact: AVX introduced the three-argument vpsignb and vpmaddubsw but only
 * for 128-bit, despite the primary change in AVX being the addition of
 * 256-bit.  We had to wait for INTGEMM_AVX2 to get 256-bit versions of vpsignb and
 * vpmaddubsw.  That's why this code is generic over 128-bit or 256-bit.
 */

INTGEMM_AVX2 inline static void InnerINTGEMM_AVX2(
    __m256i a, const __m256i *b,
    __m256i &sum0, __m256i &sum1, __m256i &sum2, __m256i &sum3,
    __m256i &sum4, __m256i &sum5, __m256i &sum6, __m256i &sum7) {
  // Annoyingly the only 8-bit multiply is signed * unsigned (maddubs).
  // So we take the sign bits off of a and apply them each b in a * b.
  //
  // We have only 16 YMM registers but we want to store:
  // 1 for a (or |a|)
  // 8 temporaries for applying sign to each column of B.
  // 8 sums.
  //
  // gcc's register allocator does:
  // 1 for a, do all the sign application, then overwrite with |a|
  // 8 temporaries
  // 7 sums in registers + 1 on the stack
  //
  // But it's possible to complete an operation early, freeing up its
  // temporary register for reuse.  But completing an operation early
  // requires us to have |a| for vpmaddubsw while completing the later
  // operation needs a again to apply sign.
  //
  // So we do two columns, 0 and 1, early.  This allows b0_b6 and b1_b7
  // to be reused by columns 6 and 7, respectively.  And there's enough
  // registers to store both a and |a|.
  //
  // These are the temporary variables used to process each column of b.
  // We let the compiler choose which register number is which, but force
  // it to allocate all registers.
  __m256i absa;
  __m256i b0_b6, b1_b7, b2, b3, b4, b5;
  // Maybe this will tell gcc that we're accessing 8 registers starting
  // at B_live.  Though I doubt it because we're passing the address as a
  // register.
  typedef struct { __m256i x[8]; } B_range;
  asm(
      // Copy the first 6 columns of b to registers.  We assume B has
      // been rearranged so that these 8 columns are consecutive.
      // vpsignb does not take a memory address as its second argument,
      // so this can't be inlined into vsignb.
      "vmovdqa          (%[B]), %[b0_b6]\n"
      "vmovdqa   %c[size](%[B]), %[b1_b7]\n"
      // These multiplies are executed by the assembler, not by the CPU
      // at run time.
      // I would have liked to just initialize b2 etc above but that
      // would make it an input argument "+x" instead of "=&x".  And +x
      // counts as two operands for purposes of gcc's annoying 30-operand
      // limit.
      "vmovdqa 2*%c[size](%[B]), %[b2]\n"
      "vmovdqa 3*%c[size](%[B]), %[b3]\n"
      "vmovdqa 4*%c[size](%[B]), %[b4]\n"
      "vmovdqa 5*%c[size](%[B]), %[b5]\n"
      // Store the absolute value of a in absa.
      "vpabsb  %[a], %[absa]\n"
      // If a byte of a is negative, negate the corresponding byte in
      // b0_b6 etc.
      "vpsignb %[a], %[b0_b6], %[b0_b6]\n"
      "vpsignb %[a], %[b1_b7], %[b1_b7]\n"
      // Multiply signed * unsigned then horizontally add to form packed
      // 16-bit integers:
      // b0[0] * |a|[0] + b0[1] * |a|[1], b0[2] * |a|[2] + b0[3] * |a|[3], ...
      "vpmaddubsw %[b0_b6], %[absa], %[b0_b6]\n"
      "vpmaddubsw %[b1_b7], %[absa], %[b1_b7]\n"
      // vpmaddubsw has latency 5 so work on some other sign bits while
      // we're at it.
      "vpsignb %[a], %[b2], %[b2]\n"
      "vpsignb %[a], %[b3], %[b3]\n"
      "vpsignb %[a], %[b4], %[b4]\n"
      "vpsignb %[a], %[b5], %[b5]\n"
      // Perform a 16-bit add with saturation to accumlate sums.
      "vpaddsw %[b0_b6], %[sum0], %[sum0]\n"
      // Now we can reuse b0_b6 for b6
      "vmovdqa 6*%c[size](%[B]), %[b0_b6]\n"
      "vpaddsw %[b1_b7], %[sum1], %[sum1]\n"
      // Now we can reuse b1_b7 for b7
      "vmovdqa 7*%c[size](%[B]), %[b1_b7]\n"
      // More crunching while the load happens.
      "vpmaddubsw %[b2], %[absa], %[b2]\n"
      "vpmaddubsw %[b3], %[absa], %[b3]\n"
      "vpmaddubsw %[b4], %[absa], %[b4]\n"
      "vpsignb %[a], %[b0_b6], %[b0_b6]\n"
      "vpsignb %[a], %[b1_b7], %[b1_b7]\n"
      "vpmaddubsw %[b5], %[absa], %[b5]\n"
      "vpmaddubsw %[b0_b6], %[absa], %[b0_b6]\n"
      "vpmaddubsw %[b1_b7], %[absa], %[b1_b7]\n"
      "vpaddsw %[b2], %[sum2], %[sum2]\n"
      "vpaddsw %[b3], %[sum3], %[sum3]\n"
      "vpaddsw %[b4], %[sum4], %[sum4]\n"
      "vpaddsw %[b5], %[sum5], %[sum5]\n"
      "vpaddsw %[b0_b6], %[sum6], %[sum6]\n"
      "vpaddsw %[b1_b7], %[sum7], %[sum7]\n"
      : [sum0] "+x" (sum0),
        [sum1] "+x" (sum1),
        [sum2] "+x" (sum2),
        [sum3] "+x" (sum3),
        [sum4] "+x" (sum4),
        [sum5] "+x" (sum5),
        [sum6] "+x" (sum6),
        [sum7] "+x" (sum7),
        [b0_b6] "=&x" (b0_b6),
        [b1_b7] "=&x" (b1_b7),
        [b2] "=&x" (b2),
        [b3] "=&x" (b3),
        [b4] "=&x" (b4),
        [b5] "=&x" (b5),
        [absa] "=&x" (absa)
      : 
        // I would like to use m here but that non-deterministically
        // chooses %(eax) or -256$(eax) and there's no way to add to that
        // memory address:
        // https://gcc.gnu.org/ml/gcc-help/2011-04/msg00518.html
        //
        [B] "r" (reinterpret_cast<const B_range*>(b)),
        [a] "x" (a),
        [size] "i" (sizeof(__m256i))
    );
}


// For INTGEMM_SSSE3 without AVX
INTGEMM_SSSE3 inline static void InnerINTGEMM_SSSE3(
    __m128i a, const __m128i *b,
    __m128i &sum0, __m128i &sum1, __m128i &sum2, __m128i &sum3,
    __m128i &sum4, __m128i &sum5, __m128i &sum6, __m128i &sum7) {
  __m128i a_positive = abs_epi8(a);
  sum0 = adds_epi16(sum0, maddubs_epi16(a_positive, sign_epi8(b[0], a)));
  sum1 = adds_epi16(sum1, maddubs_epi16(a_positive, sign_epi8(b[1], a)));
  sum2 = adds_epi16(sum2, maddubs_epi16(a_positive, sign_epi8(b[2], a)));
  sum3 = adds_epi16(sum3, maddubs_epi16(a_positive, sign_epi8(b[3], a)));
  sum4 = adds_epi16(sum4, maddubs_epi16(a_positive, sign_epi8(b[4], a)));
  sum5 = adds_epi16(sum5, maddubs_epi16(a_positive, sign_epi8(b[5], a)));
  sum6 = adds_epi16(sum6, maddubs_epi16(a_positive, sign_epi8(b[6], a)));
  sum7 = adds_epi16(sum7, maddubs_epi16(a_positive, sign_epi8(b[7], a)));
}
//INTGEMM_AVX2 or INTGEMM_SSSE3 multiply
#define INTGEMM_MULTIPLY8(Register, target, cpu_type) \
  template <typename Callback> target static void Multiply(const int8_t *A, const int8_t *B, Index A_rows, Index width, Index B_cols, Callback callback) { \
  assert(width % sizeof(Register) == 0); \
  assert(B_cols % 8 == 0); \
  assert(reinterpret_cast<uintptr_t>(A) % sizeof(Register) == 0); \
  assert(reinterpret_cast<uintptr_t>(B) % sizeof(Register) == 0); \
  const int simd_width = width / sizeof(Register); \
  auto callback_impl = callbacks::CallbackImpl<cpu_type, Callback>(callback); \
  const Register *B0_col = reinterpret_cast<const Register*>(B); \
  /*Go over 8 columns of B at a time.*/ \
  for (Index B0_colidx = 0; B0_colidx != B_cols; B0_col += 8 * simd_width, B0_colidx += 8) { \
    /*Process one row of A at a time.  Doesn't seem to be faster to do multiple rows of A at once.*/ \
    for (Index A_rowidx = 0; A_rowidx < A_rows; ++A_rowidx) { \
      /*Iterate over shared (inner) dimension.*/ \
      const Register *A_live = reinterpret_cast<const Register *>(A + A_rowidx * width); \
      const Register *A_end = A_live + simd_width; \
      const Register *B_live = B0_col; \
      /* Rather than initializing as zeros and adding, just initialize the first.*/ \
      Register a = *(A_live++); \
      Register a_positive = abs_epi8(a); \
      /* These will be packed 16-bit integers containing sums for each column of B multiplied by the row of A.*/ \
      Register sum0 = maddubs_epi16(a_positive, sign_epi8(B_live[0], a)); \
      Register sum1 = maddubs_epi16(a_positive, sign_epi8(B_live[1], a)); \
      Register sum2 = maddubs_epi16(a_positive, sign_epi8(B_live[2], a)); \
      Register sum3 = maddubs_epi16(a_positive, sign_epi8(B_live[3], a)); \
      Register sum4 = maddubs_epi16(a_positive, sign_epi8(B_live[4], a)); \
      Register sum5 = maddubs_epi16(a_positive, sign_epi8(B_live[5], a)); \
      Register sum6 = maddubs_epi16(a_positive, sign_epi8(B_live[6], a)); \
      Register sum7 = maddubs_epi16(a_positive, sign_epi8(B_live[7], a)); \
      B_live += 8; \
      /* Use A as the loop variable so the add can be done where gcc likes it for branch prediction.*/ \
      for (; A_live != A_end; ++A_live, B_live += 8) { \
        Inner##target(*A_live, B_live, sum0, sum1, sum2, sum3, sum4, sum5, sum6, sum7); \
      } \
      /* Convert 16-bit to 32-bit and add, not caring what parts are added.
       * Implementations:
       * 1. https://github.com/tesseract-ocr/tesseract/blob/master/src/arch/intsimdmatrixavx2.cpp#L67 under Apache license:
       *   This does a multiply by 1 and horizontal add:
       *    _mm512_madd_epi16(sum, _mm512_set1_epi16(1))
       *   Current fastest.
       *
       * 2. Signed extension and fold halves:
       *    sum = _mm512_add_epi32(
       *      _mm512_cvtepi16_epi32(_mm512_castsi512_si256(sum)),
       *      _mm512_cvtepi16_epi32(_mm512_extracti64x4_epi64(sum, 1)));
       *
       * 3. Sign extend by abuse of bitshift, then add.
       * sum = _mm512_add_epi32(
       *      _mm512_srai_epi32(_mm512_slli_epi32(sum, 16), 16),
       *      _mm512_srai_epi32(sum, 16));
       */ \
      Register ones = set1_epi16<Register>(1); \
      sum0 = madd_epi16(sum0, ones); \
      sum1 = madd_epi16(sum1, ones); \
      sum2 = madd_epi16(sum2, ones); \
      sum3 = madd_epi16(sum3, ones); \
      sum4 = madd_epi16(sum4, ones); \
      sum5 = madd_epi16(sum5, ones); \
      sum6 = madd_epi16(sum6, ones); \
      sum7 = madd_epi16(sum7, ones); \
      Register pack0123 = Pack0123(sum0, sum1, sum2, sum3); \
      Register pack4567 = Pack0123(sum4, sum5, sum6, sum7); \
      auto total = PermuteSummer(pack0123, pack4567); \
      RunCallback(callback_impl, total, A_rowidx, B0_colidx, A_rows, B_cols); \
    } \
  } \
} \

#define INTGEMM_MAXABSOLUTE(Register, target) \
target static inline float MaxAbsolute(const float *begin_float, const float *end_float) { \
  assert(end_float > begin_float); \
  assert(reinterpret_cast<uintptr_t>(begin_float) % sizeof(Register) == 0); \
  const Register *begin = reinterpret_cast<const Register*>(begin_float); \
  const float *end_reg = end_float - (reinterpret_cast<uintptr_t>(end_float) % sizeof(Register)) / sizeof(float); \
  const Register *end = reinterpret_cast<const Register*>(end_reg); \
  union {float f; int32_t i;} and_convert, float_convert; \
  and_convert.i = 0x7fffffff; \
  Register and_me = set1_ps<Register>(and_convert.f); \
  Register highest = setzero_ps<Register>(); \
  for (; begin < end; ++begin) { \
    Register reg = and_ps(and_me, *begin); \
    highest = max_ps(highest, reg); \
  } \
  float ret = MaxFloat32(highest); \
  /* Overhang: this would be more efficient if done in a single SIMD operation with some zeroing */ \
  for (const float *i = end_reg; i < end_float; ++i) { \
    float_convert.f = *i; \
    float_convert.i &= and_convert.i; \
    ret = std::max(ret, float_convert.f); \
  } \
  return ret; \
} \

} // namespace intgemm
