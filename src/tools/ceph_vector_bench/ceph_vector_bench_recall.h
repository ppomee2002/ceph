// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
#pragma once

struct VectorBenchOptions;

/** Pool scan + multi-table fan-out Recall@k vs ground truth. */
int run_vector_bench_recall(const VectorBenchOptions& opt);
