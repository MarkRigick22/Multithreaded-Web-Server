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

## Audit findings against the recovered copy

| Area | Finding | Change |
| --- | --- | --- |
| Admission | Master accepted one extra client before waiting for capacity. | Wait before accept; count active jobs through socket close. |
| Book | GET saved a zero completion timestamp in its record, then sent a different timestamp on the wire. POST skipped the debug sleep. | Use one commit timestamp and matching bytes. Apply sleep under both logical ownership modes. |
| Input and errors | Unchecked request-line parsing and repeated header reads at EOF. Fatal I/O wrappers could terminate the server on client errors with status zero. | Bound parsing, check framing and EOF, isolate socket errors to the connection. |
| File and process I/O | Fixed-size CGI capture could truncate output and stall waiting for the child. Child called environment routines after multi-threaded fork. The demo formatted a buffer over itself. | Use spawn actions and per-child environment, spool then stream output, validate child status, format demo output once. |
| Size and accounting | Book length and file size narrowed to signed int. Several formatting or copy operations lacked capacity checks. | Use size-aware formatting and allocation, stream files, and use 64-bit worker counters. |
| Lifecycle | Numeric prefix parsing accepted invalid arguments; fatal wrappers reported success; cleanup was unreachable. | Range-check CLI input, distinguish failure exit status and drain/join the pool on termination. |

## Recorded runs

On the ARM64 Linux guest, the GCC 13.3 optimized build passed the C monitor and seven integration cases. Two additional optimized integration runs passed. GCC 13.3 and Clang 18.1.3 each passed the full suite in both sanitizer configurations (ASan plus UBSan, and independently TSan), with no reported diagnostics. Leak detection was enabled in ASan runs. macOS Apple Clang 21 compiled the optimized targets; the host environment blocked TCP bind, so end-to-end socket results above are Linux results. The hosted Actions workflow has not run as of this local audit.
