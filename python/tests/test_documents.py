"""The documents name only things that exist.

A path, a program or a subcommand copied into prose goes stale the first time
the thing it names moves, and nothing checks it. This is the check. It was
written after a review found `CLAUDE.md` pointing at six paths that no longer
existed, a README calling integration "not started" long after it worked, and a
generator named `tests/make_real_data.py` that had been moved -- unchanged, git
said -- into the Python package when the Python was consolidated.

A path in backticks is resolved against the repository root and against the
naming document's own directory, since `python/README.md` names files relative
to `python/`. Paths under `dials/` are DIALS' own tree. A path mentioned only as
history is written without backticks, so it reads as prose and is not checked.
"""

import pathlib
import re
import shutil

import pytest

ROOT = pathlib.Path(__file__).resolve().parents[2]
DOCUMENTS = [
    "README.md",
    "CLAUDE.md",
    "python/README.md",
    "python/CLAUDE.md",
    "docs/integration.md",
    "docs/integration_history.md",
    "docs/gpu.md",
    "docs/scaling.md",
    "docs/symmetry.md",
    "docs/outstanding.md",
    "docs/review.md",
    "docs/README.md",
    "docs/conventions.md",
    "docs/spots.md",
    "docs/spots_notes.md",
    "docs/tutorials/cows-pigs-people.md",
    "RELEASE_NOTES.md",
    "docs/mask.md",
]
PATH = re.compile(
    r"`([A-Za-z0-9_./-]+/[A-Za-z0-9_.-]+\.(?:md|cc|h|hh|py|cmake|txt|json))`"
)
PROGRAM = re.compile(r"`(mxi_[a-z]+)`")


def _python_modules():
    """The wheel's own modules and packages, mxi's: named in the
    documents as Python, not as programs -- mxi_tools, mxi_view, mxi_ui."""
    text = (ROOT / "python/pyproject.toml").read_text()
    found = re.search(r"py-modules\s*=\s*\[([^\]]*)\]", text)
    modules = set(re.findall(r'"([a-z_]+)"', found.group(1))) if found else set()
    # And its packages, which setuptools finds itself: mxi_ui, the interface.
    packages = {p.parent.name for p in (ROOT / "python/src").glob("mxi_*/__init__.py")}
    return modules | packages


PYTHON_MODULES = _python_modules()


def _programs():
    """Every mxi_ program the build defines, read from the build files."""
    names = set()
    for f in [ROOT / "CMakeLists.txt", *(ROOT / "cmake").glob("*.cmake")]:
        text = f.read_text()
        names |= set(re.findall(r"add_executable\(\s*(mxi_[a-z]+)", text))
        # The programs that are subcommands of mxi now: the list mxi links.
        listed = re.search(r"set\(MXI_PROGRAMS\s+([^)]*)\)", text)
        if listed:
            names |= set(listed.group(1).split())
        # And a script copied beside the programs as one: mxi_mask, in Python.
        names |= set(re.findall(r"configure_file\([^)]*?/(mxi_[a-z]+)[\s)]", text))
    return names


@pytest.mark.parametrize("document", DOCUMENTS)
def test_every_path_named_exists(document):
    doc = ROOT / document
    text = doc.read_text()
    missing = []
    for m in PATH.finditer(text):
        path = m.group(1)
        if path.startswith("dials/"):
            continue
        if not ((ROOT / path).exists() or (doc.parent / path).exists()):
            missing.append(path)
    assert not missing, f"{document} names paths that do not exist: {missing}"


LINK = re.compile(r"\]\(([^)\s#]+)(?:#[^)]*)?\)")


@pytest.mark.parametrize("document", DOCUMENTS)
def test_every_link_leads_somewhere(document):
    """Each relative link's target exists, from the document's own directory:
    as GitHub follows them (Graeme: clicking a link on GitHub should work)."""
    doc = ROOT / document
    broken = [
        target
        for target in LINK.findall(doc.read_text())
        if not target.startswith(("http://", "https://", "mailto:"))
        and not (doc.parent / target).exists()
    ]
    assert not broken, f"{document} links to paths that do not exist: {broken}"


@pytest.mark.parametrize("document", DOCUMENTS)
def test_every_program_named_is_built(document):
    built = _programs()
    assert built, "found no mxi_ programs in the build files"
    named = set(PROGRAM.findall((ROOT / document).read_text()))
    # Names of mxi's that are not programs: mxi_physical, the scale model's type.
    unknown = named - built - {"mxi_physical"} - PYTHON_MODULES
    # Programs retired, which only the histories may name: the release notes,
    # the outstanding list, CLAUDE.md and the integration's history. Anywhere
    # else a mention of one is stale.
    retired = {"mxi_mask"}  # the matplotlib mask editor, now mxi_view's
    if document in (
        "RELEASE_NOTES.md",
        "docs/outstanding.md",
        "CLAUDE.md",
        "docs/integration_history.md",
    ):
        unknown -= retired
    assert not unknown, f"{document} names programs the build does not make: {unknown}"


def test_every_mxeq_subcommand_named_exists():
    from mxeq.cli import build_parser

    parser = build_parser()
    commands = set()
    for action in parser._subparsers._group_actions:
        commands |= set(action.choices)
    assert commands, "mxeq has no subcommands?"
    # mxeq regress has a parser of its own, handed the rest of the line before
    # mxeq's (cli.main).
    commands.add("regress")
    for document in DOCUMENTS:
        text = (ROOT / document).read_text()
        for m in re.finditer(r"`mxeq ([a-z][a-z-]*)", text):
            assert m.group(1) in commands, (document, m.group(1), sorted(commands))


# The commands mxi has (apps/mxi.cc's table): mxi find, mxi view ...
def _commands():
    text = (ROOT / "apps/mxi.cc").read_text()
    return set(re.findall(r'^\s*\{"([a-z]+)", ', text, re.M))


# The histories, which name programs as they were: mxi_find before 1.1.
HISTORIES = {"docs/integration_history.md", "CLAUDE.md", "RELEASE_NOTES.md"}

# An old program name, mxi_find, not a file's (mxi_find.log, mxi_view.py,
# apps/mxi_find.cc) nor a step's as files record it ("mxi_find").
OLD_NAME = re.compile(r'(?<![/"\w])mxi_([a-z]+)\b(?![.\w"])')


@pytest.mark.parametrize("document", DOCUMENTS)
def test_programs_are_named_as_mxi_commands(document):
    """Since 1.1 every program is a command of one, mxi find: the old names
    belong to the histories alone, and every mxi command named is one of mxi's
    (Graeme, 28 October)."""
    text = (ROOT / document).read_text()
    commands = _commands()
    assert "find" in commands and "view" in commands
    named = set(re.findall(r"`(?:\./build/)?mxi ([a-z]+)\b", text))
    unknown = named - commands - {"help"}
    assert not unknown, f"{document} names commands mxi does not have: {unknown}"
    if document in HISTORIES:
        return
    if document == "RELEASE_NOTES.md":
        return
    # A Python module of the wheel's is named as itself: mxi_view the module,
    # not the old name of mxi view the command.
    stale = sorted(
        {
            m
            for m in OLD_NAME.findall(text)
            if m in commands and f"mxi_{m}" not in PYTHON_MODULES
        }
    )
    assert not stale, f"{document} still names programs the old way: mxi_{stale}"


# Every Markdown file of the repository's own, for the wrapping test: not
# only DOCUMENTS, which the other checks were built on -- correlation.md and
# multi-lattice.md had wrapped lines too and are in neither.
ALL_MARKDOWN = sorted(
    str(f.relative_to(ROOT))
    for f in ROOT.rglob("*.md")
    if not ({"third_party", ".git", "node_modules", "build"} & set(f.parts))
    and not any(part.startswith(".") for part in f.relative_to(ROOT).parts)
)


@pytest.mark.parametrize("document", ALL_MARKDOWN)
def test_no_wrapped_line_reads_as_a_list_item(document):
    """A line of prose that wrapping starts with "+ ", "- " or "167. " is a
    list item to Markdown: review.md's "I_bg (1 / + m/n)" rendered as a
    bullet in mid-sentence (Graeme, 19 November). A list is allowed after a
    heading, a table, a bold label such as **New**, or a line ending in ":"."""
    marker = re.compile(r"^(\+|-|\*|\d+[.)])\s")
    allowed_before = re.compile(
        r"^\s*(\+|-|\*|\d+[.)])\s|^\s{2,}|^#|^\||^\*\*.*\*\*$|:$"
    )
    lines = (ROOT / document).read_text().splitlines()
    fence, found = False, []
    for i, line in enumerate(lines):
        if line.lstrip().startswith("```"):
            fence = not fence
            continue
        if fence or i == 0 or not marker.match(line):
            continue
        before = lines[i - 1]
        if before.strip() and not allowed_before.search(before):
            found.append(f"{document}:{i + 1}: {line[:60]}")
    assert not found, "\n".join(found)
