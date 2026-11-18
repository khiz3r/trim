/*
 * trim - fast streaming trim() for the Linux command line.
 *
 * SEMANTICS (the decisions the spec left open)
 * --------------------------------------------
 *  - Default mode is PER-LINE: every line is trimmed independently.  The '\n'
 *    terminator is a delimiter, never trimmed, and is preserved.  A final
 *    line without '\n' stays without one.
 *  - -z/--stream is WHOLE-STREAM mode: the whole input (all files
 *    concatenated) is one string; only its very start and very end are
 *    trimmed, and '\n' is an ordinary character (whitespace by default).
 *  - Default trim set: ASCII space, \t, \r, \v, \f (plus \n in stream mode).
 *    Locale-independent, byte-based.  Unicode spaces (NBSP etc.) are NOT
 *    special; pass their UTF-8 bytes via -c if you need them.
 *    Note: '\r' is whitespace, so CRLF input becomes LF in per-line mode.
 *    Use -c ' \t' to leave CRs alone.
 *  - -l / -r select which end(s) to trim; both or neither means both.
 *  - -c SET replaces the default set.  Bytes, not characters.  Escapes:
 *    \t \n \r \v \f \\ \xHH.  Empty SET is an error.  A '\n' in SET is
 *    ignored in per-line mode (newline is the delimiter there).
 *  - -s squeezes runs of the SAME target byte to one (like `tr -s SET`),
 *    everywhere in the kept text, in addition to trimming.  "a  \t b" is
 *    unchanged apart from the doubled space.
 *  - Binary safe (NUL bytes are fine).
 *
 * MEMORY BOUND
 * ------------
 * Trailing-trim must hold back a run of trim bytes until it knows whether
 * more text follows.  That is only needed when a run touches the end of a
 * read chunk; runs fully inside a chunk are decided in place.  Held-back
 * bytes are stored run-length encoded, so a 5 GB line of spaces costs one
 * entry.  Only pathological alternating runs (" \t \t \t ...") grow the
 * table; it is capped at --max-runs entries (default 1M = 16 MiB) and the
 * program exits with status 3 instead of growing without bound.
 *
 * EXIT STATUS: 0 ok, 1 an input file could not be read (others continue),
 *              2 usage error, 3 fatal (write error, out of memory, limit).
 *
 * Build: see Makefile.   Linux/POSIX only.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef BUF_SIZE
#define BUF_SIZE (128u * 1024u) /* input chunk; page multiple */
#endif
#ifndef OBUF_SIZE
#define OBUF_SIZE BUF_SIZE /* output buffer */
#endif
#define DEFAULT_MAX_RUNS ((size_t)1 << 20)
#define PIPE_SIZE (1 << 20)
#define PROG "trim"
#define VERSION "1.0"

#if defined(__GNUC__) || defined(__clang__)
#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define LIKELY(x) (x)
#define UNLIKELY(x) (x)
#endif

/* ------------------------------------------------------------------------ */
/* Configuration (immutable after option parsing)                           */
/* ------------------------------------------------------------------------ */

static struct {
    bool lstrip, rstrip, squeeze, stream;
    unsigned char set[256];  /* bytes that are trimmed / squeezed          */
    unsigned char stop[256]; /* bytes that end a scan of ordinary text:
                                set, plus '\n' in per-line mode            */
    size_t max_runs;
} cfg;

/* ------------------------------------------------------------------------ */
/* Runtime state                                                            */
/* ------------------------------------------------------------------------ */

typedef struct {
    uint64_t n;
    unsigned char c;
} run_t;

static struct {
    unsigned char *ibuf, *obuf;
    size_t olen;
    bool leading;   /* still skipping leading trim bytes of this line      */
    int prev;       /* last accepted byte of this line, -1 at line start    */
    run_t *pend;    /* held-back trailing candidates, run-length encoded    */
    size_t npend, cap_pend;
    int last_out;   /* last byte handed to the output layer, -1 = none      */
} st;

/* ------------------------------------------------------------------------ */
/* Errors and output                                                        */
/* ------------------------------------------------------------------------ */

static void die(const char *msg)
{
    fprintf(stderr, PROG ": %s\n", msg);
    exit(3);
}

static void die_errno(const char *what)
{
    fprintf(stderr, PROG ": %s: %s\n", what, strerror(errno));
    exit(3);
}

static void write_all(const unsigned char *p, size_t n)
{
    while (n) {
        ssize_t w = write(STDOUT_FILENO, p, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EPIPE) /* reader went away (e.g. `| head`) */
                _exit(3);
            die_errno("write error");
        }
        p += (size_t)w;
        n -= (size_t)w;
    }
}

static void flush_out(void)
{
    if (st.olen) {
        write_all(st.obuf, st.olen);
        st.olen = 0;
    }
}

static inline void emit_byte(unsigned char c)
{
    if (UNLIKELY(st.olen == OBUF_SIZE))
        flush_out();
    st.obuf[st.olen++] = c;
    st.last_out = c;
}

static inline void emit(const unsigned char *p, size_t n)
{
    if (n == 0)
        return;
    st.last_out = p[n - 1];
    if (UNLIKELY(n >= OBUF_SIZE)) { /* big: bypass the copy */
        flush_out();
        write_all(p, n);
        return;
    }
    if (n > OBUF_SIZE - st.olen)
        flush_out();
    memcpy(st.obuf + st.olen, p, n);
    st.olen += n;
}

static void emit_fill(unsigned char c, uint64_t n)
{
    st.last_out = c;
    while (n) {
        size_t room = OBUF_SIZE - st.olen;
        if (room == 0) {
            flush_out();
            room = OBUF_SIZE;
        }
        size_t k = n < room ? (size_t)n : room;
        memset(st.obuf + st.olen, c, k);
        st.olen += k;
        n -= k;
    }
}

/* ------------------------------------------------------------------------ */
/* Per-line state helpers                                                   */
/* ------------------------------------------------------------------------ */

static inline void line_reset(void)
{
    st.leading = cfg.lstrip;
    st.prev = -1;
    st.npend = 0;
}

static void pend_push(unsigned char c, uint64_t n)
{
    if (st.npend == st.cap_pend) {
        if (st.cap_pend >= cfg.max_runs)
            die("trailing run too fragmented (raise --max-runs)");
        size_t ncap = st.cap_pend ? st.cap_pend * 2 : 256;
        if (ncap > cfg.max_runs)
            ncap = cfg.max_runs;
        run_t *np = realloc(st.pend, ncap * sizeof *np);
        if (!np)
            die("out of memory");
        st.pend = np;
        st.cap_pend = ncap;
    }
    st.pend[st.npend].c = c;
    st.pend[st.npend].n = n;
    st.npend++;
}

/* Hold back p[0..q) (all trim bytes) until we learn what follows. */
static void pend_add(const unsigned char *p, const unsigned char *q)
{
    int prev = st.prev;
    while (p < q) {
        unsigned char c = *p;
        size_t k = 1;
        while (p + k < q && p[k] == c)
            k++;
        if (cfg.squeeze) {
            if (c != prev) {
                pend_push(c, 1);
                prev = c;
            }
        } else {
            if (st.npend && st.pend[st.npend - 1].c == c)
                st.pend[st.npend - 1].n += k;
            else
                pend_push(c, k);
            prev = c;
        }
        p += k;
    }
    st.prev = prev;
}

/* Text followed the held-back bytes, so they were interior: release them. */
static void flush_pending(void)
{
    for (size_t i = 0; i < st.npend; i++)
        emit_fill(st.pend[i].c, st.pend[i].n);
    st.npend = 0;
}

/* Emit a run of trim bytes that is known to be kept. */
static void emit_run(const unsigned char *p, const unsigned char *q)
{
    if (!cfg.squeeze) {
        emit(p, (size_t)(q - p));
        st.prev = q[-1];
        return;
    }
    int prev = st.prev;
    while (p < q) {
        unsigned char c = *p;
        if (c != prev) {
            emit_byte(c);
            prev = c;
        }
        do
            p++;
        while (p < q && *p == c);
    }
    st.prev = prev;
}

/* ------------------------------------------------------------------------ */
/* The core: one chunk of input                                             */
/* ------------------------------------------------------------------------ */

/*
 * Fast path, used when NOT squeezing.  The interior of a line never needs to
 * be inspected: memchr finds the end of the line, a short backward scan finds
 * the trailing run, and everything in between is one memcpy.  A line that
 * continues into the next chunk emits its kept text now and holds back only
 * its (run-length encoded) trailing candidates.
 */
static void process_chunk_plain(const unsigned char *p, const unsigned char *end)
{
    while (p < end) {
        if (st.leading) { /* leading trim (-l) */
            while (p < end && cfg.set[*p])
                p++;
            if (p == end)
                return;
            if (!cfg.stream && *p == '\n') {
                emit_byte('\n');
                p++;
                line_reset();
                continue;
            }
            st.leading = false;
        }

        /* [p, e) is the rest of this line within the chunk */
        const unsigned char *e = end;
        bool complete = false;
        if (!cfg.stream) {
            const unsigned char *nl = memchr(p, '\n', (size_t)(end - p));
            if (nl) {
                e = nl;
                complete = true;
            }
        }

        /* [p, t) is kept text, [t, e) trailing candidates */
        const unsigned char *t = e;
        if (cfg.rstrip)
            while (t > p && cfg.set[t[-1]])
                t--;

        if (t > p) {
            if (st.npend)
                flush_pending(); /* text follows them: they were interior */
            emit(p, (size_t)(t - p));
        }

        if (complete) {
            st.npend = 0; /* trailing run (and any held-back part) dropped */
            emit_byte('\n');
            p = e + 1;
            line_reset();
        } else {
            if (t < e)
                pend_add(t, e); /* undecided until more input arrives */
            return;
        }
    }
}

/* General byte-level machine; required for -s (needs per-byte dedup). */
static void process_chunk_slow(const unsigned char *p, const unsigned char *end)
{
    const unsigned char *q;

    while (p < end) {
        /* 1. leading trim (only when -l is active) */
        if (st.leading) {
            while (p < end && cfg.set[*p])
                p++;
            if (p == end)
                return;
            if (!cfg.stream && *p == '\n') { /* line was all trimmable */
                emit_byte('\n');
                p++;
                line_reset();
                continue;
            }
            st.leading = false;
        }

        /* 2. ordinary text: the hot loop, one table lookup per byte */
        q = p;
        while (q < end && !cfg.stop[*q])
            q++;
        if (q > p) {
            if (st.npend)
                flush_pending();
            emit(p, (size_t)(q - p));
            st.prev = q[-1];
            p = q;
            if (p == end)
                return;
        }

        /* 3. end of line */
        if (!cfg.stream && *p == '\n') {
            st.npend = 0; /* held-back bytes were trailing: drop them */
            emit_byte('\n');
            p++;
            line_reset();
            continue;
        }

        /* 4. a run of trim bytes */
        q = p;
        while (q < end && cfg.set[*q])
            q++;
        if (cfg.rstrip) {
            if (q == end) { /* undecided until the next chunk */
                pend_add(p, q);
                return;
            }
            if (!cfg.stream && *q == '\n') { /* trailing: drop it */
                st.npend = 0;
                p = q;
                continue;
            }
            if (st.npend)
                flush_pending();
        }
        emit_run(p, q);
        p = q;
    }
}

static inline void process_chunk(const unsigned char *p, const unsigned char *end)
{
    if (cfg.squeeze)
        process_chunk_slow(p, end);
    else
        process_chunk_plain(p, end);
}

/* ------------------------------------------------------------------------ */
/* Input                                                                    */
/* ------------------------------------------------------------------------ */

static void grow_pipe(int fd)
{
#ifdef F_SETPIPE_SZ
    struct stat sb;
    if (fstat(fd, &sb) == 0 && S_ISFIFO(sb.st_mode))
        (void)fcntl(fd, F_SETPIPE_SZ, PIPE_SIZE); /* best effort */
#else
    (void)fd;
#endif
}

/* Returns 0 on success, -1 if reading failed (message already printed). */
static int process_fd(int fd, const char *name)
{
    bool first = true;

    for (;;) {
        ssize_t n = read(fd, st.ibuf, BUF_SIZE);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, PROG ": %s: %s\n", name, strerror(errno));
            return -1;
        }
        if (n == 0)
            break;
        if (first) {
            first = false;
            /* Previous file ended mid-line: terminate it, like `cat`+sed. */
            if (!cfg.stream && st.last_out != -1 && st.last_out != '\n')
                emit_byte('\n');
        }
        process_chunk(st.ibuf, st.ibuf + n);
    }
    if (!cfg.stream) { /* EOF ends the current line; nothing is appended */
        st.npend = 0;
        line_reset();
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Options                                                                  */
/* ------------------------------------------------------------------------ */

static void usage(FILE *f)
{
    fputs("Usage: " PROG " [OPTION]... [FILE]...\n"
          "Trim whitespace (or chosen bytes) from the ends of each line.\n"
          "With no FILE, or FILE is -, read standard input.\n\n"
          "  -l, --left          trim leading bytes only\n"
          "  -r, --right         trim trailing bytes only\n"
          "  -c, --chars=SET     trim bytes in SET instead of whitespace\n"
          "                      escapes: \\t \\n \\r \\v \\f \\\\ \\xHH\n"
          "  -s, --squeeze       squeeze runs of the same trim byte to one\n"
          "  -z, --stream        treat all input as ONE string; newline is\n"
          "                      then an ordinary (whitespace) character\n"
          "  -M, --max-runs=N    cap on held-back trailing runs (default 1048576)\n"
          "  -h, --help          show this help\n"
          "  -V, --version       show version\n\n"
          "Default whitespace: space \\t \\r \\v \\f (plus \\n with -z).\n"
          "Exit: 0 ok, 1 unreadable input file, 2 usage, 3 fatal.\n",
          f);
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Parse -c argument into cfg.set.  Returns false on malformed input. */
static bool parse_chars(const char *s)
{
    bool any = false;
    memset(cfg.set, 0, sizeof cfg.set);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '\\') {
            s++;
            switch (*s) {
            case 't': c = '\t'; break;
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 'v': c = '\v'; break;
            case 'f': c = '\f'; break;
            case '\\': c = '\\'; break;
            case 'x': {
                int h = hexval((unsigned char)s[1]);
                int l = h < 0 ? -1 : hexval((unsigned char)s[2]);
                if (l < 0) {
                    fprintf(stderr, PROG ": bad \\x escape in --chars\n");
                    return false;
                }
                c = (unsigned char)(h * 16 + l);
                s += 2;
                break;
            }
            case '\0':
                fprintf(stderr, PROG ": dangling backslash in --chars "
                                "(use \\\\ for a backslash)\n");
                return false;
            default:
                fprintf(stderr, PROG ": unknown escape \\%c in --chars\n", *s);
                return false;
            }
        }
        cfg.set[c] = 1;
        any = true;
    }
    if (!any) {
        fprintf(stderr, PROG ": --chars must not be empty\n");
        return false;
    }
    return true;
}

int main(int argc, char **argv)
{
    static const struct option longopts[] = {
        {"left", no_argument, NULL, 'l'},
        {"right", no_argument, NULL, 'r'},
        {"chars", required_argument, NULL, 'c'},
        {"squeeze", no_argument, NULL, 's'},
        {"stream", no_argument, NULL, 'z'},
        {"max-runs", required_argument, NULL, 'M'},
        {"help", no_argument, NULL, 'h'},
        {"version", no_argument, NULL, 'V'},
        {NULL, 0, NULL, 0}};
    const char *chars = NULL;
    bool want_l = false, want_r = false;
    int opt;

    cfg.max_runs = DEFAULT_MAX_RUNS;

    while ((opt = getopt_long(argc, argv, "lrc:szM:hV", longopts, NULL)) != -1) {
        switch (opt) {
        case 'l': want_l = true; break;
        case 'r': want_r = true; break;
        case 'c': chars = optarg; break;
        case 's': cfg.squeeze = true; break;
        case 'z': cfg.stream = true; break;
        case 'M': {
            char *e;
            errno = 0;
            unsigned long long v = strtoull(optarg, &e, 10);
            if (errno || *optarg == '\0' || *e || v == 0 || v > SIZE_MAX / sizeof(run_t)) {
                fprintf(stderr, PROG ": invalid --max-runs '%s'\n", optarg);
                return 2;
            }
            cfg.max_runs = (size_t)v;
            break;
        }
        case 'h': usage(stdout); return 0;
        case 'V': puts(PROG " " VERSION); return 0;
        default: usage(stderr); return 2;
        }
    }

    /* which ends */
    if (!want_l && !want_r)
        want_l = want_r = true;
    cfg.lstrip = want_l;
    cfg.rstrip = want_r;

    /* trim set */
    if (chars) {
        if (!parse_chars(chars))
            return 2;
    } else {
        memset(cfg.set, 0, sizeof cfg.set);
        cfg.set[' '] = cfg.set['\t'] = cfg.set['\r'] = cfg.set['\v'] = cfg.set['\f'] = 1;
        cfg.set['\n'] = 1; /* removed below in per-line mode */
    }
    if (!cfg.stream)
        cfg.set['\n'] = 0; /* newline is the delimiter, never trimmed */
    memcpy(cfg.stop, cfg.set, sizeof cfg.stop);
    if (!cfg.stream)
        cfg.stop['\n'] = 1;

    /* buffers: page-aligned */
    if (posix_memalign((void **)&st.ibuf, 4096, BUF_SIZE) ||
        posix_memalign((void **)&st.obuf, 4096, OBUF_SIZE))
        die("out of memory");
    st.last_out = -1;
    line_reset();

    grow_pipe(STDIN_FILENO);
    grow_pipe(STDOUT_FILENO);

    int status = 0;
    if (optind >= argc) {
        if (process_fd(STDIN_FILENO, "standard input") < 0)
            status = 1;
    } else {
        for (int i = optind; i < argc; i++) {
            if (strcmp(argv[i], "-") == 0) {
                if (process_fd(STDIN_FILENO, "standard input") < 0)
                    status = 1;
                continue;
            }
            int fd = open(argv[i], O_RDONLY | O_CLOEXEC);
            if (fd < 0) {
                fprintf(stderr, PROG ": %s: %s\n", argv[i], strerror(errno));
                status = 1;
                continue;
            }
#ifdef POSIX_FADV_SEQUENTIAL
            (void)posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif
            if (process_fd(fd, argv[i]) < 0)
                status = 1;
            close(fd);
        }
    }

    /* EOF: anything still held back is trailing and is discarded. */
    flush_out();
    return status;
}
