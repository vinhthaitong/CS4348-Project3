# Project 3: Pipelined File Search

Team number: **To be confirmed**  
Member 1: **To be confirmed**  
Member 2: **To be confirmed**

## Build and run

```sh
make
./pipegrep 10 -1 -1 -1 "error"
make clean
```

The Makefile uses `g++ -std=c++17 -pthread` and creates `pipegrep` in this directory. C++17, POSIX directory/metadata functions, and thread support are required. `make clean` removes only the executable. The required run on `cs1.utdallas.edu`, `cs2.utdallas.edu`, or `giant.utdallas.edu` is still outstanding.

The five arguments are buffer capacity (positive record count), minimum file size in bytes (`-1` disables), owner UID (`-1` disables), group GID (`-1` disables), and a nonempty literal, case-sensitive substring. Files must be strictly larger than an enabled size threshold and pass every enabled filter. Quote substrings containing spaces.

The program searches regular files directly in the **current working directory**, including hidden and extensionless files. It does not recurse or follow symbolic links. To search a different directory, build first, change into the input directory, and invoke the executable by its absolute path. Keep captured output and timing files outside that input directory.

Matching lines print as `./filename(line_number): line text`. The final stdout line is `***** You found N matches *****`. Each matching line counts once. Errors go to stderr; success, including zero matches, returns 0. Invalid arguments, file errors, and fatal failures return nonzero.

## Small manual test

From the project directory, build and create input outside it:

```sh
make
project_dir="$PWD"
test_dir="$(mktemp -d)"
printf 'hello world\nan error occurred\nerror error\nERROR\n' > "$test_dir/sample.txt"
cd "$test_dir"
"$project_dir/pipegrep" 10 -1 -1 -1 "error"
cd "$project_dir"
```

Expected output:

```text
./sample.txt(2): an error occurred
./sample.txt(3): error error
***** You found 2 matches *****
```

The repeated substring counts once and uppercase `ERROR` does not match. Search a separate input directory to avoid including source, documentation, and executable contents.

## Numeric types

The program uses basic `int`, `unsigned long long`, `bool`, and `char` types. `unsigned long long` stores buffer capacity, numeric argument values, line numbers, and match counts because `int` is too small for some valid IDs, file sizes, and counts. `size_t`, `off_t`, `uid_t`, and `gid_t` remain only in limit checks for the platform's supported sizes/IDs. Strings, queues, and synchronization objects are still needed for the pipeline. Floating-point `double` is not used for exact counts or IDs.

After this numeric-type simplification, the program rebuilt without warnings and passed 30 targeted checks for exact output, capacities 1/10/100 and the maximum local capacity value, malformed/overflowing numbers, size/UID/GID limits, actual ownership, and strict file-size boundaries. The full-suite and benchmark results below were recorded before this type-only change; they were not rerun afterward.

## Pipeline and synchronization

Main parses arguments, constructs four buffers, starts five `std::thread` workers, joins them, and returns the error status.

| Worker | Task | Output buffer record |
| --- | --- | --- |
| 1 | Enumerate current-directory candidates | B1: relative path string beginning `./` |
| 2 | Use `lstat` to filter regular files by size/UID/GID | B2: eligible path string |
| 3 | Read files with `std::getline`, numbering from 1 | B3: `Line { filename, number, text }` |
| 4 | Select lines with `std::string::find` | B4: complete matching `Line` |
| 5 | Print records and final count | No output buffer |

Each `BoundedBuffer<T>` is a FIFO queue with capacity `buffsize`, one mutex, two condition variables, and a closed flag. All four buffers use this same approach. Producers block until space exists; consumers block until data exists or the buffer is closed. Waits use predicates, so waking does not incorrectly imply the queue is ready. There is no busy waiting.

### Critical sections

- `push`: while holding that buffer's mutex, test available space and insert the record. This prevents a race between checking capacity and changing the queue. The condition-variable wait releases the mutex while sleeping.
- `pop`: under the same mutex, test data/completion, move out the front record, and remove it. This prevents conflicting queue accesses and preserves FIFO order. Its wait also releases the mutex.
- `close`: set the closed flag under that buffer's mutex, then notify consumers. This synchronizes completion with consumer predicate checks.
- Error reporting: an independent mutex protects complete stderr diagnostic writes so workers do not interleave messages. An atomic boolean records whether any error occurred without a data race.

File reads, metadata inspection, matching, and output occur outside buffer mutexes. A pop releases its mutex before the worker pushes to the next buffer; no worker holds one queue lock while waiting on another. Stage 5's count is local to its single worker and requires no mutex.

### Completion and errors

Stage 1 closes B1 after enumeration. Each following stage drains its input, then closes its output. A closed buffer still supplies queued records; `pop` returns false only when closed **and empty**. Stage 5 drains B4 and prints its summary once. An empty string or ordinary path is never used as a completion marker. There is exactly one producer for each buffer; no producer pushes after closing its output.

Inspection/open/read errors set the atomic error flag and produce stderr diagnostics. Other files continue; a failed read may leave already-read lines in the output. Workers still close their output normally, and main returns nonzero after joining them. Directory-open/enumeration failures, allocation failures caught by the worker wrapper, and thread-creation failures are fatal: report the failure and exit the process nonzero. This simple fatal path avoids waiting forever on a partially started pipeline; it does not attempt sophisticated resource-failure recovery.

## Local verification

Development checks were run with a temporary Python test driver; it is not included in this minimal source folder. Python is not required to build or run the program. Correctness subprocesses used a 15-second timeout and benchmark subprocesses 30 seconds; these are local hang detectors, not the unspecified evaluator time limit.

Observed locally on November 23, 2026: 230 correctness assertions passed, followed by all nine benchmark equivalence checks. Coverage includes the assignment example, invalid counts/numbers/overflow, empty substring, literal/case-sensitive matching, whitespace, hidden/extensionless files, long lines, missing final newline, size boundaries, UID/GID combinations, skipped directories/symlinks/FIFOs, empty inputs, repeated capacities 1/10/100, and unreadable-file continuation/nonzero exit. Running from temporary directories also checks that input comes from the calling directory.

The Unix-socket fixture was skipped because the sandbox denies creating a socket. Device entries, metadata/read failures, directory-open failure, and thread-creation failure were not injected. Source inspection supports the nonregular-file guard and synchronization design; runtime checks alone do not prove race freedom.

## Performance experiment

Observed local measurements, not university-host results:

- Dataset: 30 generated text files, 567,000 total bytes; 1,000 lines/file, every tenth line containing `error`; 3,000 matching lines total.
- Substring: `error`; size/UID/GID filters all `-1`.
- Environment: macOS 27.2.1, arm64; `g++` resolves to Apple clang 21.0.0 (clang-2100.4.1.1).
- Method: Python `time.perf_counter()` measures wall time around process startup through completion. Stdout goes to a temporary file outside the input directory, stderr is captured separately. Same dataset and settings for every run; no cache flushing. Capacities run in order 1, 10, 100, three times each.

| Capacity | Run 1 (s) | Run 2 (s) | Run 3 (s) | Mean (s) |
| --- | --- | --- | --- | --- |
| 1 | 0.203582 | 0.168805 | 0.161240 | 0.177876 |
| 10 | 0.046397 | 0.044660 | 0.045163 | 0.045407 |
| 100 | 0.036099 | 0.036352 | 0.035881 | 0.036111 |

Every timed output had identical matching records and a final count of 3,000; cross-file order was ignored for equivalence. This small local dataset was faster with larger buffers. Reduced blocking/handoff frequency is a plausible explanation, not an instrumented conclusion. Caching, scheduling, startup, and output costs also affect these short runs. Larger buffers need not help on other datasets or machines, and capacity bounds records rather than bytes.

## Where another worker might help

If measurements on a larger workload show file reading is the bottleneck, another Stage 3 worker could read independent files concurrently. This experiment did not measure individual stage times, so it does not establish that Stage 3 is the bottleneck. Extra readers could overlap I/O, but might contend for storage and fail to improve performance.

With multiple Stage 3 consumers, closed B2 would wake all readers, and each reader would finish only after B2 is empty and closed. B3 must close only after the **last** reader has finished pushing, using a synchronized remaining-producer count. A single reader should retain ownership of each file so its lines stay ordered. A completion-token design would need enough tokens for all input consumers. No extra workers are implemented; the submitted design retains exactly five.

## Known limitations and remaining submission work

No functional bugs were observed in the exercised cases. University-host execution remains unverified, as do the fault paths listed above. Inputs follow the assignment's unchanged ordinary-text and filename assumptions. Fatal resource errors terminate the process rather than recovering.

Fill in the team number and both names before submission. Verify a fresh extracted source build on an allowed university host, rehearse both members' demonstrations, and prepare the identical ZIP for both members. No demonstration appointment or Canvas submission has been verified.

## Assignment review and submission status

Reviewed against the supplied `Project_3.pdf`, pages 1-5. Source review confirms five workers, four bounded FIFOs, predicate waits, small buffer critical sections, and completion propagation. Local tests support the interface, search/filter behavior, formatting, and tested error paths. A fresh copy containing only the source, Makefile, and README also built successfully, passed the capacity-1 example, and preserved source/test input on `make clean`. This is a local source-copy check, not a university-host or final ZIP check.

The PDF additionally states on page 1 that generating substantial portions of code or complete solutions with AI is not permitted. This implementation was generated with AI assistance and therefore cannot be represented as compliant with that rule. Resolve this with the instructor before submitting it; technical test results do not resolve authorship requirements. Page 4 prohibits AI, online assistance, and communication with other students during the demonstration.

Remaining items: actual team number/names, required university-host run, final ZIP verification and identical submissions, and both members' independent demonstration preparation. Demonstration signup is due November 29, 2026, at 11:59 p.m. Central Time; submission is due November 30, 2026, at 11:59 p.m. Central Time. Each member submits the same ZIP, using the assignment's team-and-member naming pattern, with one project folder containing all source/header files, Makefile, and complete README. No final ZIP or submission has been created or verified here.
