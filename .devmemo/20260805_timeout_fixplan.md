## Investigation Results

The root cause is that **the timeout is being counted by the number of `poll()` calls rather than by elapsed wall-clock time**.

The current implementation is:

```c
while (waited < LAUNCH_TIMEOUT_MS && status < 0) {
    if (read_status_fd(sfd, &status)) break;

    struct pollfd pfds[2] = {
        { .fd = rfd, .events = POLLIN | POLLHUP },
        { .fd = sfd, .events = POLLIN | POLLHUP }
    };

    poll(pfds, 2, 100);
    waited += 100;
}
```

The `100` in `poll(..., 100)` does **not** mean "always wait 100 ms." It means **wait up to 100 ms**. If either file descriptor becomes ready, `poll()` returns immediately.

Meanwhile, the inner shell operates in the following order:

1. Write the `tmux` output to `result.fifo`
2. Obtain the `tmux` exit status
3. Write the exit status to `status.fifo`

This sequence can be confirmed in the target code.

As a result, `result.fifo` typically reaches the following state before `status.fifo`:

* The session ID is written, generating `POLLIN`
* The writer closes the FIFO, generating `POLLHUP`
* The parent does not read `result.fifo` during the waiting loop, so the ready state is never cleared

`POLLHUP` is generated when the writing side closes the descriptor, and if unread data remains, it may occur simultaneously with `POLLIN`.

This produces the following behavior:

```text
poll() returns immediately because result.fifo has POLLIN/POLLHUP
waited += 100
poll() returns immediately again
waited += 100
...
300 iterations
waited == 30000
```

Although only a few milliseconds have actually elapsed, the program believes that 30 seconds have passed.

### Reproduction Results

Using a C test program with the same structure, where `status.fifo` is written 10 ms after closing `result.fifo`, the following results were obtained:

```text
mode=old   elapsed_ms=0  loops=300 status=-1
mode=fixed elapsed_ms=11 loops=1   status=0
```

Therefore, the WSL startup time and the value of `LAUNCH_TIMEOUT_MS` are **not** the direct cause.

---

## Fix Option 1: Minimal Change

During the waiting loop, poll only `status.fifo`.

At this stage, `result.fifo` is not being read, so it should not be included in the poll set. In addition, determine the timeout using actual elapsed time measured with `CLOCK_MONOTONIC`.

```diff
 #include <sys/stat.h>
 #include <sys/types.h>
+#include <time.h>
 #include <unistd.h>

 #define SEP_CHAR '\037'
 #define LAUNCH_TIMEOUT_MS 30000

+static long long
+monotonic_ms(void)
+{
+    struct timespec ts;
+
+    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
+        die("clock_gettime");
+
+    return (long long)ts.tv_sec * 1000 +
+           ts.tv_nsec / 1000000;
+}
+
 int main(int argc, char **argv)
 {
     ...
     int status = -1;
-    int waited = 0;
-    while (waited < LAUNCH_TIMEOUT_MS && status < 0) {
+    long long deadline = monotonic_ms() + LAUNCH_TIMEOUT_MS;
+
+    while (status < 0) {
         if (read_status_fd(sfd, &status)) break;
-        struct pollfd pfds[2] = {
-            { .fd = rfd, .events = POLLIN | POLLHUP },
-            { .fd = sfd, .events = POLLIN | POLLHUP }
-        };
-        poll(pfds, 2, 100);
-        waited += 100;
+
+        long long remaining = deadline - monotonic_ms();
+        if (remaining <= 0)
+            break;
+
+        struct pollfd pfd = {
+            .fd = sfd,
+            .events = POLLIN
+        };
+
+        /*
+         * Keeping the polling interval at 100 ms makes it easier
+         * to add waitpid(WNOHANG) or similar logic here later.
+         */
+        int timeout = remaining > 100 ? 100 : (int)remaining;
+        int rc;
+
+        do {
+            rc = poll(&pfd, 1, timeout);
+        } while (rc < 0 && errno == EINTR);
+
+        if (rc < 0)
+            die("poll status fifo");
+
+        if (pfd.revents & (POLLERR | POLLNVAL)) {
+            errno = EIO;
+            die("poll status fifo");
+        }
+
+        /*
+         * The status writer connected and then closed the FIFO
+         * without writing anything. Under this protocol there is
+         * no reason to wait for another writer.
+         */
+        if ((pfd.revents & POLLHUP) &&
+            !(pfd.revents & POLLIN)) {
+            fprintf(stderr,
+                "tmux wrapper: WSL launch closed status FIFO "
+                "without returning a status\n");
+            return 1;
+        }
     }
```

### Benefits of This Fix

* Prevents the busy loop caused by `POLLIN`/`POLLHUP` on `result.fifo`
* Prevents timeout until a real 30 seconds have elapsed
* Keeps timeout calculations correct even if `poll()` is interrupted by a signal
* Requires only a small code change

Although `LAUNCH_TIMEOUT_MS` is currently defined as 30,000 ms, the waiting loop effectively invalidates that meaning.

---

## Fix Option 2: Recommended Robust Solution

A more robust design is to monitor both `result.fifo` and `status.fifo` simultaneously while **draining `result.fifo` immediately whenever it becomes ready**.

The current implementation never reads `result.fifo` until the status arrives. As a result, if the output from `tmux -P -F` exceeds the FIFO buffer capacity, the following deadlock is theoretically possible:

```text
tmux:
  write to result.fifo blocks because the FIFO is full
  ↓
  tmux never exits
  ↓
  status.fifo is never written

wrapper:
  waits indefinitely for status.fifo
  never reads result.fifo
```

The recommended event loop is:

```text
deadline = CLOCK_MONOTONIC + timeout

while status not received:
    poll(result.fifo, status.fifo, remaining_time)

    if result.fifo has POLLIN:
        read until EAGAIN
        accumulate everything before SEP as session_id
        write or discard everything after SEP

    if result.fifo has POLLHUP:
        drain any remaining data
        remove result FD from the poll set

    if status.fifo has POLLIN:
        read the status

    if status.fifo has POLLHUP without a status:
        protocol error

    if monotonic deadline has expired:
        timeout
```

With this approach, every `POLLIN` event is immediately consumed, so level-triggered readiness never persists.

---

## Additional Improvements Worth Making

### 1. Reap the `cmd.exe` Child Process

The current code stores the PID returned by `fork()`, but never calls `waitpid()`.

```c
pid_t pid = fork();
...
if (pid == 0) {
    execv(cmd_exe, cmd.v);
    _exit(127);
}
```

As a result, after `cmd.exe /c start` exits, the wrapper may `exec()` into `tmux attach-session` and continue running for a long time, leaving the child as a zombie process.

A reasonable solution is to periodically execute the following inside the waiting loop:

```c
int launcher_status;
pid_t w = waitpid(pid, &launcher_status, WNOHANG);

if (w == pid) {
    if (!WIFEXITED(launcher_status) ||
        WEXITSTATUS(launcher_status) != 0) {
        fprintf(stderr,
            "tmux wrapper: cmd.exe launcher failed\n");
        return 1;
    }

    pid = -1;
} else if (w < 0 && errno != EINTR) {
    die("waitpid");
}
```

This also allows launcher failures in `cmd.exe` itself to be detected immediately rather than waiting for the full 30-second timeout.

### 2. Make the Timeout Configurable

The shell version already supports:

```sh
LAUNCH_TIMEOUT=${TMUX_WSL_LAUNCH_TIMEOUT:-15}
```

The C version currently hardcodes 30 seconds.

For example, adding millisecond-based configuration makes testing easier:

```c
static int
launch_timeout_ms(void)
{
    const char *s = getenv("TMUX_WSL_LAUNCH_TIMEOUT_MS");
    if (s == NULL || *s == '\0')
        return LAUNCH_TIMEOUT_MS;

    char *end;
    errno = 0;
    long value = strtol(s, &end, 10);

    if (errno != 0 || *end != '\0' ||
        value < 1 || value > 600000) {
        fprintf(stderr,
            "tmux wrapper: invalid "
            "TMUX_WSL_LAUNCH_TIMEOUT_MS\n");
        exit(1);
    }

    return (int)value;
}
```

---

## Recommended Approach

The following changes should be applied first:

1. Poll only `status.fifo`.
2. Measure timeout using `CLOCK_MONOTONIC`.
3. Handle `poll()` return values and `EINTR`.
4. Reap the `cmd.exe` child with `waitpid(..., WNOHANG)`.

After that, if necessary, the implementation can be extended into an event loop that continuously drains `result.fifo`.

To fix the immediate timeout issue alone, **Fix Option 1 is highly likely to resolve the problem.**
