# trim

A fast, streaming `trim()` for the Linux command line, written in C.
It strips whitespace (or a chosen set of bytes) from the ends of each line,
reading from files or pipes in fixed-size chunks, so memory use stays flat
no matter how large the input is.

## Build

```bash
make              # -O3 -march=native -Wall -Wextra ... (fastest, host-specific)
make portable     # -O3 without -march=native (safe to distribute)
make test         # fuzz the normal build and a tiny-buffer ASan/UBSan build
sudo make install # installs to /usr/local/bin (override with PREFIX=...)
```

Or compile by hand:

```bash
cc -O3 -march=native -Wall -Wextra -std=gnu11 -o trim trim.c
```

`-march=native` can produce binaries that crash with an illegal-instruction
error on older CPUs. Use `make portable` for anything you ship.

## Usage

```
trim [OPTION]... [FILE]...
```

With no `FILE`, or when `FILE` is `-`, input is read from standard input.
Output always goes to standard output.

| Option | Meaning |
|---|---|
| `-l`, `--left` | Trim leading bytes only (`trimStart`). |
| `-r`, `--right` | Trim trailing bytes only (`trimEnd`). |
| `-c SET`, `--chars=SET` | Trim the bytes in `SET` instead of whitespace. |
| `-s`, `--squeeze` | Squeeze runs of the same target byte to one, like `tr -s`, in addition to trimming. |
| `-z`, `--stream` | Treat all input as one string. Only the very start and end are trimmed; newline becomes an ordinary character. |
| `-M N`, `--max-runs=N` | Cap on held-back trailing runs (default 1048576). See [Memory](#memory). |
| `-h`, `--help` | Show help. |
| `-V`, `--version` | Show version. |

`-l` and `-r` together, or neither, trim both ends.

## Semantics

- **Per-line by default.** Each line is trimmed independently. The `\n`
  terminator is a delimiter: it is never trimmed and is always preserved.
  A last line with no trailing newline stays without one.
- **Default trim set:** ASCII space, `\t`, `\r`, `\v`, `\f` (plus `\n` with `-z`).
  Matching is byte-based and locale-independent. Unicode spaces such as NBSP
  are not special; pass their UTF-8 bytes with `-c` if you need them.
- **CRLF:** because `\r` is whitespace, CRLF input becomes LF in the default
  mode. Use `-c ' \t'` to trim blanks and leave `\r` alone.
- **`-c SET`:** bytes, not characters. Escapes: `\t \n \r \v \f \\ \xHH`.
  An empty set is an error. A `\n` in `SET` is ignored in per-line mode
  (newline is the delimiter there) and honored with `-z`.
- **`-s`:** collapses repeated *identical* target bytes everywhere in the
  kept text. `"a   b"` becomes `"a b"`, but `"a \t b"` keeps its mixed run.
  With `-l` or `-r`, interior squeezing still applies.
- **Multiple files:** processed in order. In per-line mode, if a file ends
  without a newline and another file follows, a newline is inserted between
  them. With `-z` the files form one continuous stream.
- **Binary safe:** NUL bytes are handled like any other byte.

### Exit status

| Code | Meaning |
|---|---|
| 0 | Success. |
| 1 | An input file could not be read. The remaining files are still processed. |
| 2 | Usage error (bad option, empty or malformed `-c`, bad `-M`). |
| 3 | Fatal: write error, out of memory, or the `--max-runs` limit was hit. |

If the reader closes the pipe (for example `trim ... | head`), the process
ends quietly through SIGPIPE, as is conventional for filters.

## Examples

```bash
# pipes
cat huge.log | trim > clean.log

# files (multiple files and "-" for stdin are fine; a missing file is
# reported, the rest continue, and the exit status is 1)
trim a.txt b.txt - > out.txt

# one side only
trim -l file            # leading only
trim -r file            # trailing only

# custom character sets
trim -c 'xe' file                 # "xxhixbyex" -> "hixby"
trim -c ' \t' crlf.txt            # trim blanks, keep \r
trim -c '\x2d\x5f' file           # trim '-' and '_'
trim -c '\\' file                 # trim backslashes

# squeeze
trim -s file                      # "a   b" -> "a b", plus end-trimming
trim -s -c ' ' file               # only squeeze/trim spaces

# whole-stream mode
printf '\n\n  a\n\n b \n\n' | trim -z     # -> "a\n\n b"

# raise the held-back-run cap for unusual whitespace-heavy input
trim -r -M 8000000 file
```

## Design

### I/O

- Plain `read(2)` and `write(2)` with two page-aligned buffers
  (128 KiB each, set by `BUF_SIZE` / `OBUF_SIZE` at compile time).
- `EINTR` is retried and partial writes are looped.
- When stdin or stdout is a pipe, the kernel pipe buffer is raised to 1 MiB
  (best effort). Regular files get `POSIX_FADV_SEQUENTIAL`.

### Two processing paths

- **Fast path (no `-s`).** For each line, `memchr` finds the end, a short
  backward scan finds the trailing run, and the interior is one `memcpy`
  without being inspected. A line that spans chunks emits its kept text
  immediately and holds back only its trailing candidates.
- **Slow path (`-s`).** A byte-level state machine that removes duplicates as
  it goes. Correct for every flag combination, but several times slower.

### Memory

To trim the end of a line, the program must hold back a run of trim bytes
until it knows whether more text follows. That only matters when a run
touches the end of a read chunk; runs fully inside a chunk are decided in
place. Held-back bytes are **run-length encoded**, so a 5 GB line of spaces
costs a single table entry.

Only pathological alternating input (`" \t \t \t..."`) grows the table. It
is capped at `--max-runs` entries (default 1M, about 16 MiB). If the cap is
exceeded, `trim` exits with status 3 and an error message instead of growing
without bound.

## Performance

Measured in a Linux sandbox on a 1 GB whitespace-dense text file. Absolute
numbers will differ on your hardware; the ratios are the useful part.

| Command | Throughput |
|---|---|
| `trim file` | about 1.3 GB/s |
| `trim -l` / `trim -r` | about 1.8 to 1.9 GB/s |
| `trim -c abc` | about 2.5 GB/s |
| `cat file \| trim` | about 1 GB/s (pipe-limited) |
| `trim -s` | about 250 MB/s |
| `sed 's/^[ \t]*//;s/[ \t]*$//'` | about 34 MB/s |
| `awk '{gsub(/^[ \t]+\|[ \t]+$/,"")}1'` | about 40 MB/s |
| `cat` (baseline) | about 6.8 GB/s |

Peak memory with a 1 GB single line of spaces: about 13 MB.

## Testing

`tests/fuzz.py` compares the binary against a small reference model on
thousands of random inputs and flag combinations (`-l`, `-r`, `-s`, `-z`,
several `-c` sets, NUL bytes, CR/LF/VT/FF). `make test` runs it against both
the normal build and a build with 5-byte buffers under AddressSanitizer and
UBSanitizer, which forces every chunk-boundary path to be exercised.

```bash
make test
# 8000 runs, 0 failures
```

## Limitations

- Linux/POSIX only.
- Byte-based: there is no UTF-8 awareness and no locale support.
- `-s` is slower than the other modes.
- No ranges or character classes in `-c` (use an explicit set).
- Output goes only to standard output; use shell redirection to write a file.

## Files

```
trim.c          the implementation
Makefile        build, portable build, test, install
tests/fuzz.py   differential fuzzer
README.md       this file
```
