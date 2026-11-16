# Making a release

A release is made by pushing a tag: GitHub builds it, tests it and publishes
it (`.github/workflows/release.yml`). Nothing is published unless every build
and its tests pass. Actions minutes are free for a public repository.

```sh
# The version in its five places (CMakeLists.txt, vcpkg.json, python/
# pyproject.toml, python/src/mxeq/__init__.py, docs/spots.md's example), and
# RELEASE_NOTES.md's section for it, then:
git tag -a v1.1.1 -m "mxi 1.1.1"
git push origin v1.1.1
```

It can be run by hand too, for a tag pushed already -- to make a release again
after a failure, say -- from the Actions page (release, Run workflow) or
`gh workflow run release.yml -f tag=v1.1.1`. A tag whose release exists must
have it deleted first (`gh release delete v1.1.1`): GitHub keeps one release a
tag.

Push a candidate first -- `v1.1.1-rc1` -- to see the whole of it work: a tag
with `-rc`, `-alpha` or `-beta` makes a pre-release. Its notes are 1.1.1's
section where there is one yet, and otherwise the "Unreleased" section, said
to be a candidate's. The release itself must have its section: without one
nothing is published.

## What it makes

| asset | what |
|---|---|
| `mxi-<version>-source.tar.gz` | the complete source: the repository and its submodules, gemmi and bitshuffle, at the commits the tag pins. GitHub's own source archives leave the submodules out, and without them nothing builds; this is also the source for a conda-forge recipe |
| `mxi-<version>-linux-x86_64.tar.gz` | `mxi`, built in manylinux_2_28 (AlmaLinux 8, glibc 2.28), for the CPU: it runs wherever glibc is 2.28 or later -- RHEL 8 and 9 and their clones, Ubuntu 20.04 on, Debian 11 on -- no CUDA driver needed |
| `mxi-<version>-linux-x86_64-cuda.tar.gz` | `mxi`, built the same way with CUDA 12.8 for Volta to Blackwell (V100 to B200, the RTX 50s) |
| `mxi-<version>-linux-arm64.tar.gz` | `mxi` for ARM64 Linux, built in manylinux_2_28 on GitHub's ARM runner, for the CPU: a Raspberry Pi 4 or 5 with a 64-bit OS, ARM servers |
| `mxi-<version>-linux-arm64-cuda.tar.gz` | the same with CUDA 12.8, NVIDIA's toolkit for ARM servers (sbsa), for Hopper and Blackwell -- the GH200 and GB200 -- with their PTX for later GPUs; not Jetson, whose toolkit is another |
| `mxi-<version>-macos-arm64.tar.gz` | `mxi`, for Apple silicon, with Metal: macOS 13 or later, its static libraries too (an overlay triplet, `cmake/triplets`); any API newer than 13 used without a check fails the build, so that the binaries cannot fail on an older macOS |
| `mxi-<version>-py3-none-manylinux_2_28_x86_64.whl`, `mxi-<version>-py3-none-manylinux_2_28_aarch64.whl`, `mxi-<version>-py3-none-macosx_13_0_arm64.whl` | the platform's wheel: the Python -- mxeq, the referee; `mxi_tools`; `mxi-view`, the viewer; `mxi-ui`, the interface -- and that platform's `mxi`, in the wheel's scripts, so that `pip install` puts it beside them (`scripts/make_wheel.py`); each installed in a fresh environment, and its `mxi` run, before it is released |
| `mxi_cuda-<version>-py3-none-manylinux_2_28_x86_64.whl`, `mxi_cuda-<version>-py3-none-manylinux_2_28_aarch64.whl` | the same with the CUDA builds' `mxi`: a wheel of its own name, `mxi-cuda`, to install instead of `mxi` |
| `mxi-<version>-windows-x86_64.zip`, `mxi-<version>-py3-none-win_amd64.whl` | Windows: `mxi.exe` alone, zipped, and the wheel with it -- built by `.github/workflows/windows.yml`, which the release workflow calls, with clang-cl and vcpkg's static libraries; its C++ tests run, the zip's DLLs checked to be Windows' own, the wheel installed and run (`docs/windows.md`) |

Thirteen files in all: seven archives, the source among them, and six wheels;
the publishing step counts them and publishes nothing short of that. The release
page shows fifteen: GitHub adds its own "Source code" zip and tar.gz to every
release, and they cannot be turned off -- the repository alone, without its
submodules, so they do not build; `mxi-<version>-source.tar.gz` is the one to
use (1.3.0, Graeme).

The x86 builds -- Linux, with and without CUDA, and Windows -- use SSE4.1, for
the rounding instruction, throughout: any x86-64 processor from 2009 on. Before
1.3.1 only the spot finder had it, the flag standing in CMakeLists.txt after the
library was defined; the rest called the C library to round, and with GCC for
fmax, round and sqrt, which made indexing on a user's Xeon eight times slower
than on an M4 Max.

A wheel's version is its tag's, as Python writes versions (PEP 440): `v1.3.0`
gives `1.3.0`, `v1.3.0-rc2` `1.3.0rc2`, `-beta1` `b1`, `-alpha1` `a1` -- so a
candidate's wheels say they are one, as its tag does, and `pip`, `mxi-ui
--version` and `mxi-view --version` say so too (Graeme, 8 November).
`scripts/make_wheel.py` refuses a tag whose release number is not the
source's, so a version left unchanged stops the release rather than
mislabelling it. `mxi` itself says the source's version, `1.3.0`, as before.

Each archive is `bin/` -- `mxi` alone, every step a command of it; the Python
is the wheel's (Graeme, 8 November: everything in the wheel) -- with the
licence, the README and the release notes. Unpack it and put its `bin` on the
PATH. The program needs nothing installed: HDF5 and zlib are linked in, from
vcpkg (`vcpkg.json`), and on Linux the C++ runtime too; built in
manylinux_2_28, it needs glibc 2.28 or later, which packaging checks; the CUDA
driver is the system's. `scripts/package_release.sh` refuses to package a
build that would need anything else. `mxi-ui` and `mxi-view` need Python with
tkinter (`docs/ui.md`).

One program, not 24 each with its own HDF5: the archive is a few megabytes, where 24 programs had made it some 50.

## Snapshots

Every push to `main` runs the same workflow, and its builds -- the six
platforms', tested as a release's are -- replace the rolling pre-release
`snapshot`, its tag moved to the commit: `mxi-snapshot-<platform>.tar.gz`,
Windows' `.zip`, at
`https://github.com/graeme-winter/mxi/releases/download/snapshot/`. No wheels
and no source archive: the binary, for a machine with no compilers -- the GH200
Graeme tests on is not his (14 November). A newer push cancels an older one's
run; six archives or nothing is replaced.

What a build calls itself is set when it is configured, `MXI_VERSION_LABEL`: a
release's tag without its `v` -- a candidate says `1.3.2-rc1` -- or `snapshot`,
the source's version and the commit, `1.3.2+3806819`. Built by hand with
neither, the source's version, as before. `mxi --version` says it, and the
banner, the first line of every log.

## Licences

mxi's source is BSD-3-Clause. The release builds include FFTW, for indexing's
transform: a vcpkg feature, `fftw` (`vcpkg.json`), asked for with
`-DVCPKG_MANIFEST_FEATURES=fftw -DMXI_FFTW=ON`. FFTW is GPL, version 2 or
later, so the program as released is distributed under the GPL's terms as a
whole -- a modest commitment, every part's source being open. A build without
FFTW (the default, `-DMXI_FFTW=OFF`) uses the built-in transform and is BSD
with the notices below.

The code linked into the program comes with its notices, in each archive's
`licenses/`: gemmi (MPL-2.0, its source in the source archive), bitshuffle
(MIT) and the LZ4 within it (BSD-2), and in the Linux builds mimalloc (MIT),
from the submodules; HDF5, libaec, zlib
and FFTW, from vcpkg's copyright files; NVIDIA's CUDA runtime in the CUDA
build; and a README saying which is which, and what FFTW makes of the whole.
`scripts/package_release.sh` fails if a notice is missing, or if the program
contains FFTW the build does not record -- a new library linked in needs its
own added.

## macOS

The binaries are not signed, so macOS blocks them once downloaded. Clear the
quarantine flag once, after unpacking -- the release notes say so:

```sh
xattr -dr com.apple.quarantine mxi-1.1.1-macos-arm64
```

Signing and notarising would need an Apple Developer ID and its credentials
as the repository's secrets.

## When a build fails

Every build runs to its end whatever another does, so that one run reports on
all of them; nothing is published unless all pass. The CUDA toolkit comes from
NVIDIA's redistributable archives (`scripts/install_cuda.py`) -- the compiler,
the runtime and the headers it draws on, each checked against the SHA-256 its
manifest gives, each download tried five times -- not from NVIDIA's RHEL 8
repository, whose index once named files it did not have, for hours. A build that failed for something passing is
made again by running the workflow by hand for the same tag, its release
deleted first if one was made (above).

## What the builds test, and what they cannot

A build's own runner cannot see an instruction an older CPU lacks: GitHub's
arm64 runner is a Neoverse, with every extension, and 1.3.2's arm64 build passed
all its tests there and died with SIGILL on a Raspberry Pi 4 (Graeme, 16
November) -- mimalloc's build compiles it for ARMv8.1 on arm64 unless told
MI_NO_OPT_ARCH, LSE atomics in every malloc. The arm64 jobs now run
scripts/check_arm64_portable.py on the program: nothing beyond ARMv8.0 -- no
LSE, dot product or SVE -- outside libgcc's __aarch64_* outline atomics, which
have LSE in them and choose at run time, so that a plain grep of the disassembly
would fail every correct build.

Each build runs ctest and the Python suite, the tests that need no external
data, before its archive is kept, in a Python of our choosing, 3.12: the
Linux containers' own, and on macOS one set up for the purpose, the runner's
having once moved to a version (3.15) for which h5py and gemmi had no wheels.
Hosted runners have no GPU to rely on, so
the device tests (ctest's `dext_gpu`) are left out: the CUDA and Metal builds
are released tested on their CPU paths, their device paths tested on real
machines as before; the CPU build is tested whole.

mxi-ui's window tests are left out too (`MXI_NO_WINDOW_TESTS`): v1.1.4-rc1's
macOS test step seemed to stall -- the runners having problems then, or as
likely a window waiting on a hosted runner for a display that never comes --
and that cause is ruled out so. They run where there is a display -- xvfb in a
Linux session, a desktop -- so before a release, on a desktop,
`MXI_BUILD=build pytest -rs python/tests/test_ui_window.py
python/tests/test_ui_viewer.py` -- the viewer's tests make their data with
mxi, and without the build directory skip, every one, as on 7 November (`-rs`
says why any is skipped) (`docs/outstanding.md`, 87). Each test step is held
to 30 minutes, so that anything else that stalls fails in half an hour, with
its log, rather than after GitHub's six hours.

The Metal build downloads Apple's metal-cpp headers from the URL at the top of
the workflow (`METAL_CPP_URL`); if Apple renames the zip, the current name is
on https://developer.apple.com/metal/cpp/.

## The scripts, by hand

```sh
scripts/source_archive.sh v1.1.1         # mxi-1.1.1-source.tar.gz
scripts/release_notes.py v1.1.1 > notes.md
scripts/package_release.sh build v1.1.1 linux-x86_64
```

A static build like the release's, here, needs vcpkg:

```sh
git clone https://github.com/microsoft/vcpkg && vcpkg/bootstrap-vcpkg.sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=vcpkg/scripts/buildsystems/vcpkg.cmake \
      -DVCPKG_TARGET_TRIPLET=x64-linux-release
```
