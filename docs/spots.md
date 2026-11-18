# Spot finding: mxi find

The first step after import: every image of every sweep thresholded, the
signal pixels grouped into spots in three dimensions, and the spots written as
a reflection table with centroids, intensities and shoeboxes -- on the CPU, or
on a GPU through CUDA or Metal, the same spots either way.

```sh
mxi import /data/ins10_1_master.h5
mxi find imported.rflx            # -> strong.rflx
mxi find imported.rflx --gpu      # the threshold on the GPU
```

**The threshold** is the extended dispersion threshold, DIALS's default
(`DispersionExtendedThreshold`: kernel 3 x 3, sigma_b 6, sigma_s 3, threshold 0,
min_count 2, gain 1, the same treatment of the frame's border), in
`src/dext.{hh,cc}` and, window for window, in the GPU kernels
(`src/dext_metal.{cc,metal}`, `src/dext_cuda.cu`); the grouping six-connected
in three dimensions, the centroids and the table after it.

**The frames** are read straight from their compressed chunks, outside HDF5's
filter pipeline, and decompressed by bitshuffle (`src/nxmx_reader.cc`,
`src/decompress.cc`) -- the same reader integration uses.

**Sixteen-bit data on the device.** `real_t<std::uint32_t>` is `double` and
Apple GPUs have no double precision at all, so a 32-bit path under Metal would
be a different calculation from the CPU's. It refuses rather than quietly
running in float; `--gpu-force` narrows 32-bit frames to 16 bits where every
count fits (`mxi max` says whether they do). The CPU path takes both.

## Building

Spot finding is part of the one build (the README's "Building"); its options
are the build's: `-DMXI_METAL=ON -DMETAL_CPP_DIR=...` or `-DMXI_CUDA=ON` for a
device, `-DMXI_AVX2=OFF` for portable bitshuffle, `-DENABLE_FAST_MATH=ON` with
Metal (below). Configuring both device backends at once is an error rather
than a race at the linker; each is off by default and looked for only when
asked for, so a machine with neither still configures.

## Running

```
mxi find IMPORTED.rflx [options]
mxi find MASTER.nxs [options]

  -x MASTER.nxs      the images, where the experiment list's have moved
  -o file            a .rflx (strong.rflx, with the experiment list), or a
                     DIALS .refl, the table alone
  -j, --threads N   threads for everything this does in parallel; 0, every
                    core this job may use (0)
  -g, --gpu          the threshold on the GPU
  --gpu-force        32-bit frames narrowed to 16 bits for a 16-bit GPU path
  --no-shoeboxes     leave out the pixel data, which is most of the file
  --min-spot-size N  contiguous pixels a spot needs (3)
  --max-spot-size N  and the most it may have (1000)
  --max-separation D peak to centroid, in pixels; 0 turns it off (2)
  --subtract-background  centroids and intensities less the local background
  --2d               group each frame on its own, as for stills
  --z-offset N       array index of image number 0; from the experiment list
  --chunk N          frames held between groupings (a few per thread)
  -t timeout         seconds to wait for the file to appear (default 60)
  -p poll-ms         interval between checks while waiting (default 200)
  --timing, --verbose, --help, --version
```

`--version` says which backend was built in, because that is not visible from
the outside and the CPU and the two devices are not the same program:

```
$ mxi find --version
mxi find 1.3.0 (Metal)
```

What it prints follows `dials.find_spots`' summary, wording included, so that
the two can be read side by side:

```
$ mxi find -j 4 series.expt series.nxs
Experiments: 1 experiment, images 1 to 12, 1 panel of 256 x 384, identifier 1c0ffee0
Series series: 12 images of 256 x 384, from series.nxs, 4 threads, on the CPU
Grouping in three dimensions, 3 to 1000 pixels a spot, peak within 2.0 of the centroid
Thresholded 12 of 12 images in 0.0 s (516.4 images/s), 0 never written, 0 failures
Found 8928 signal pixels on 12 frames
Extracted 48 spots
Removed 0 spots with size < 3 pixels
Removed 0 spots with size > 1000 pixels
Calculated 48 spot centroids
Calculated 48 spot intensities
Filtered 48 of 48 spots by peak-centroid distance
Wrote 48 reflections to strong.refl (0.1 MB, most of it shoeboxes)
```

-- the synthetic fixture from `python/src/mxeq/fixtures/nxmx.py`, twelve frames
with forty-eight reflections planted in them, so that every number is
checkable. The centroids and the intensities are one pass here, so those
counts are equal by construction; both lines are printed anyway. The
peak-centroid line appears only when that filter is on, since "Filtered 48 of
48" would otherwise read as a filter that passed everything.

`--gpu` fails, rather than falling back, if the build has no backend or no
device is present: it is an explicit request, and answering with the other
implementation would misreport where the results came from.

`mxi find` waits for a master file that does not exist yet, up to `-t`, and
takes frames as their files are written -- a bounded queue between the reading
and the threads that threshold (`src/queue.hh`) -- so it can be pointed at a
series still being collected.

### The experiment list

The experiment list -- `mxi import`'s, or a DIALS `.expt` -- says where each
sweep's images are, and three things more (`src/experiment_images.*`, which
unlike the full reader takes a scan with no oscillation, for stills):

* **The scan's image range**, because a reflection's `z` is an array index, not
  an image number: the array index of image *n* is *n* - 1, an NXmx series
  numbers its frames from zero, and a sliced import starts higher -- so the
  list sets `--z-offset`, and says so when it is not zero. Getting this wrong
  indexes nothing, and neither file says why.
* **The panel size**, so that a series which is not the one the experiment
  describes is refused here rather than surfacing as a nonsensical lattice
  later.
* **The experiment identifier**, which ties the table to its experiment.

Given a master directly, `z` is the frame's own index and the table carries no
identifier.

**Only the scan's frames are read.** A series offers only the frames its scan
covers (`Series::restrict_frames`), and a scan claiming more images than the
file holds is an error: spot finding 1800 images of a 36000 image run once
thresholded all 36000 with a warning. The `.expt`'s `single_file_indices` say
which file index each scan image is, so the offset is `(first_image - 1) -
first_index` -- nothing when the two line up, five for a slice file starting
at zero; without them the file index of image *n* is taken as *n* - 1, the
same assumption the restriction makes, so the frames read and the z they are
given cannot disagree. `python/tests/test_spotfinder_scan_range.py` builds both
layouts.

From 100 images (`--progress-from N` to change it), `mxi find` shows its
progress through the series as `mxi integrate` does: a bar on a terminal, as wide
as the terminal allows, written to the terminal alone so that the log does not
keep it, and a line each tenth of the way where the output is not a terminal.
Frames are counted as their chunk finishes; reading a stream, whose length is
not known, it shows none.

## Hot pixels

Each image's signal pixels are counted against their pixel as they are grouped,
and after the last image those signal on `--hot-fraction` of them or more
(0.25), from 100 images, are said, and with `--mask-hot` written to the output's
untrusted regions. docs/mask.md, Hot pixels.

## Three dimensions, not one frame at a time

A reflection is swept through the Ewald sphere over a degree or two and lands on
several frames. Grouped per frame it comes back as three or four spots with
three or four centroids, and nothing downstream can put them together again. So
the grouping is DIALS': six-connected in x, y and z, a transcription of
`PixelListLabeller::labels_3d`. `--2d` is for stills, and is what
`spotfinder.force_2d=True` means over there.

It is streaming, which DIALS is not. Only frames *f* - 1 and *f* can be
connected, so a component with no pixel on the frame just added can never grow
again: it is written out and its pixels are dropped. DIALS holds every signal
pixel of the whole sweep and labels the lot at the end, which at a few hundred
thousand pixels a frame is gigabytes.

That is also why frames have to reach the grouping in order, and why they are
collected a chunk at a time: dispatch a few frames per thread, wait for them,
sort, group. The barrier costs a fraction of one frame's latency per chunk, and
the alternative -- a reorder buffer that has to know a frame it is waiting for
will never arrive, which is exactly what an unallocated chunk is -- is a great
deal more machinery for that fraction. `--chunk` is the memory knob.

A gap in the frame numbering is not an error. An unallocated chunk is a frame the
writer never received; nothing can be connected across a frame that does not
exist, so everything open closes at the gap and the reflections that would have
spanned it come back as two spots each.

Because frames reach the grouping in order however they were scheduled, the
table does not depend on `-j` or `--chunk`: one thread and eight produce
byte-identical files. `tests/regression.sh` checks that.

## What is in the table

The columns `dials.find_spots` writes, with the same names and the same types:
`bbox` as `int6`, `flags` carrying `Strong` and nothing else, `id`,
`intensity.sum.value` and `.variance`, `n_signal`, `panel`, `shoebox`,
`xyzobs.px.value` and `.variance`.

A `.refl` is a msgpack document around raw column dumps and nothing else -- no
header, no compression, no pickle:

```
[ "dials::af::reflection_table", 2,
  { "identifiers" : { id : identifier, ... },
    "nrows"       : N,
    "data"        : { name : [ type, [ N, <binary> ] ], ... } } ]
```

which is why this needs neither DIALS nor a msgpack library. The reference is
`dials/array_family/reflection_table_msgpack_adapter.h`. Two things about it
are worth knowing: a column type name DIALS does not recognise makes the whole
file unreadable rather than partly readable, and the dumps are memory images, so
the format is portable across neither endianness nor the width of
`std::size_t`. DIALS has the same property and is silent about it; this refuses
to write where the result would not read back.

The centroid, its variance and the intensity are DIALS' own -- `centroid_valid`,
its unbiased standard error plus the 1/12 DIALS adds, and `Summation` over a
shoebox whose background is all zero. All of it reduces to closed forms over the
pixel list, because the spot finder fills only the signal pixels of a shoebox,
so nothing goes back to the images a second time -- the shoeboxes included.

The shoeboxes are nearly the whole file: a dense block of float data, byte mask
and float background per spot, against a few bytes a row for everything else.
They carry nothing the pixel list does not. Worth having for
`dials.image_viewer`; `--no-shoeboxes` for indexing.

One panel. A segmented detector would need a panel number per spot, and is
refused rather than guessed at.

Several experiments -- several sweeps, as `mxi import a.nxs b.nxs` writes them --
are found one after another, each from the images its own image set names, into
one table: every spot with its experiment's index as its `id`, and each
experiment's identifier in the table's map, as dials.find_spots writes them. The
rows of each are exactly what that experiment alone gives, in order. A master
file named as well is refused, as it cannot belong to them all.

## Against dials.find_spots

Not "it is broken" -- the list is short and each item is attributable:

* `dext` evaluates the dispersion test in float where DIALS uses double, so a
  spot at the threshold can fall either side of it.
* The detector's own pixel mask, as DIALS reads it: NXdetector's
  `pixel_mask` (or, in older DECTRIS masters, `detectorSpecific/pixel_mask`), a
  pixel bad wherever it is not zero. The NXmx reader reads it when it opens the
  series, and every frame decoded has the masked pixels stamped with the
  sentinel, `max() - 1`, before anything else sees it -- so spot finding,
  integration and `mxi max` all leave them out, as they leave out the sentinels
  the detector wrote, with no change to any kernel. A mask of the wrong shape is
  not used, said so. Until October 2026 only the sentinels were recognised, and
  a bad pixel recorded with a plausible value was masked by DIALS and measured
  here: on a small molecule of Graeme's, one near the detector's edge gave every
  shoebox over it a background a hundred thousand times too high, and two
  observations at -25 sigma. On insulin, whose 260861 masked pixels never
  carry counts, spot finding and integration are byte-identical either way.
  `python/tests/test_pixel_mask.py`.
* DIALS sums a shoebox in float (`ProfileFloatType`), this in double, which
  shows up only above 2^24 counts in one spot.
* Rows come out as their components close rather than in first-pixel order over
  the whole sweep. Nothing depends on row order.

A per-cent tail of unmatched weak spots is that list. Wholesale disagreement is
a bug.

## Checking it

```sh
python -m mxeq.checks.spot_table strong.refl      # well formed, and what is in it
mxeq check strong dials_strong.refl strong.refl   # how two spot lists differ
tests/spots/regression.sh                         # everything, no detector needed
```

`mxeq.checks.spot_table` decodes a table the way DIALS's own adapter reads it
and checks every column's type and length. Given two files it matches spots by
centroid and reports what matched, how far apart, and where the unmatched ones
are -- bunched at a detector edge or in a block of frames says something quite
different from scattered and weak. It needs nothing but python3.

`tests/spots/regression.sh` generates a synthetic NXmx series with reflections
planted at known positions, three frames each, and checks the whole path: the
table decodes, every reflection comes back as one spot where it was planted,
`--2d` gives exactly three times as many, a missing frame splits the
reflections that spanned it and no others, one thread and eight agree byte for
byte, and a mismatched experiment list is refused. `python3 -m
mxeq.fixtures.nxmx DIR 12`, `mxi find DIR/series.expt DIR/series.nxs` and
`python -m mxeq.checks.planted_spots DIR/manifest.json strong.rflx` do the same
by hand.

## Tests

In `mxi_tests` (`./build/mxi_tests spots_`), self-contained -- no detector, no
fixtures to generate first:

| | |
| --- | --- |
| `spots_dext_squares` | where the 32-bit window sums stop being exact, and the margin |
| `spots_dext_fused` | the fused kernel's phases, on the CPU, against the threshold |
| `spots_signal_order` | the O(n) index sort against `std::sort` |
| `spots_grouping` | the grouping, its centroids and its filters, by hand |
| `spots_table_format` | the reflection table, decoded again by a second implementation |
| `spots_table_golden` | the table's bytes, held to what they have been |
| `spots_experiment_images` | the experiment list reader, including the awkward shapes |
| `spots_end_to_end` | the threshold, the grouping and the table together |

And `dext_gpu`, a program of its own on a GPU build: the device against the
CPU, with no tolerance at all. It runs both over synthetic frames chosen for
the edges -- sizes that are not multiples of the 32 x 32 threadgroup, spots
planted hard against every border, both masked sentinels -- under both window
variants, and compares every field of every signal pixel; it skips rather
than fails on a machine with no device. `spots_end_to_end` is the one that says
the coordinate conventions are right: a mistake in fast against slow, pixel
corners against centres, or frames against array indices shows up there as a
systematic offset and nowhere else as anything at all.

## The threshold

Each frame goes through the extended dispersion spot finder: a dispersion test
over a 7 x 7 window, an erode, then a Poisson test against a local mean over
11 x 11. It emits the surviving pixels as a packed list rather than a mask --
index, value, local background, window population, 16 bytes each -- which is
smaller on a sparse frame, removes a scan of the mask at both ends, and on the
device removes the download of a whole frame to recover about one per cent of
it.

Pixels at or above `numeric_limits<T>::max() - 1` are treated as masked. Only 16
and 32 bit are supported; 8-bit data is refused rather than promoted.

The window sums are accumulated in 32 bits at 16 bits a pixel, and the tables
wrap over a frame without that mattering: the summed-area identity holds in
Z / 2^32, so differencing a wrapped table gives the true window value provided
that value fits. For the count and the sum it always does. The sum of squares
has a limit -- a uniform 7 x 7 window reaches 2^32 at 9362 counts a pixel -- and
real frames are nowhere near it: the busiest window that can be built out of
reflections, two 2500-count spots two pixels apart, is a factor of 30 short, and
saturated pixels are masked before they can contribute at all.
`mxi_tests spots_dext_squares` measures that margin rather than assuming it, so a
change to the kernel size or to the masking shows up as a failure there.

## Reading NXmx

The older DECTRIS file writer's master has no `/entry/data/data`, only
`/entry/data/data_000001` and on, each an external link to a data file's own
stack: then a block a link, in order, each link's file resolved against the
master's directory and its frames following the last's, and a link whose file
is not there refused by name (`docs/import.md`). Otherwise:

`/entry/data/data` in the master file is a virtual dataset over the data files.
A virtual dataset cannot be chunk-read, so the mapping is unpacked instead:
`H5Pget_virtual_filename`, `H5Pget_virtual_dsetname` and the two selections
give, for each block, which data file holds which frames and where they start in
it. Frames are then read with `H5Dread_chunk` in the worker threads, so the
compressed bytes reach the threshold without HDF5's filter pipeline ever
running. The bitshuffle plugin does not have to be installed, and nothing here
assumes a layout the mapping does not itself state -- not `prefix_NNNN.h5`, not
1000 frames a file, not a filter identifier.

Every HDF5 call is made under one lock, because the C library keeps global state
and is not thread-safe unless it was built that way -- vcpkg's default port is
not, and giving each thread its own file handle does not help. That costs
nothing: a thread-safe HDF5 takes a single global lock across its whole public
API anyway, so those builds were already serialising reads, and what runs in
parallel either way is the decompression and the threshold, which is where the
time goes.

A frame the writer never received is an unallocated chunk, which reads back as
the fill value. Those are reported as "never written" and not thresholded,
rather than analysed as a frame of saturated pixels.

## Building a GPU backend

### Metal

```sh
cmake -S . -B build -DMXI_METAL=ON -DMETAL_CPP_DIR=/path/to/metal-cpp
```

metal-cpp is Apple's header-only C++ wrapper over the Metal API, distributed as
a zip from <https://developer.apple.com/metal/cpp/> rather than through any
package manager, so it is pointed at rather than found. Everything else comes
from the Xcode command line tools.

The build compiles `src/dext_metal.metal` with `xcrun -sdk macosx metal
-fno-fast-math` and embeds the resulting `.metallib` into the binary, so the
tool carries its own kernels and cannot get out of step with a shader file
installed beside it. `-fno-fast-math` is not optional: the Metal compiler
enables fast math by default, and the claim this backend makes is that it agrees
with `dext.cc` bit for bit.

The device shares memory with the host, so there is no upload and no download.
`gpu::host_alloc` hands back the contents of a shared `MTLBuffer`, the frame is
decompressed straight into it, and the kernels read it where it lies; the packed
list of signal pixels comes back out of memory the CPU already reads. At 16M
pixels that is 36 MB of traffic per frame that does not happen.

### CUDA

```sh
cmake -S . -B build -DMXI_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES="80;90"
```

Architectures come from `CMAKE_CUDA_ARCHITECTURES`, since the right answer is
whichever cards this will run on; left unset it uses `native` on CMake 3.24 and
later. There, `host_alloc` returns page-locked memory so the copies to and from
the device are asynchronous rather than running at half rate -- and the copy to
the device is not small: 36 MB at 16M pixels is about 2.7 ms over PCIe 3.0 x16,
a fifth of a frame.

### Fast math, and what it costs

```sh
cmake -S . -B build -DMXI_METAL=ON -DENABLE_FAST_MATH=ON
cmake --build build -j
ctest --test-dir build -R dext_gpu --output-on-failure   # what it changed
build/bench_dext_gpu                                     # what it bought
```

The shader is normally compiled `-fno-fast-math`, against the Metal compiler's
default, because the claim the device backends make is that they agree with
`dext.cc` bit for bit and fast math is licence to reassociate exactly the
arithmetic that claim is about. `-DENABLE_FAST_MATH=ON` asks for the other one:
`-ffast-math` for the shader, `-use_fast_math` for nvcc.

It reaches the device only. `dext.cc` is the reference the device is measured
against, so compiling that the same way would move the thing being measured and
leave nothing to measure it with. Asking for it in a build with no backend is a
configure warning rather than a silent no-op.

Both instruments change their minds about what to do with a disagreement:

* `dext_gpu` tolerates a background differing in its last bits, says
  how many pixels differ and by how much in absolute and relative terms, and
  still fails if the *set* of pixels found has changed. That is the part fast
  math is not allowed to do quietly -- a spot that appears or disappears is a
  different answer, not a rounder one.
* `bench_dext_gpu` reports the same comparison and then prints its timings
  anyway, where without the flag it refuses to time something unverified. The
  timings are the reason for the build.

And `--version` says `fast math`, because a reflection table from such a build
is not one to compare against DIALS without knowing that.

### Two ways to sum a window

`stage0` needs the count, sum and sum of squares over a 7 x 7 window; `stage2`
needs the masked local mean over an 11 x 11 one. Each can sum its window's
pixels directly or build summed-area tables in threadgroup memory, and each
choice is independent:

```sh
MXI_GPU_STAGE0=tile   mxi find --gpu series.nxs
MXI_GPU_STAGE2=direct mxi find --gpu series.nxs
```

`tile` touches each pixel about twice but fills its tables with a few dozen of
the threadgroup's 1024 threads, in two serial passes, with the rest waiting at a
barrier -- and it holds threadgroup memory. `direct` sums every pixel's window
itself, 49 loads a pixel at `stage0` and 121 at `stage2`, with no barrier and
all 1024 threads working throughout.

On paper the table loses. In fact it wins comfortably at `stage2`: per-pixel load
traffic costs more than the barrier saves. Measured per frame at 4362 x 4148,
20 repeats:

| stage0 | stage2 | Apple silicon | workstation CUDA |
| --- | --- | --- | --- |
| `tile` | `tile` | **4.09 ms** | 12.37 ms |
| `direct` | `tile` | 4.86 ms | **12.10 ms** |
| `tile` | `direct` | 8.13 ms | 15.01 ms |
| `direct` | `direct` | 9.24 ms | 16.06 ms |

`stage2`'s tile wins on both and is not in doubt: 25% on CUDA and better than
50% on Metal. `stage0` is where the two devices disagree -- 16% for `tile` on
Apple silicon, 2% for `direct` on the CUDA card -- which is why the default for
that stage is asked of the backend rather than fixed, and why both variants are
kept.

Read the two columns apart rather than against each other. The Metal column is
from a run after the benchmark stopped timing a staging copy the tool does not
make; the CUDA column is from before, where the same fix means something
different -- on CUDA the copy is real, and it was copying ordinary rather than
page-locked memory. Its 2% is the number most in need of a re-run.

Measure rather than trusting the table:

```sh
build/bench_dext_gpu                 # 4362 x 4148, 20 repeats, one worker
build/bench_dext_gpu 4362 4148 20 16 # and aggregate across 16 workers
```

It times all four combinations, reports the CPU for scale, refuses to draw a
conclusion if two variants disagree about what they found, says how dense the
frame it measured was -- the planted frame carries about a hundred times more
signal pixels than diffraction does, and two lines of the per-stage profile are
proportional to that count -- and reports aggregate throughput as well as
single-frame latency -- which is a different question: a
configuration doing more load traffic per pixel loses more as workers pile up,
because they compete for the same memory.

## Putting the signal pixels in order

The device emits its signal pixels compacted per SIMD group, so they arrive in
whatever order the groups finished, and the grouping needs them ascending by
index. `std::sort` was doing that and it was the most expensive thing in
`gpu::find()`: 5.8 ms of a frame at 126,002 signal pixels, against 1.7 for
`signal_order::by_index`, and the gap widens with density -- at 600,000 pixels
it is 33 ms against 9. The flat index is bounded, so one pass over its high bits
puts every pixel in a bucket of consecutive indices; the buckets are then in
order by construction and each holds about four pixels. O(n) rather than
O(n log n), and it uses nothing about the emit order, which is a property of the
threadgroup shape and the scheduler and would break quietly if relied on.

## Subtracting the background

The threshold emits each signal pixel with its local background: the mean, over
its final window, of the pixels the dispersion stage did not take for signal --
a fair background, the spot left out. DIALS' spot finder discards it, weighting
each centroid by raw counts and summing them for the intensity, and `mxi find`
does the same by default, byte for byte. `mxi find --subtract-background` keeps
it:

* the centroid weighted by count less background, clamped at zero, so that a
  weak spot is not drawn towards the middle of the pixels selected, nor in z to
  the middle of its images;
* the intensity the sum of count less background, unclamped, its variance still
  the counts';
* the background written into the shoeboxes, where `mxi integrate`'s profile
  model subtracts it before measuring sigma_b and sigma_m.

It is the same on every backend, since the background comes from the CPU's,
Metal's and CUDA's thresholds alike. On the 300 image insulin sweep, through
the whole chain against the default: 12920 spots against 12915, the median
intensity 232 against 246; indexing the same; refinement's RMSDs 0.2954 px,
0.2311 px and 0.2373 images against 0.2981, 0.2327 and 0.2401, about 1 per cent
better on each; sigma_b 0.0270 degrees against 0.0273, sigma_m unchanged. A
strong sweep with little background, so a small effect; weak data with more
background should show more.

## 32-bit frames on a GPU of 16 bits

Metal's threshold takes 16-bit frames only, Apple's GPUs having no double
precision. Many detectors write 32-bit frames whose counts, the dose kept down
for radiation damage, never come near 65535. `mxi max EXPT` reads and
decompresses every frame on every core and reports the largest count on a
valid pixel -- neither the bad pixel's marker, max() - 1, nor the tile join's,
max(), a count, and each reported apart -- and whether it is below 0xFFFD, the values above being the 16-bit markers; it exits 0 if so.

`mxi find --gpu --gpu-force` then narrows each 32-bit frame to 16 bits before
the threshold, keeping each pixel's low half: the bad pixel 0xfffffffe and the
tile join 0xffffffff become the 16-bit markers 0xfffe and 0xffff, and every count
is copied as it is, in one branch-free pass the compiler makes SIMD. A valid count of 0xFFFD or more
stops it, the image and the count named -- it does not trust that `mxi max` was
run. Values and badness carry over exactly, so the spots are the 32-bit
threshold's, byte for byte; `python/tests/test_max.py` holds it to that on a
planted 32-bit series with a tile join's column and a dead pixel, and holds the
refusal to a planted count of 70000. `--gpu-force` narrows on the CPU's
threshold too, which is how that is tested where there is no GPU.

## The fused kernel

`MXI_GPU_FUSED=1` runs the three stages as one CUDA kernel
(`src/dext_fused.hh`), for 16-bit frames; 32-bit frames and Metal run the
three kernels whatever it says. On an RTX 4060 the three kernels were the limit,
8.9 ms a frame, and each read the whole frame, the first two writing a
whole-frame mask back to device memory: five passes over memory a frame. The
tile kernels built their summed-area tables with 42 of 1024 threads, one a row.

The fused kernel takes a 32 x 32 tile of output and reads its pixels once, with
the halo all three windows need (52 x 52), keeps the two masks in shared memory,
and sums every window separably -- rows, then columns -- across all of the
block's threads. A pixel outside the frame is loaded as masked, which is exact:
it adds to no sum, sets stage 0's mask so the erode cannot grow through it, and
is left out of the background, as each kernel treats one outside the frame. The
sums are integers, the same in any order, and the tests are written as the three
kernels write them, so the answer is theirs bit for bit.

The phases are host-and-device functions of one item each: the kernel spreads
them over its threads, and `dext_fused_emulated()` runs them in order on the CPU.
`test_dext_fused` compares that with `dext()` on 49 frames -- sizes not multiples
of the tile, spots on the edges and corners, masked and saturated pixels, a masked
row and column, both pixel types, and one 3262 x 3108 frame with 916077 signal
pixels -- and every field of every pixel agrees; an off-by-one in either of two
halo indices fails 37 of them. `test_dext_gpu` runs the kernel itself against the
CPU on CUDA, beside the four window pairs. It compiles here with nvcc 12.0 for
compute capability 8.9, no warnings; it has not run, there being no GPU here.

## Status

* **The Metal backend:** 1800 images of 3108 x 3262 pixels thresholded at 502
  images a second on a Mac's GPU, 1917439 signal pixels and 146118 spots, with
  the stage 0 and stage 2 tile windows. `dext_gpu` is the first thing to run
  on a new machine.
* **The CUDA backend agrees with Metal exactly** on the same images -- 16-bit,
  stage 0 direct and stage 2 tile, an RTX 4060 -- every count the same, at 196
  images a second. Its 32-bit instantiation, and its `stage0_tile` and
  `stage2_direct` kernels, have not been run; `MXI_GPU_STAGE0=tile
  MXI_GPU_STAGE2=direct` selects those two.
* **Real diffraction:** on a 300 image insulin sweep every filtering count
  matches `dials.find_spots` exactly -- 18755 extracted, 5683 and 5 removed for
  size, 13067 centroids, 12915 kept -- and the pipeline indexes, refines and
  integrates what it writes.
* **The table** is written by the library's writer, as every program's is,
  which DIALS reads; the bytes are those the spot finder's own writer gave
  (`spots_table_golden`).

## What it uses

bitshuffle, MIT, and lz4, which comes with it, BSD 2-Clause; HDF5, a BSD-style
licence of its own.
