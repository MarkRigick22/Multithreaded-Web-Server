# Architecture

The server uses one master thread, a fixed pool of N workers and one shared append-only book. An accepted TCP connection carries one HTTP request. The worker closes the socket after the response.

## Admission and dispatch

The master waits on `not_full` while pending plus active equals capacity. It then waits for the listener or shutdown pipe to become readable and calls nonblocking `accept`. Only the master increases occupancy. Between checking capacity and admitting the connection, workers can only leave occupancy unchanged or reduce it. Excess connections wait in the kernel backlog. Their arrival statistic starts at accept time, so it excludes time spent in that backlog.

Under the queue mutex, enqueue adds at the tail. Dequeue removes the head and moves exactly one job from pending to active. Completion decrements active after closing the client socket and signals the master. Empty-queue workers wait on `not_empty`. Predicate loops tolerate spurious wakeups. The queue mutex is released before HTTP reads, file access, CGI, log operations and socket writes. FIFO describes dequeue order. Multiple workers can finish in a different order.

Each worker owns its counters for its lifetime. IDs start at one. A nonempty request line increments the total, even if parsing or routing subsequently fails. Successful GET increments the static or dynamic counter. A successful POST snapshot increments the POST counter. A connection closed before any request line arrives contributes zero requests.

## Book ownership and timestamps

A short-held mutex protects three fields: the number of active readers, the presence of an active writer, and the number of waiting writers. Each caller acquires logical ownership before releasing this mutex.

| Operation | Admission predicate | Release behavior |
| --- | --- | --- |
| POST snapshot | No active writer and no waiting writer | Last reader signals a waiting writer |
| GET append | No active writer and zero active readers | Signal a waiting writer, otherwise wake all readers |

A waiting writer increments its count before sleeping. New readers therefore yield to a queued writer. Waiters within the writer group have no FIFO guarantee. A continuous stream of writers can delay readers indefinitely.

The debug sleep occurs inside logical ownership, with the monitor mutex released. POST readers can sleep concurrently. GET writers cannot. GET records its log timestamp immediately before formatting and committing the entry under exclusivity. POST timestamps completion of its copy under shared ownership. The sending worker owns the snapshot and writes it after releasing the read lock. Network backpressure cannot hold the book lock. The clock is wall time to match the assignment. Benchmark durations use a monotonic clock.

## CGI, errors and shutdown

The document root is resolved before serving. Only a trusted operator should control its contents and CGI programs. Static responses stream from an open file. CGI runs with a per-child query environment and explicit standard descriptors. The spawn actions prevent other server descriptors from reaching the child. The worker waits for that specific child and streams its temporary output file. This preserves the assignment's CGI output, including CGI-generated header text inside the outer HTTP body. Disk space and child execution time depend on the installed program. The bundled delay demo accepts 0–60 seconds.

SIGINT or SIGTERM writes to a nonblocking wake pipe. The master stops admission, wakes idle workers and joins the pool after admitted jobs drain. A full queue can delay shutdown until a worker finishes. Each socket read or write has a ten-second inactivity timeout. A slow but continuously progressing peer can hold a worker longer. The process owns no persistent log across restarts.
