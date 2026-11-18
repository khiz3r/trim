#!/usr/bin/env python3
"""Differential fuzzer: compares trim binaries against a tiny reference model.

usage: fuzz.py BIN [BIN...]
Run it against a normal build AND the tiny-buffer sanitizer build
(`make test` does both) so every chunk-boundary path is exercised.
"""
import random
import subprocess
import sys

WS = b" \t\r\x0b\x0c"
ALPHABET = b"abc  \t\t\r\n\n\x0b\x00"
CHAR_SETS = [  # (argv string, decoded bytes)
    (None, None),
    ("ab", b"ab"),
    (" ", b" "),
    (" \\t", b" \t"),
    ("\\n ", b"\n "),
    ("\\x61\\x00", b"a\x00"),
    ("c\\\\", b"c\\"),
]


def reference(data, chars, l, r, sq, stream):
    if chars is None:
        S = set(WS) | ({10} if stream else set())
    else:
        S = set(chars) - ({10} if not stream else set())

    def proc(b):
        i, j = 0, len(b)
        if l:
            while i < j and b[i] in S:
                i += 1
        if r:
            while j > i and b[j - 1] in S:
                j -= 1
        t = b[i:j]
        if not sq:
            return t
        out = bytearray()
        prev = None
        for c in t:
            if c in S and c == prev:
                continue
            out.append(c)
            prev = c
        return bytes(out)

    if stream:
        return proc(data)
    return b"\n".join(proc(x) for x in data.split(b"\n"))


def main():
    bins = sys.argv[1:]
    if not bins:
        sys.exit(__doc__)
    rng = random.Random(1234)
    fails = 0
    cases = 0
    for it in range(4000):
        n = rng.choice([0, 1, 2, 5, 12, 40, 120])
        data = bytes(rng.choice(ALPHABET) for _ in range(n))
        argstr, chars = rng.choice(CHAR_SETS)
        side = rng.choice([(), ("-l",), ("-r",), ("-l", "-r")])
        sq = rng.random() < 0.4
        stream = rng.random() < 0.3
        args = list(side)
        if sq:
            args.append("-s")
        if stream:
            args.append("-z")
        if argstr is not None:
            args += ["-c", argstr]
        l = r = True
        if side == ("-l",):
            r = False
        elif side == ("-r",):
            l = False
        want = reference(data, chars, l, r, sq, stream)
        for b in bins:
            cases += 1
            p = subprocess.run([b] + args, input=data, capture_output=True)
            if p.returncode != 0 or p.stdout != want:
                fails += 1
                if fails <= 10:
                    print("FAIL", b, args, repr(data))
                    print("  want", repr(want))
                    print("  got ", repr(p.stdout), "rc", p.returncode, p.stderr[:200])
    print(f"{cases} runs, {fails} failures")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
