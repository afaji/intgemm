#include "../aligned.h"
#include "intgemm_config.h"
#include "../avx512_gemm.h"
#include "../sse2_gemm.h"
#include "../avx2_gemm.h"
#include "../ssse3_gemm.h"
#include "../intgemm.h"
#include "../stop_watch.h"
#include "../callbacks.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <random>

namespace intgemm {
namespace {

float MaxAbsoluteBaseline(const float *begin, const float *end) {
  auto res = std::minmax_element(begin, end);
  return std::max(fabsf(*res.first), fabsf(*res.second));
}

void BenchmarkMaxAbsolute() {
  std::mt19937 gen;
  std::uniform_real_distribution<float> dist(0.f, 1.f);
  gen.seed(45678);

  AlignedVector<float> v(4096 * 4096);
  for (auto& it : v) {
    it = dist(gen);
  }

  std::vector<uint64_t> stats;
  // Hopefully these don't get optimized out...
  MaxAbsoluteBaseline(v.begin(), v.end());
  {
    StopWatch w(stats);
    MaxAbsoluteBaseline(v.begin(), v.end());
  }
  {
    StopWatch w(stats);
    avx2::MaxAbsolute(v.begin(), v.end());
  }
  std::cout << "MaxAbsolute baseline = " << stats[0] << " optimized = " << stats[1] << " speedup = " << ((float)stats[0] / (float)stats[1])<< '\n';
}

struct RandomMatrices {
  RandomMatrices(Index A_rows_in, Index width_in, Index B_cols_in) :
    A_rows(A_rows_in), width(width_in), B_cols(B_cols_in),
    A(A_rows * width), B(width * B_cols) {
    std::mt19937 gen;
    std::uniform_real_distribution<float> dist(-1.f, 1.f);
    gen.seed(45678);

    for (auto& it : A) {
      it = dist(gen);
    }
    for (auto& it : B) {
      it = dist(gen);
    }
  }

  const Index A_rows, width, B_cols;
  AlignedVector<float> A, B;
};

template <class Backend> void Run(const RandomMatrices &m, std::vector<uint64_t> &stats) {
  typedef typename Backend::Integer Integer;
  float quant_mult = 127.0 / 2;
  float unquant_mult = 1.0 / (quant_mult * quant_mult);
  AlignedVector<Integer> A_prepared(m.A_rows * m.width);
  Backend::PrepareA(m.A.begin(), A_prepared.begin(), quant_mult, m.A_rows, m.width);
  AlignedVector<Integer> B_prepared(m.width * m.B_cols);
  Backend::PrepareB(m.B.begin(), B_prepared.begin(), quant_mult, m.width, m.B_cols);
  AlignedVector<float> output(m.A_rows * m.B_cols);
  // Burn in
  Backend::Multiply(A_prepared.begin(), B_prepared.begin(), m.A_rows, m.width, m.B_cols, callbacks::UnquantizeAndWrite(unquant_mult, output.begin()));
  {
    StopWatch w(stats);
    Backend::Multiply(A_prepared.begin(), B_prepared.begin(), m.A_rows, m.width, m.B_cols, callbacks::UnquantizeAndWrite(unquant_mult, output.begin()));
  }
}

template <class Backend> void RunAll(RandomMatrices *matrices, RandomMatrices *matrices_end, std::vector<std::vector<uint64_t>> &stats) {
  if (Backend::kUses > kCPU) return;
  std::size_t size = matrices_end - matrices;
  if (stats.size() < size)
    stats.resize(size);
  for (std::size_t i = 0; i < size; ++i) {
    Run<Backend>(matrices[i], stats[i]);
  }
}

struct BackendStats {
  std::vector<std::vector<uint64_t>> ssse3_8bit;
  std::vector<std::vector<uint64_t>> avx2_8bit;
  std::vector<std::vector<uint64_t>> avx512_8bit;
  std::vector<std::vector<uint64_t>> avx512vnni_8bit;
  std::vector<std::vector<uint64_t>> sse2_16bit;
  std::vector<std::vector<uint64_t>> avx2_16bit;
  std::vector<std::vector<uint64_t>> avx512_16bit;
};

const float kOutlierThreshold = 0.75;
void Summarize(std::vector<uint64_t> &stats) {
  // Throw out outliers.
  std::vector<uint64_t>::iterator keep = stats.begin() + stats.size() * kOutlierThreshold;
  std::nth_element(stats.begin(), keep, stats.end());
  double avg = 0.0;
  for (std::vector<uint64_t>::const_iterator i = stats.begin(); i != keep; ++i) {
    avg += *i;
  }
  avg /= (keep - stats.begin());
  double stddev = 0.0;
  for (std::vector<uint64_t>::const_iterator i = stats.begin(); i != keep; ++i) {
    double off = (double)*i - avg;
    stddev += off * off;
  }
  stddev = sqrt(stddev / (keep - stats.begin() - 1));
  std::cout << std::setw(10) << *std::min_element(stats.begin(), stats.end()) << '\t' << std::setw(8) << avg << '\t' << std::setw(8) << stddev;
}

template <class Backend> void Print(std::vector<std::vector<uint64_t>> &stats, int index) {
  if (stats.empty()) return;
  std::cout << std::setw(16) << Backend::kName << '\t';
  Summarize(stats[index]);
  std::cout << '\n';
}

} // namespace intgemm
} // namespace

// Program takes no input
int main(int, char ** argv) {
  std::cerr << "Remember to run this on a specific core:\ntaskset --cpu-list 0 " << argv[0] << std::endl;

  using namespace intgemm;
  BenchmarkMaxAbsolute();
  RandomMatrices matrices[] = {
    {1, 64, 8},
    {8, 256, 256},
    {8, 2048, 256},
    {8, 256, 2048},
    {320, 256, 256},
    {472, 256, 256},
    {248, 256, 256},
    {200, 256, 256},
    // Additional stuff
    {256, 256, 256},
    {512, 512, 512},
    {1024, 1024, 1024},
/*    {4096, 4096, 4096},
    {4096, 4096, 2048},
    {4096, 4096, 1024},
    {4096, 4096, 512},
    {4096, 4096, 256},*/
    {4096, 4096, 128}
  };
  RandomMatrices *matrices_end = (RandomMatrices*)matrices + sizeof(matrices) / sizeof(RandomMatrices);
  // Only do full sampling for <1024 rows.
  RandomMatrices *full_sample;
  for (full_sample = matrices_end - 1; full_sample >= matrices && full_sample->A_rows >= 1024; --full_sample) {}
  ++full_sample;

  BackendStats stats;
  const int kSamples = 100;
  // Realistically, we don't expect different architectures or different precisions to run in the
  // same run of an application. Benchmark per architecture and per precision level.
  std::cerr << "SSSE3 8bit, 100 samples..." << std::endl;
  for (int samples = 0; samples < kSamples; ++samples) {
    RandomMatrices *end = (samples < 4) ? matrices_end : full_sample;
    RunAll<SSSE3_8bit>(matrices, end, stats.ssse3_8bit);
  }

  std::cerr << "SSE2 16bit, 100 samples..." << std::endl;
  for (int samples = 0; samples < kSamples; ++samples) {
    RandomMatrices *end = (samples < 4) ? matrices_end : full_sample;
    RunAll<SSE2_16bit>(matrices, end, stats.sse2_16bit);
  }

  std::cerr << "AVX2 8bit, 100 samples..." << std::endl;
  for (int samples = 0; samples < kSamples; ++samples) {
    RandomMatrices *end = (samples < 4) ? matrices_end : full_sample;
    RunAll<AVX2_8bit>(matrices, end, stats.avx2_8bit);
  }

  std::cerr << "AVX2 16bit, 100 samples..." << std::endl;
  for (int samples = 0; samples < kSamples; ++samples) {
    RandomMatrices *end = (samples < 4) ? matrices_end : full_sample;
    RunAll<AVX2_16bit>(matrices, end, stats.avx2_16bit);
  }

#ifdef INTGEMM_COMPILER_SUPPORTS_AVX512BW
  std::cerr << "AVX512 8bit, 100 samples..." << std::endl;
  for (int samples = 0; samples < kSamples; ++samples) {
    RandomMatrices *end = (samples < 4) ? matrices_end : full_sample;
    RunAll<AVX512_8bit>(matrices, end, stats.avx512_8bit);
  }

  std::cerr << "AVX512 16bit, 100 samples..." << std::endl;
  for (int samples = 0; samples < kSamples; ++samples) {
    RandomMatrices *end = (samples < 4) ? matrices_end : full_sample;
    RunAll<AVX512_16bit>(matrices, end, stats.avx512_16bit);
  }
#endif
#ifdef INTGEMM_COMPILER_SUPPORTS_AVX512VNNI
  std::cerr << "AVX512VNNI 8bit, 100 samples..." << std::endl;
  for (int samples = 0; samples < kSamples; ++samples) {
    RandomMatrices *end = (samples < 4) ? matrices_end : full_sample;
    RunAll<AVX512VNNI_8bit>(matrices, end, stats.avx512vnni_8bit);
  }
#endif

  if (stats.sse2_16bit.empty()) {
    std::cerr << "No CPU support." << std::endl;
    return 1;
  }
  for (std::size_t i = 0; i < sizeof(matrices) / sizeof(RandomMatrices); ++i) {
    std::cout << "Multiply\t" << matrices[i].A_rows << '\t' << matrices[i].width << '\t' << matrices[i].B_cols << '\t' << "Samples=" << (kOutlierThreshold * stats.sse2_16bit[i].size()) << '\n';
    Print<SSSE3_8bit>(stats.ssse3_8bit, i);
    Print<AVX2_8bit>(stats.avx2_8bit, i);
#ifdef INTGEMM_COMPILER_SUPPORTS_AVX512BW
    Print<AVX512_8bit>(stats.avx512_8bit, i);
#endif
#ifdef INTGEMM_COMPILER_SUPPORTS_AVX512VNNI
    Print<AVX512VNNI_8bit>(stats.avx512vnni_8bit, i);
#endif
    Print<SSE2_16bit>(stats.sse2_16bit, i);
    Print<AVX2_16bit>(stats.avx2_16bit, i);
#ifdef INTGEMM_COMPILER_SUPPORTS_AVX512BW
    Print<AVX512_16bit>(stats.avx512_16bit, i);
#endif
  }
  return 0;
}


