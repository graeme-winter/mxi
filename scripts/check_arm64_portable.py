#!/usr/bin/env python3
"""Fail if an arm64 binary uses instructions an ARMv8.0 CPU lacks.

    python3 scripts/check_arm64_portable.py build/mxi [--objdump OBJDUMP]

1.3.2's Linux arm64 build died with SIGILL on a Raspberry Pi 4 (Cortex-A72,
ARMv8.0) before printing anything, `mxi --version` included: mimalloc's own
build compiles it with -march=armv8.1-a on arm64 whatever MI_OPT_ARCH says,
so every malloc ran LSE atomics (ldaddal). Graeme, 16 November.

What it looks for, outside the functions where it is allowed:

- LSE atomics (ARMv8.1: cas, ldadd, ldset, ldclr, ldeor, swp, ...), except
  in libgcc's __aarch64_* helpers: GCC's -moutline-atomics, its default,
  calls those, and they choose LSE or the ARMv8.0 sequence at run time --
  a portable binary has LSE in them, and should;
- the dot product (sdot, udot: ARMv8.2's, optional);
- SVE (any z register).

Exit 0 with nothing found; 1 with each finding, by function, and the
counts; 2 if it cannot run.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from collections import Counter

LSE = re.compile(
    r"^(cas|casp|ldadd|ldclr|ldeor|ldset|ldsmax|ldsmin|ldumax|ldumin|"
    r"stadd|stclr|steor|stset|stsmax|stsmin|stumax|stumin|swp)"
    r"(a|al|l)?(b|h)?$"
)
DOT = re.compile(r"^(sdot|udot|usdot|sudot)$")
SVE = re.compile(r"\bz\d+\.[bhsdq]\b")
ALLOWED = re.compile(r"^__aarch64_")  # libgcc's outline atomics
FUNCTION = re.compile(r"^[0-9a-f]+ <(.+)>:$")
INSTRUCTION = re.compile(r"^\s*[0-9a-f]+:\s+(?:[0-9a-f]{8}\s+)?(\S+)\s*(.*)$")


def findings(disassembly: str):
    """(function, kind, instruction) for each disallowed instruction."""
    function = ""
    for line in disassembly.splitlines():
        m = FUNCTION.match(line)
        if m:
            function = m.group(1)
            continue
        m = INSTRUCTION.match(line)
        if not m:
            continue
        op, operands = m.group(1), m.group(2)
        kind = None
        if LSE.match(op):
            kind = "LSE atomic"
        elif DOT.match(op):
            kind = "dot product"
        elif SVE.search(operands):
            kind = "SVE"
        if kind and not ALLOWED.match(function):
            yield function, kind, f"{op} {operands}".strip()


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("binary")
    p.add_argument("--objdump", default="objdump")
    args = p.parse_args(argv)
    try:
        run = subprocess.run(
            [args.objdump, "-d", "--no-show-raw-insn", args.binary],
            capture_output=True,
            text=True,
        )
    except FileNotFoundError:
        print(f"no {args.objdump} to disassemble with", file=sys.stderr)
        return 2
    if run.returncode != 0 or "aarch64" not in run.stdout[:2000]:
        print(
            f"{args.objdump} could not disassemble {args.binary} as arm64: "
            f"{run.stderr.strip()[:200]}",
            file=sys.stderr,
        )
        return 2
    found = list(findings(run.stdout))
    if not found:
        print(f"{args.binary}: ARMv8.0 only, outside libgcc's outline atomics")
        return 0
    by_function = Counter((f, k) for f, k, _ in found)
    print(f"{args.binary}: {len(found)} instructions an ARMv8.0 CPU lacks")
    for (function, kind), n in by_function.most_common(20):
        example = next(i for f, k, i in found if f == function and k == kind)
        print(f"  {n:6d}  {kind:12s} in {function}: {example}")
    if len(by_function) > 20:
        print(f"  ... and {len(by_function) - 20} more functions")
    return 1


if __name__ == "__main__":
    sys.exit(main())
