// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
#pragma once

struct VectorBenchOptions;

/** Fan-out load: .fvecs -> RADOS with LSH PG placement + AIO writes. */
int run_vector_bench_load(const VectorBenchOptions& opt);
