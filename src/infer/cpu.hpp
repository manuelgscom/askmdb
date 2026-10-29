#pragma once
// Number of inference threads.

namespace askmdb::infer {

inline constexpr int kMaxThreads = 8;

// Physical cores available to the process (affinity mask), at least 1, without a cap.
int physical_cores();

// physical_cores() capped at kMaxThreads.
int default_threads();

}  // namespace askmdb::infer
