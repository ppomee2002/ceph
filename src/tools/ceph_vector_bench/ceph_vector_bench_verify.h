// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
#pragma once

struct VectorBenchOptions;

/** LSH locality + optional GT pair colocation; no RADOS. */
int run_vector_bench_verify(const VectorBenchOptions& opt);
