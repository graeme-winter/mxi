"""scripts/check_arm64_portable.py: an instruction ARMv8.0 lacks is found in
the function it is in, and not where libgcc's outline atomics choose at run
time -- 1.3.2's arm64 build, mimalloc made for ARMv8.1, died on a Pi 4."""

import importlib.util
import pathlib

SCRIPT = pathlib.Path(__file__).parents[2] / "scripts" / "check_arm64_portable.py"
spec = importlib.util.spec_from_file_location("check_arm64_portable", SCRIPT)
check = importlib.util.module_from_spec(spec)
spec.loader.exec_module(check)

DISASSEMBLY = """
probe:     file format elf64-littleaarch64

Disassembly of section .text:

0000000000400100 <__aarch64_ldadd8_acq_rel>:
  400100:	ldaddal	x16, x0, [x1]
  400104:	ret

0000000000400200 <mi_stats_merge_from.part.0>:
  400200:	ldadd	x3, x3, [x4]
  400204:	casal	x1, x2, [x0]
  400208:	add	x0, x0, #0x1

0000000000400300 <dot>:
  400300:	sdot	v0.4s, v1.16b, v2.16b
  400304:	ld1w	{z0.s}, p0/z, [x0]
  400308:	ldaxr	x1, [x0]
"""


def test_each_finding_in_its_function_and_none_in_the_helpers():
    found = list(check.findings(DISASSEMBLY))
    assert [(f, k) for f, k, _ in found] == [
        ("mi_stats_merge_from.part.0", "LSE atomic"),
        ("mi_stats_merge_from.part.0", "LSE atomic"),
        ("dot", "dot product"),
        ("dot", "SVE"),
    ]


def test_armv8_0_alone_passes():
    clean = """
0000000000400300 <f>:
  400300:	ldaxr	x1, [x0]
  400304:	stlxr	w2, x1, [x0]
  400308:	add	v0.4s, v1.4s, v2.4s
"""
    assert list(check.findings(clean)) == []
