"""Closed-loop benchmark, with validated responses and nearest-rank percentiles."""
from concurrent.futures import ThreadPoolExecutor
import math
from pathlib import Path
import statistics
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tests"))
from integration import request, server

print("workload workers capacity requests/s p50_ms p95_ms", flush=True)
for path in ("/", "/output.cgi?0.025"):
    for workers, capacity in ((1, 8), (4, 1), (4, 4), (4, 8)):
        trials = []
        for trial in range(3):
            with server(workers, capacity) as port, ThreadPoolExecutor(8) as pool:
                def fetch(_):
                    before = time.perf_counter()
                    if request(port, "GET", path)[0] != 200:
                        raise RuntimeError("unsuccessful GET")
                    return 1000 * (time.perf_counter() - before)
                list(pool.map(fetch, range(8)))
                before = time.perf_counter()
                times = sorted(pool.map(fetch, range(80)))
                elapsed = time.perf_counter() - before
                trials.append((80 / elapsed, times[39], times[math.ceil(.95 * 80)-1]))
        print(path, workers, capacity, *(f"{statistics.median(v):.2f}" for v in zip(*trials)),
              flush=True)
