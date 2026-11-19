# Validation and benchmark method

Run from the repository root. The suite needs a C compiler, POSIX threads, Make, Python 3.9+ and loopback TCP. The Linux spawn implementation uses glibc 2.34+.

```sh
make clean && make -j2 test
python3 tests/integration.py Integration.test_parallel_snapshots

make clean && ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  make -j2 test CFLAGS='-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined'

make clean && TSAN_OPTIONS=halt_on_error=1 \
  make -j2 test CFLAGS='-O1 -g -fno-omit-frame-pointer -fsanitize=thread'

make clean && make -j2 && make bench
```

The optimized build uses C11, -O2 -g, -Wall -Wextra -Wpedantic -Werror and pthreads. Clean between instrumented builds so binaries and objects use the same flags.

| Check | What the test observes |
| --- | --- |
| Reader overlap and writer priority | A second reader completes while the first holds the log. A test-only observation of the actual condition wait establishes that a later reader blocks behind a waiting writer. The writer must finish first. |
| Static, dynamic and errors | One worker handles a fixed sequence. Status, body, all counters, timestamp order and the exact serialized log are checked. |
| Mixed traffic | Each of three worker/capacity configurations handles 144 requests with 12 clients. Every GET entry appears once, every POST snapshot is a complete prefix of the final log, and worker counts are consecutive. |
| FIFO and admission | Four partially sent requests are completed in reverse send order. One-worker dispatch must follow connection order. With capacity one the next accept timestamp must follow the preceding log commit. |
| Debug sleep | Both GET and POST must spend at least 0.99 seconds inside a one-second logger operation. |
| Protocol and lifecycle | Incomplete headers and early disconnects are followed by a successful request. Invalid CLI exits are checked. Each test server must drain on termination and exit successfully. |
| CGI output and child error | A 128 KiB response is compared byte for byte. A failed CGI returns 500 and a subsequent GET succeeds. |

The C monitor test has a 15-second alarm. Integration tests use socket and subprocess deadlines plus a 180-second suite alarm. GitHub Actions defines release, ASan/UBSan and separate TSan jobs. Local Linux execution and remote CI status are reported separately.

## Interpreting the benchmark

Eight closed-loop Python client threads issue one GET per TCP connection. Each trial starts a new server, warms up with eight requests, then times 80 requests. The driver validates status, content length and statistics headers. It takes the median of three trial throughputs and of each trial's nearest-rank p50 and p95. The static workload fetches the bundled home page. The dynamic workload spawns the bundled CGI with a requested 25 ms delay. Client scheduling, socket setup, parsing and guest emulation are included. There is no debug sleep. This is a small end-to-end experiment, not a hardware capacity estimate.

The starter's single-thread demo uses a dummy log, no-op append and placeholder statistics. The one-worker configuration instead measures the same implementation and the same work. It still has a master thread.
