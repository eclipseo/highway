// Copyright 2019 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "hwy/nanobenchmark.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>  // abort
#include <string.h>  // memcpy
#include <time.h>    // clock_gettime

#include <algorithm>  // sort
#include <array>
#include <atomic>
#include <limits>
#include <numeric>  // iota
#include <random>
#include <string>
#include <vector>

#if defined(_WIN32) || defined(_WIN64)
#ifndef NOMINMAX
#define NOMINMAX
#endif  // NOMINMAX
#include <windows.h>
#endif

#if defined(__MACH__)
#include <mach/mach.h>
#include <mach/mach_time.h>
#endif

#if defined(__HAIKU__)
#include <OS.h>
#endif

#include "hwy/base.h"
#if HWY_ARCH_PPC
#include <sys/platform/ppc.h>  // NOLINT __ppc_get_timebase_freq
#elif HWY_ARCH_X86

#if HWY_COMPILER_MSVC
#include <intrin.h>
#else
#include <cpuid.h>  // NOLINT
#endif              // HWY_COMPILER_MSVC

#endif  // HWY_ARCH_X86

namespace hwy {
namespace {
namespace timer {

// Ticks := platform-specific timer values (CPU cycles on x86). Must be
// unsigned to guarantee wraparound on overflow.
using Ticks = uint64_t;

// Start/Stop return absolute timestamps and must be placed immediately before
// and after the region to measure. We provide separate Start/Stop functions
// because they use different fences.
//
// Background: RDTSC is not 'serializing'; earlier instructions may complete
// after it, and/or later instructions may complete before it. 'Fences' ensure
// regions' elapsed times are independent of such reordering. The only
// documented unprivileged serializing instruction is CPUID, which acts as a
// full fence (no reordering across it in either direction). Unfortunately
// the latency of CPUID varies wildly (perhaps made worse by not initializing
// its EAX input). Because it cannot reliably be deducted from the region's
// elapsed time, it must not be included in the region to measure (i.e.
// between the two RDTSC).
//
// The newer RDTSCP is sometimes described as serializing, but it actually
// only serves as a half-fence with release semantics. Although all
// instructions in the region will complete before the final timestamp is
// captured, subsequent instructions may leak into the region and increase the
// elapsed time. Inserting another fence after the final RDTSCP would prevent
// such reordering without affecting the measured region.
//
// Fortunately, such a fence exists. The LFENCE instruction is only documented
// to delay later loads until earlier loads are visible. However, Intel's
// reference manual says it acts as a full fence (waiting until all earlier
// instructions have completed, and delaying later instructions until it
// completes). AMD assigns the same behavior to MFENCE.
//
// We need a fence before the initial RDTSC to prevent earlier instructions
// from leaking into the region, and arguably another after RDTSC to avoid
// region instructions from completing before the timestamp is recorded.
// When surrounded by fences, the additional RDTSCP half-fence provides no
// benefit, so the initial timestamp can be recorded via RDTSC, which has
// lower overhead than RDTSCP because it does not read TSC_AUX. In summary,
// we define Start = LFENCE/RDTSC/LFENCE; Stop = RDTSCP/LFENCE.
//
// Using Start+Start leads to higher variance and overhead than Stop+Stop.
// However, Stop+Stop includes an LFENCE in the region measurements, which
// adds a delay dependent on earlier loads. The combination of Start+Stop
// is faster than Start+Start and more consistent than Stop+Stop because
// the first LFENCE already delayed subsequent loads before the measured
// region. This combination seems not to have been considered in prior work:
// http://akaros.cs.berkeley.edu/lxr/akaros/kern/arch/x86/rdtsc_test.c
//
// Note: performance counters can measure 'exact' instructions-retired or
// (unhalted) cycle counts. The RDPMC instruction is not serializing and also
// requires fences. Unfortunately, it is not accessible on all OSes and we
// prefer to avoid kernel-mode drivers. Performance counters are also affected
// by several under/over-count errata, so we use the TSC instead.

// Returns a 64-bit timestamp in unit of 'ticks'; to convert to seconds,
// divide by InvariantTicksPerSecond.
inline Ticks Start() {
  Ticks t;
#if HWY_ARCH_PPC
  asm volatile("mfspr %0, %1" : "=r"(t) : "i"(268));
#elif HWY_ARCH_X86 && HWY_COMPILER_MSVC
  _ReadWriteBarrier();
  _mm_lfence();
  _ReadWriteBarrier();
  t = __rdtsc();
  _ReadWriteBarrier();
  _mm_lfence();
  _ReadWriteBarrier();
#elif HWY_ARCH_X86_64
  asm volatile(
      "lfence\n\t"
      "rdtsc\n\t"
      "shl $32, %%rdx\n\t"
      "or %%rdx, %0\n\t"
      "lfence"
      : "=a"(t)
      :
      // "memory" avoids reordering. rdx = TSC >> 32.
      // "cc" = flags modified by SHL.
      : "rdx", "memory", "cc");
#elif HWY_ARCH_RVV
  asm volatile("rdcycle %0" : "=r"(t));
#elif defined(_WIN32) || defined(_WIN64)
  LARGE_INTEGER counter;
  (void)QueryPerformanceCounter(&counter);
  t = counter.QuadPart;
#elif defined(__MACH__)
  t = mach_absolute_time();
#elif defined(__HAIKU__)
  t = system_time_nsecs();  // since boot
#else  // POSIX
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  t = static_cast<Ticks>(ts.tv_sec * 1000000000LL + ts.tv_nsec);
#endif
  return t;
}

inline Ticks Stop() {
  uint64_t t;
#if HWY_ARCH_PPC
  asm volatile("mfspr %0, %1" : "=r"(t) : "i"(268));
#elif HWY_ARCH_X86 && HWY_COMPILER_MSVC
  _ReadWriteBarrier();
  unsigned aux;
  t = __rdtscp(&aux);
  _ReadWriteBarrier();
  _mm_lfence();
  _ReadWriteBarrier();
#elif HWY_ARCH_X86_64
  // Use inline asm because __rdtscp generates code to store TSC_AUX (ecx).
  asm volatile(
      "rdtscp\n\t"
      "shl $32, %%rdx\n\t"
      "or %%rdx, %0\n\t"
      "lfence"
      : "=a"(t)
      :
      // "memory" avoids reordering. rcx = TSC_AUX. rdx = TSC >> 32.
      // "cc" = flags modified by SHL.
      : "rcx", "rdx", "memory", "cc");
#else
  t = Start();
#endif
  return t;
}

}  // namespace timer

namespace robust_statistics {

// Sorts integral values in ascending order (e.g. for Mode). About 3x faster
// than std::sort for input distributions with very few unique values.
template <class T>
void CountingSort(T* values, size_t num_values) {
  // Unique values and their frequency (similar to flat_map).
  using Unique = std::pair<T, int>;
  std::vector<Unique> unique;
  for (size_t i = 0; i < num_values; ++i) {
    const T value = values[i];
    const auto pos =
        std::find_if(unique.begin(), unique.end(),
                     [value](const Unique u) { return u.first == value; });
    if (pos == unique.end()) {
      unique.push_back(std::make_pair(value, 1));
    } else {
      ++pos->second;
    }
  }

  // Sort in ascending order of value (pair.first).
  std::sort(unique.begin(), unique.end());

  // Write that many copies of each unique value to the array.
  T* HWY_RESTRICT p = values;
  for (const auto& value_count : unique) {
    std::fill(p, p + value_count.second, value_count.first);
    p += value_count.second;
  }
  NANOBENCHMARK_CHECK(p == values + num_values);
}

// @return i in [idx_begin, idx_begin + half_count) that minimizes
// sorted[i + half_count] - sorted[i].
template <typename T>
size_t MinRange(const T* const HWY_RESTRICT sorted, const size_t idx_begin,
                const size_t half_count) {
  T min_range = std::numeric_limits<T>::max();
  size_t min_idx = 0;

  for (size_t idx = idx_begin; idx < idx_begin + half_count; ++idx) {
    NANOBENCHMARK_CHECK(sorted[idx] <= sorted[idx + half_count]);
    const T range = sorted[idx + half_count] - sorted[idx];
    if (range < min_range) {
      min_range = range;
      min_idx = idx;
    }
  }

  return min_idx;
}

// Returns an estimate of the mode by calling MinRange on successively
// halved intervals. "sorted" must be in ascending order. This is the
// Half Sample Mode estimator proposed by Bickel in "On a fast, robust
// estimator of the mode", with complexity O(N log N). The mode is less
// affected by outliers in highly-skewed distributions than the median.
// The averaging operation below assumes "T" is an unsigned integer type.
template <typename T>
T ModeOfSorted(const T* const HWY_RESTRICT sorted, const size_t num_values) {
  size_t idx_begin = 0;
  size_t half_count = num_values / 2;
  while (half_count > 1) {
    idx_begin = MinRange(sorted, idx_begin, half_count);
    half_count >>= 1;
  }

  const T x = sorted[idx_begin + 0];
  if (half_count == 0) {
    return x;
  }
  NANOBENCHMARK_CHECK(half_count == 1);
  const T average = (x + sorted[idx_begin + 1] + 1) / 2;
  return average;
}

// Returns the mode. Side effect: sorts "values".
template <typename T>
T Mode(T* values, const size_t num_values) {
  CountingSort(values, num_values);
  return ModeOfSorted(values, num_values);
}

template <typename T, size_t N>
T Mode(T (&values)[N]) {
  return Mode(&values[0], N);
}

// Returns the median value. Side effect: sorts "values".
template <typename T>
T Median(T* values, const size_t num_values) {
  NANOBENCHMARK_CHECK(!values->empty());
  std::sort(values, values + num_values);
  const size_t half = num_values / 2;
  // Odd count: return middle
  if (num_values % 2) {
    return values[half];
  }
  // Even count: return average of middle two.
  return (values[half] + values[half - 1] + 1) / 2;
}

// Returns a robust measure of variability.
template <typename T>
T MedianAbsoluteDeviation(const T* values, const size_t num_values,
                          const T median) {
  NANOBENCHMARK_CHECK(num_values != 0);
  std::vector<T> abs_deviations;
  abs_deviations.reserve(num_values);
  for (size_t i = 0; i < num_values; ++i) {
    const int64_t abs = std::abs(int64_t(values[i]) - int64_t(median));
    abs_deviations.push_back(static_cast<T>(abs));
  }
  return Median(abs_deviations.data(), num_values);
}

}  // namespace robust_statistics
}  // namespace
namespace platform {
namespace {

// Prevents the compiler from eliding the computations that led to "output".
template <class T>
inline void PreventElision(T&& output) {
#if HWY_COMPILER_MSVC == 0
  // Works by indicating to the compiler that "output" is being read and
  // modified. The +r constraint avoids unnecessary writes to memory, but only
  // works for built-in types (typically FuncOutput).
  asm volatile("" : "+r"(output) : : "memory");
#else
  // MSVC does not support inline assembly anymore (and never supported GCC's
  // RTL constraints). Self-assignment with #pragma optimize("off") might be
  // expected to prevent elision, but it does not with MSVC 2015. Type-punning
  // with volatile pointers generates inefficient code on MSVC 2017.
  static std::atomic<T> dummy(T{});
  dummy.store(output, std::memory_order_relaxed);
#endif
}

#if HWY_ARCH_X86

void Cpuid(const uint32_t level, const uint32_t count,
           uint32_t* HWY_RESTRICT abcd) {
#if HWY_COMPILER_MSVC
  int regs[4];
  __cpuidex(regs, level, count);
  for (int i = 0; i < 4; ++i) {
    abcd[i] = regs[i];
  }
#else
  uint32_t a;
  uint32_t b;
  uint32_t c;
  uint32_t d;
  __cpuid_count(level, count, a, b, c, d);
  abcd[0] = a;
  abcd[1] = b;
  abcd[2] = c;
  abcd[3] = d;
#endif
}

std::string BrandString() {
  char brand_string[49];
  std::array<uint32_t, 4> abcd;

  // Check if brand string is supported (it is on all reasonable Intel/AMD)
  Cpuid(0x80000000U, 0, abcd.data());
  if (abcd[0] < 0x80000004U) {
    return std::string();
  }

  for (size_t i = 0; i < 3; ++i) {
    Cpuid(static_cast<uint32_t>(0x80000002U + i), 0, abcd.data());
    memcpy(brand_string + i * 16, abcd.data(), sizeof(abcd));
  }
  brand_string[48] = 0;
  return brand_string;
}

// Returns the frequency quoted inside the brand string. This does not
// account for throttling nor Turbo Boost.
double NominalClockRate() {
  const std::string& brand_string = BrandString();
  // Brand strings include the maximum configured frequency. These prefixes are
  // defined by Intel CPUID documentation.
  const char* prefixes[3] = {"MHz", "GHz", "THz"};
  const double multipliers[3] = {1E6, 1E9, 1E12};
  for (size_t i = 0; i < 3; ++i) {
    const size_t pos_prefix = brand_string.find(prefixes[i]);
    if (pos_prefix != std::string::npos) {
      const size_t pos_space = brand_string.rfind(' ', pos_prefix - 1);
      if (pos_space != std::string::npos) {
        const std::string digits =
            brand_string.substr(pos_space + 1, pos_prefix - pos_space - 1);
        return std::stod(digits) * multipliers[i];
      }
    }
  }

  return 0.0;
}

#endif  // HWY_ARCH_X86

}  // namespace

double InvariantTicksPerSecond() {
#if HWY_ARCH_PPC
  return __ppc_get_timebase_freq();
#elif HWY_ARCH_X86
  // We assume the TSC is invariant; it is on all recent Intel/AMD CPUs.
  return NominalClockRate();
#elif defined(_WIN32) || defined(_WIN64)
  LARGE_INTEGER freq;
  (void)QueryPerformanceFrequency(&freq);
  return double(freq.QuadPart);
#elif defined(__MACH__)
  // https://developer.apple.com/library/mac/qa/qa1398/_index.html
  mach_timebase_info_data_t timebase;
  (void)mach_timebase_info(&timebase);
  return double(timebase.denom) / timebase.numer * 1E9;
#else
  // TODO(janwas): ARM? Unclear how to reliably query cntvct_el0 frequency.
  return 1E9;  // Haiku and clock_gettime return nanoseconds.
#endif
}

double Now() {
  static const double mul = 1.0 / InvariantTicksPerSecond();
  return static_cast<double>(timer::Start()) * mul;
}

uint64_t TimerResolution() {
  // Nested loop avoids exceeding stack/L1 capacity.
  timer::Ticks repetitions[Params::kTimerSamples];
  for (size_t rep = 0; rep < Params::kTimerSamples; ++rep) {
    timer::Ticks samples[Params::kTimerSamples];
    for (size_t i = 0; i < Params::kTimerSamples; ++i) {
      const timer::Ticks t0 = timer::Start();
      const timer::Ticks t1 = timer::Stop();
      samples[i] = t1 - t0;
    }
    repetitions[rep] = robust_statistics::Mode(samples);
  }
  return robust_statistics::Mode(repetitions);
}

}  // namespace platform
namespace {

static const timer::Ticks timer_resolution = platform::TimerResolution();

// Estimates the expected value of "lambda" values with a variable number of
// samples until the variability "rel_mad" is less than "max_rel_mad".
template <class Lambda>
timer::Ticks SampleUntilStable(const double max_rel_mad, double* rel_mad,
                               const Params& p, const Lambda& lambda) {
  // Choose initial samples_per_eval based on a single estimated duration.
  timer::Ticks t0 = timer::Start();
  lambda();
  timer::Ticks t1 = timer::Stop();
  timer::Ticks est = t1 - t0;
  static const double ticks_per_second = platform::InvariantTicksPerSecond();
  const size_t ticks_per_eval =
      static_cast<size_t>(ticks_per_second * p.seconds_per_eval);
  size_t samples_per_eval = est == 0
                                ? p.min_samples_per_eval
                                : static_cast<size_t>(ticks_per_eval / est);
  samples_per_eval = HWY_MAX(samples_per_eval, p.min_samples_per_eval);

  std::vector<timer::Ticks> samples;
  samples.reserve(1 + samples_per_eval);
  samples.push_back(est);

  // Percentage is too strict for tiny differences, so also allow a small
  // absolute "median absolute deviation".
  const timer::Ticks max_abs_mad = (timer_resolution + 99) / 100;
  *rel_mad = 0.0;  // ensure initialized

  for (size_t eval = 0; eval < p.max_evals; ++eval, samples_per_eval *= 2) {
    samples.reserve(samples.size() + samples_per_eval);
    for (size_t i = 0; i < samples_per_eval; ++i) {
      t0 = timer::Start();
      lambda();
      t1 = timer::Stop();
      samples.push_back(t1 - t0);
    }

    if (samples.size() >= p.min_mode_samples) {
      est = robust_statistics::Mode(samples.data(), samples.size());
    } else {
      // For "few" (depends also on the variance) samples, Median is safer.
      est = robust_statistics::Median(samples.data(), samples.size());
    }
    NANOBENCHMARK_CHECK(est != 0);

    // Median absolute deviation (mad) is a robust measure of 'variability'.
    const timer::Ticks abs_mad = robust_statistics::MedianAbsoluteDeviation(
        samples.data(), samples.size(), est);
    *rel_mad = static_cast<double>(abs_mad) / static_cast<double>(est);

    if (*rel_mad <= max_rel_mad || abs_mad <= max_abs_mad) {
      if (p.verbose) {
        printf("%6zu samples => %5zu (abs_mad=%4zu, rel_mad=%4.2f%%)\n",
               samples.size(), size_t(est), size_t(abs_mad), *rel_mad * 100.0);
      }
      return est;
    }
  }

  if (p.verbose) {
    printf(
        "WARNING: rel_mad=%4.2f%% still exceeds %4.2f%% after %6zu samples.\n",
        *rel_mad * 100.0, max_rel_mad * 100.0, samples.size());
  }
  return est;
}

using InputVec = std::vector<FuncInput>;

// Returns vector of unique input values.
InputVec UniqueInputs(const FuncInput* inputs, const size_t num_inputs) {
  InputVec unique(inputs, inputs + num_inputs);
  std::sort(unique.begin(), unique.end());
  unique.erase(std::unique(unique.begin(), unique.end()), unique.end());
  return unique;
}

// Returns how often we need to call func for sufficient precision.
size_t NumSkip(const Func func, const uint8_t* arg, const InputVec& unique,
               const Params& p) {
  // Min elapsed ticks for any input.
  timer::Ticks min_duration = ~timer::Ticks(0);

  for (const FuncInput input : unique) {
    double rel_mad;
    const timer::Ticks total = SampleUntilStable(
        p.target_rel_mad, &rel_mad, p,
        [func, arg, input]() { platform::PreventElision(func(arg, input)); });
    min_duration = HWY_MIN(min_duration, total - timer_resolution);
  }

  // Number of repetitions required to reach the target resolution.
  const size_t max_skip = p.precision_divisor;
  // Number of repetitions given the estimated duration.
  const size_t num_skip =
      min_duration == 0
          ? 0
          : static_cast<size_t>((max_skip + min_duration - 1) / min_duration);
  if (p.verbose) {
    printf("res=%zu max_skip=%zu min_dur=%zu num_skip=%zu\n",
           size_t(timer_resolution), max_skip, size_t(min_duration), num_skip);
  }
  return num_skip;
}

// Replicates inputs until we can omit "num_skip" occurrences of an input.
InputVec ReplicateInputs(const FuncInput* inputs, const size_t num_inputs,
                         const size_t num_unique, const size_t num_skip,
                         const Params& p) {
  InputVec full;
  if (num_unique == 1) {
    full.assign(p.subset_ratio * num_skip, inputs[0]);
    return full;
  }

  full.reserve(p.subset_ratio * num_skip * num_inputs);
  for (size_t i = 0; i < p.subset_ratio * num_skip; ++i) {
    full.insert(full.end(), inputs, inputs + num_inputs);
  }
  std::mt19937 rng;
  std::shuffle(full.begin(), full.end(), rng);
  return full;
}

// Copies the "full" to "subset" in the same order, but with "num_skip"
// randomly selected occurrences of "input_to_skip" removed.
void FillSubset(const InputVec& full, const FuncInput input_to_skip,
                const size_t num_skip, InputVec* subset) {
  const size_t count =
      static_cast<size_t>(std::count(full.begin(), full.end(), input_to_skip));
  // Generate num_skip random indices: which occurrence to skip.
  std::vector<uint32_t> omit(count);
  std::iota(omit.begin(), omit.end(), 0);
  // omit[] is the same on every call, but that's OK because they identify the
  // Nth instance of input_to_skip, so the position within full[] differs.
  std::mt19937 rng;
  std::shuffle(omit.begin(), omit.end(), rng);
  omit.resize(num_skip);
  std::sort(omit.begin(), omit.end());

  uint32_t occurrence = ~0u;  // 0 after preincrement
  size_t idx_omit = 0;        // cursor within omit[]
  size_t idx_subset = 0;      // cursor within *subset
  for (const FuncInput next : full) {
    if (next == input_to_skip) {
      ++occurrence;
      // Haven't removed enough already
      if (idx_omit < num_skip) {
        // This one is up for removal
        if (occurrence == omit[idx_omit]) {
          ++idx_omit;
          continue;
        }
      }
    }
    if (idx_subset < subset->size()) {
      (*subset)[idx_subset++] = next;
    }
  }
  NANOBENCHMARK_CHECK(idx_subset == subset->size());
  NANOBENCHMARK_CHECK(idx_omit == omit.size());
  NANOBENCHMARK_CHECK(occurrence == count - 1);
}

// Returns total ticks elapsed for all inputs.
timer::Ticks TotalDuration(const Func func, const uint8_t* arg,
                           const InputVec* inputs, const Params& p,
                           double* max_rel_mad) {
  double rel_mad;
  const timer::Ticks duration =
      SampleUntilStable(p.target_rel_mad, &rel_mad, p, [func, arg, inputs]() {
        for (const FuncInput input : *inputs) {
          platform::PreventElision(func(arg, input));
        }
      });
  *max_rel_mad = HWY_MAX(*max_rel_mad, rel_mad);
  return duration;
}

// (Nearly) empty Func for measuring timer overhead/resolution.
HWY_NOINLINE FuncOutput EmptyFunc(const void* /*arg*/, const FuncInput input) {
  return input;
}

// Returns overhead of accessing inputs[] and calling a function; this will
// be deducted from future TotalDuration return values.
timer::Ticks Overhead(const uint8_t* arg, const InputVec* inputs,
                      const Params& p) {
  double rel_mad;
  // Zero tolerance because repeatability is crucial and EmptyFunc is fast.
  return SampleUntilStable(0.0, &rel_mad, p, [arg, inputs]() {
    for (const FuncInput input : *inputs) {
      platform::PreventElision(EmptyFunc(arg, input));
    }
  });
}

}  // namespace

int Unpredictable1() { return timer::Start() != ~0ULL; }

size_t Measure(const Func func, const uint8_t* arg, const FuncInput* inputs,
               const size_t num_inputs, Result* results, const Params& p) {
  NANOBENCHMARK_CHECK(num_inputs != 0);
  const InputVec& unique = UniqueInputs(inputs, num_inputs);

  const size_t num_skip = NumSkip(func, arg, unique, p);  // never 0
  if (num_skip == 0) return 0;  // NumSkip already printed error message
  // (slightly less work on x86 to cast from signed integer)
  const float mul = 1.0f / static_cast<float>(static_cast<int>(num_skip));

  const InputVec& full =
      ReplicateInputs(inputs, num_inputs, unique.size(), num_skip, p);
  InputVec subset(full.size() - num_skip);

  const timer::Ticks overhead = Overhead(arg, &full, p);
  const timer::Ticks overhead_skip = Overhead(arg, &subset, p);
  if (overhead < overhead_skip) {
    fprintf(stderr, "Measurement failed: overhead %zu < %zu\n",
            size_t(overhead), size_t(overhead_skip));
    return 0;
  }

  if (p.verbose) {
    printf("#inputs=%5zu,%5zu overhead=%5zu,%5zu\n", full.size(), subset.size(),
           size_t(overhead), size_t(overhead_skip));
  }

  double max_rel_mad = 0.0;
  const timer::Ticks total = TotalDuration(func, arg, &full, p, &max_rel_mad);

  for (size_t i = 0; i < unique.size(); ++i) {
    FillSubset(full, unique[i], num_skip, &subset);
    const timer::Ticks total_skip =
        TotalDuration(func, arg, &subset, p, &max_rel_mad);

    if (total < total_skip) {
      fprintf(stderr, "Measurement failed: total %zu < %zu\n", size_t(total),
              size_t(total_skip));
      return 0;
    }

    const timer::Ticks duration =
        (total - overhead) - (total_skip - overhead_skip);
    results[i].input = unique[i];
    results[i].ticks = static_cast<float>(duration) * mul;
    results[i].variability = static_cast<float>(max_rel_mad);
  }

  return unique.size();
}

}  // namespace hwy
