#include "test.h"
#include "../aligned.h"
#include "../avx2_gemm.h"
#include "../avx512_gemm.h"
#include "../sse2_gemm.h"
#include "../ssse3_gemm.h"

#include <cstring>
#include <iostream>
#include <math.h>

namespace intgemm {
namespace {

void QuantizeRef(const float *input, int16_t *output, float quant_mult, std::size_t size) {
  for (std::size_t i = 0; i < size; ++i) {
    float value = roundf(input[i] * quant_mult);
    value = std::max(-32768.0f, value);
    value = std::min(32767.0f, value);
    output[i] = value;
  }
}

void QuantizeRef(const float *input, int8_t *output, float quant_mult, std::size_t size) {
  for (std::size_t i = 0; i < size; ++i) {
    float value = roundf(input[i] * quant_mult);
    value = std::max(-127.0f, value);
    value = std::min(127.0f, value);
    output[i] = value;
  }
}

template <class I> bool IsOff(float from, I ref, I test) {
  if (ref == test) return false;
  if (ref - test > 1 && test - ref > 1) return true;
  float off_test = fabs((float)test - from);
  float off_ref = fabs((float)ref - from);
  // Allow 0.5 to round either way.
  if (off_test > 0.49 && off_test < 0.51 && off_ref > 0.49 && off_ref < 0.51) return false;
  return true;
}

template <class Backend> bool Test(const float *input_unaligned, float quant_mult, std::size_t size) {
  typedef typename Backend::Integer Integer;
  bool success = true;
  AlignedVector<float> input(size);
  std::memcpy(input.begin(), input_unaligned, sizeof(float) * size);

  AlignedVector<Integer> ref(size);
  AlignedVector<Integer> test(size);
  QuantizeRef(input.begin(), ref.begin(), quant_mult, size);
  Backend::Quantize(input.begin(), test.begin(), quant_mult, size);
  for (std::size_t i = 0; i < size; ++i) {
    if (IsOff(input[i] * quant_mult, ref[i], test[i])) {
      UNSCOPED_INFO("Error at " << i << " from " << input[i] << '*' << quant_mult << '=' << (input[i]*quant_mult) << " ref = " << static_cast<int>(ref[i]) << " test = " << static_cast<int>(test[i]));
      success = false;
    }
  }
  return success;
}

template <class Backend> void TestMany(std::size_t grow) {
  float input[33] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32};
  float corners[33] = {-32769, -32768, -32767, -129, -128, -127, -1, 0, 1, 126, 127, 128, 129, 32766, 32768, 32769, -1.9, -1.5, -1.1, -1, -0.9, -0.5, -0.1, 0.0, 0.1, 0.5, 0.9, 1.0, 1.1, 1.5, 1.9, 16056.8, 2.5};
  for (std::size_t len = 0; len <= 33; len += grow) {
    CHECK(Test<Backend>(input, 1.0, len));
    CHECK(Test<Backend>(input, 32.0, len));
    CHECK(Test<Backend>(corners, 1.0, len));
    CHECK(Test<Backend>(corners, -1.0, len));
    CHECK(Test<Backend>(corners, -0.49, len));
  }
}

TEST_CASE ("Quantize SSE2", "[quantize]") {
  if (kCPU < CPUType::SSE2) return;
  TestMany<SSE2_16bit>(8);
}

TEST_CASE ("Quantize SSSE3", "[quantize]") {
  if (kCPU < CPUType::SSSE3) return;
  TestMany<SSSE3_8bit>(1);
}

TEST_CASE ("Quantize AVX2", "[quantize]") {
  if (kCPU < CPUType::AVX2) return;
  TestMany<AVX2_8bit>(1);
  TestMany<AVX2_16bit>(16);
}
#ifdef INTGEMM_COMPILER_SUPPORTS_AVX512BW
  TEST_CASE ("Quantize AVX512", "[quantize]") {
    if (kCPU < CPUType::AVX512BW) return;
    TestMany<AVX512_8bit>(1);
    TestMany<AVX512_16bit>(16);
  }
#endif

} // namespace
} // namespace intgemm
