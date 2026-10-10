# rflx: one HDF5 file a step

STATUS: agreed and built, October 2026: HDF5 required; the library,
src/rflx.cc -- the tree, the tables and the file; mxi_convert; mxeq reading
.rflx; every program reading it, and writing it unless told the pair. On the
insulin sweep the chain from images to MTZ on .rflx alone writes, step by step,
exactly what the pair chain writes, and the same MTZ but for the time it was
written; uncompressed and written directly, each step takes the pair chain's
time to within the noise.

mxi's primary file format becomes `.rflx`: one HDF5 file holding the experiment
list, the reflection table, or both, in the layout specified by dxtbx-h5
(github.com/graeme-winter/dxtbx-h5, docs/HANDOVER.md, Part 1). Each program
writes one `.rflx` holding everything that step knows, so the next step reads one
file, not a pair that may not belong together. One converter, mxi_convert, moves
between `.rflx` and the DIALS pair, `.expt` and `.refl`; and every program still
reads the DIALS pair directly, as it does now.

## Decisions (Graeme, 9 October 2026)

1. **HDF5 is required** to build mxi, where today the core builds without it
   and only the spot finder and the importer need it. In practice nothing useful
   runs without those two.
2. **One converter**, its direction told from the files it is given. Files are
   named `.rflx`.
3. **Programs still read `.expt` and `.refl`**, as the code to do so exists and
   it is convenient.
4. **No DIALS links**: nothing is written under `/dials`. A `.rflx` reaches DIALS
   through the converter.

## The format

dxtbx-h5's HANDOVER, Part 1, is the specification, and mxi conforms to its
checklist (its section 8) as reader and writer. The points mxi depends on:

* **Types are carried by HDF5**: a value's dtype and dataspace are its type.
  Whether a value is an attribute or a dataset carries no meaning; a reader looks
  in both.
* **`/experiments`** is the dictionary dxtbx's `ExperimentList.to_dict()` makes
  -- exactly what `.expt` holds as JSON -- written as a group tree: a dict a group
  with `__type__ = "dict"` and its keys, in order and as spelled, in `__keys__`;
  a rectangular list of one type an array; a ragged or mixed list a group with
  `__type__ = "list"`, `__len__` and children `0`, `1` and on; strings UTF-8,
  integers int64, floats float64, null a null dataspace. Keys escaped (`%`, `/`,
  a leading `_`, the empty key) so that any key is a name and none collides with
  the reserved ones. Lossless: integers and floats stay apart, key order is kept.
* **`/reflections/{n}`**, a group a table, carrying `identifiers` and
  `experiment_ids` -- the map from the `id` column to experiment identifiers --
  and `nrows`. A row's experiment is its `id`, never which group it is in.
* **A column is a dataset**, its dtype the contract: bool, int32, uint64,
  float64; (N, 2) and (N, 3) float64 for vectors, (N, 3, 3) for a matrix, (N, 3)
  int32 a Miller index, (N, 6) int32 a box. `dials_type` names the type outright.
* **Shoeboxes** are a group: `shoebox_data` and `shoebox_background` float32 and
  `shoebox_mask` uint8, each the concatenation of every shoebox's pixels, z
  slowest, sliced by the bounding boxes; with `bbox` and `panel`. The mask is
  uint8 -- the HANDOVER's warning, and the mistake mxi made once in msgpack.

### What mxi writes

* Root attributes: `format = "dxtbx_h5"`, `format_version = 1`, `creator` naming
  mxi and its version, `timestamp`.
* `/experiments`, always: every step's file carries the models as that step left
  them.
* `/reflections/0`, one table, its rows' experiments by `id`, as mxi's tables are
  now -- several sweeps in one table. Written by every program that has
  reflections: all but mxi_import.
* `dials_type` on every column, which the format allows and a reader may use.
* No compression in the programs' own files, read by the next step: gzip on one
  thread made Graeme's benchmark, the 16M sweep from images to scaled data,
  half again as long, 36 s to 54, and uncompressed HDF5 writes as fast as
  msgpack (strong.refl's 14.7 MB in 0.07 s; gzip took 0.17 s for 2.1 MB).
  mxi_convert compresses, for files kept or sent, as dxtbx-h5 writes -- gzip at
  level 1 and shuffle from 256 elements, which every HDF5 reads, where LZ4
  needs a plugin -- unless told `--no-compress`.
* Nothing under `/dials`, and no `/images` links.

### What mxi reads

Any conforming file: experiments only, reflections only, or both; several table
groups, joined in order, each row's experiment by its `id`. The earlier layouts
the HANDOVER describes (its section 6), and DIALS's own HDF5 `.refl`, are not
read: those, if wanted, come later and deliberately, through the converter.

## The programs

**Input.** One `.rflx`, or the DIALS pair as now:

    mxi_index imported.rflx                    # one file
    mxi_index imported.expt strong.refl        # the pair, as now

A program needing reflections refuses a `.rflx` without them, by name.

**Output.** One `.rflx`, named for the step -- `imported.rflx`, `strong.rflx`,
`indexed.rflx`, `refined.rflx`, `integrated.rflx`, `symmetrized.rflx`,
`scaled.rflx` -- holding the models and the step's reflections. `-o PATH` names
it; its extension decides its meaning: `.rflx` one file, `.refl` the meaning `-o`
has now in mxi_find, mxi_integrate and mxi_scale, so that scripts keep working.
`--output-expt` and `--output-refl`, where a program has them, still write the
DIALS pair when given, any half not named under its old default name. So a
script naming its outputs writes what it always wrote: Graeme's chain scripts
need no change. mxi_import's `-o` naming an `.expt` writes JSON, as before.

Every program writes its `.rflx` directly, with no file written only to be
read again -- as the first versions of these two did, at a cost Graeme had
noticed. mxi_find encodes its table with the spot finder's own writer into
memory, reads it back as a file's would be, and writes it into the `.rflx` with
the experiment list it was given -- none, given only a master, and then the
`.rflx` holds the spots alone. mxi_integrate's final write, of one sweep, of
several joined, or of the second run after post-refinement, goes to the
`.rflx`; only its intermediate files -- each sweep's run, the first run and the
post-refined models before the second -- are pairs, beside the result, removed.

So the chain becomes

    mxi_import    master.nxs         # imported.rflx
    mxi_find      imported.rflx      # strong.rflx: the models and the spots
    mxi_index     strong.rflx        # indexed.rflx
    mxi_refine    indexed.rflx       # refined.rflx
    mxi_integrate refined.rflx       # integrated.rflx
    mxi_symmetry  integrated.rflx    # symmetrized.rflx
    mxi_scale     symmetrized.rflx   # scaled.rflx
    mxi_export    scaled.rflx        # scaled.mtz

**The converter**, its direction from what it is given:

    mxi_convert indexed.expt indexed.refl              # indexed.rflx
    mxi_convert indexed.rflx                           # indexed.expt, indexed.refl
    mxi_convert indexed.rflx -o other.rflx             # a .rflx rewritten
    mxi_convert indexed.expt                           # experiments alone

Either half may be absent, as the format allows; a `.refl` it writes is msgpack.

**mxeq** reads `.rflx` wherever it takes an `.expt` or a `.refl`, through h5py.

## Implementation

Three pieces of library, each tested on its own before any program uses it:

1. **The tree**: `json::Value` to and from an HDF5 group by the HANDOVER's section
   2.2. mxi already reads `.expt` into a `json::Value` before building its models,
   so `/experiments` becomes a second way to get the same tree, and no model
   code changes.
2. **The tables**: mxi's `Table` to and from `/reflections/{n}` by its section 3,
   the dtype contract in both directions; shoeboxes between mxi's records and
   the three flat arrays.
3. **The file**: open, check, read either half or both; write with the root
   attributes; HDF5's own lock around every call, as the frame reader takes it;
   across HDF5 1.10 to 2.0, whose API differs in places this will touch.

Then mxi_convert; then each program's input, then its output; then mxeq.

## How it will be judged

* **Conformance with dxtbx-h5**: its reference file, written by its
  make_reference_file.py -- every scalar type, an empty list, a null, a ragged
  and a mixed list, a nested model, every column type, shoeboxes -- read by mxi
  to the contents the script says it holds. The script needs the dxtbx_h5
  package, so the file is generated once with it and kept as a fixture, with the
  dxtbx-h5 commit it came from. And where dxtbx-h5 is installed, a test that
  `dxh5 info` and `dxh5 split` read what mxi writes.
* **Round trips**: an `.expt` through `.rflx` and back equal as JSON; a `.refl`
  through `.rflx` and back to the same bytes, shoeboxes included.
* **The programs**: each, given a `.rflx`, writing exactly what it writes given
  the DIALS pair the `.rflx` was made from.
* **Cost**: reading and writing time and size against msgpack, on the 3600 image
  insulin sweep's integrated table with shoeboxes.

## Open questions

1. **Compression**: settled, October 2026 -- none in the programs' files,
   gzip in mxi_convert's (above). Parallel gzip, compressing chunks on every
   core and writing them direct, could make a compressed default cheap, if disk
   ever matters more.
2. **Masks, gain and pedestal**, which dxtbx stores as absolute paths: kept as
   paths, as dxtbx-h5 keeps them for now, so a `.rflx` moved away from its masks
   loses them, as an `.expt` does.
3. **DIALS's own HDF5 `.refl`** (dials/dials#3255): through mxi_convert, later,
   when it is settled.
4. **`.rflx` from DIALS directly**, by dxtbx-h5's `dxh5 combine`, which writes
   this layout already: nothing more needed from mxi.
