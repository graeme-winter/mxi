# mxi_import

STATUS: first version, 2 October 2026; several masters, 5 October. Right on
masters from two writers -- DECTRIS's for insulin (the 4M cut of ins10_1),
redhorn-archive's for thaumatin (thau_1_3) -- its goniometer, axes, sensor, mu,
trusted range and scan identical to dials.import's on insulin. To be made right on the population of
data sets one at a time, `mxeq compare-expt` saying what differs.

`mxi_import master.nxs` writes `imported.rflx` (`docs/rflx.md`) -- or, with
`-o imported.expt`, the `.expt` that `dials.import master.nxs` writes -- so that
nothing of DIALS is needed for the chain. `mxi_import a.nxs
b.nxs` writes one experiment a master, as dials.import writes several sweeps:
each with a beam, detector, goniometer, scan and image set of its own, the
experiments' indices numbered past those before, each its own identifier. The
overrides apply to every master alike.

## What it reads

* **The beam:** `incident_wavelength` (instrument's or sample's NXbeam), or
  `incident_energy`; a wavelength that varies through the scan is its first
  value, said so. Direction along +z in DIALS's frame (the beam travelling
  along -z), and dials.import's polarisation defaults: fraction 0.999, normal
  +y.
* **The detector:** a panel for each NXdetector_module, named by its path.
  The module's fast and slow pixel directions give the pixel size (their
  values) and directions (their vectors); everything they depend on places the
  module. A chain is followed from its dataset through each `depends_on`, a
  relative path resolved from the dataset's group, to `"."`; each step is
  `R p + offset` for a rotation and `p + offset + value vector` for a
  translation, innermost first, at the first image's value. An offset without
  `offset_units` is in the transformation's own `units`, as nxmx reads it:
  Diamond's Eiger masters write offsets so, in metres -- taken as millimetres
  they put the detector a thousandth of the way out. A rotation's offset with
  none is taken as millimetres, said so. Then from NeXus's
  McStas frame to DIALS's imgCIF, a half turn about y: x and z negated.
  `data_size` and `data_origin`, slow then fast, are the image size and raw
  offset, fast then slow.
* **The sensor:** `sensor_material` (Silicon, CdTe, GaAs, Ge to their symbols),
  `sensor_thickness`, and mu -- not in the file -- from Hubbell and Seltzer's NIST
  mass attenuation coefficients as cctbx's eltbx tabulates them and dials.import
  reads them, for the materials dxtbx knows: silicon, CdTe and GaAs. Log-log in
  energy, the interval found by cctbx's own rule -- the first point above the
  energy and the one before it -- which matters at CdTe's and GaAs's absorption
  edges, in the MX range and given twice in the tables: Cd's K edge at 26.711
  keV takes mu/rho from 9.834 to 29.43 cm^2/g. Times cctbx's density -- 2.33,
  6.2 and 5.32 g/cm^3 -- on insulin's 0.953738 A, 3.663092965478474 per mm for
  silicon, dials.import's to the last digit. Another material, or an energy
  outside its table, has mu 0, no parallax correction, and says so -- `--mu`.
* **The trusted range:** 0 to the lower of two limits. The detector's: the
  count a photon counter can still correct for, set mainly by the exposure
  time -- as low as 31881 at 2 ms, far higher for longer -- which the master
  gives as `saturation_value` or the DECTRIS `countrate_correction_count_cutoff`.
  And the data type's: the two largest values of the image's type are markers,
  a bad pixel and a tile join, so the largest count is 2^bits - 3, 65533 for 16
  bits. The bit depth from `bit_depth_image` or `bit_depth_readout`, or the
  first data file's own type -- not the virtual dataset's, which a writer may
  widen (Diamond's is int64 over its counts). A DECTRIS master gives both
  limits as links into the `_meta.h5` beside it: a link into a file that is not
  there is said so, where it leads, not taken for absent. With neither limit
  to be had the top is 2147483647, as dxtbx takes a file without one -- no
  count distrusted for its size, the markers recognised either way -- said so.
  `--trusted-max` overrides it all.
* **The goniometer:** the rotations of `/entry/sample/depends_on`'s chain,
  innermost first, written as the file gives their vectors (not normalised, as
  dials.import writes them); the one whose values change the scan axis at angle
  0, the others at their values. More than one moving, or none, is a failure.
* **The scan:** one image a value of the scan axis, images 1 to n; the
  oscillation those values; exposure time `frame_time` and the epochs
  `frame_time` apart, both 0 without it, as dials.import gives them (October
  2026; before, `count_time` and 0, which differed from dials.import whenever
  a master had a count time and no frame time, as insulin's has).
* **Transformation vectors as the file gives them,** as dxtbx's nxmx reader
  takes them: a translation the value times the vector, a rotation the value
  times the vector as a rotation vector. NXtransformations says a vector should
  be of unit length, the magnitude in the value, and on a conforming file
  normalising changes nothing; but a beamline master put its module_offset as
  1 m along (0.15756, 0.16414, 0), and mxi, normalising, put the detector 4.4
  times too far off the beam where dials.import put it where it was meant
  (October 2026). The pixel directions are directions only, normalised as
  dxtbx's panel normalises them.
* **The image set:** an ImageSequence of the master, by absolute path, and a new
  identifier.

Every unit is read from its attribute -- lengths to mm, angles to degrees -- and
one missing is taken as mm or degrees and said.

## The older DECTRIS file writer's masters

Before NXmx, DECTRIS's own file writer wrote a "nearly NeXus" master -- Graeme's
example a 2023 Eiger 16M on firmware 1.6.6: no NXdetector_module, no depends_on
chains, no /entry/data/data, the geometry in its own fields and the frames in
/entry/data/data_000001 and on, each an external link to a data file. dials.import
reads them through dxtbx's FormatHDF5EigerNearlyNexus, whose EigerNXmxFixer
builds the missing structure; mxi_import recognises the same masters -- an entry
with no definition, a detector described as a DECTRIS Eiger, geometry/orientation
and geometry/translation present -- and makes the models by the fixer's rules,
so that the two agree:

* **The detector:** one panel, `/entry/instrument/detector/module`; its fast and
  slow directions geometry/orientation/value's two triples, its origin
  geometry/translation/distances, each with its z negated, as the fixer has it,
  "to align with Dectris/NeXus documentation", then into DIALS's frame as any
  NeXus vector. Pixel sizes from x_pixel_size and y_pixel_size; the size from the
  first data file, or detectorSpecific's x_ and y_pixels_in_detector if the data
  files are not beside the master. On Graeme's master the beam meets the panel at
  2054.14, 2236.73, its own beam_center_x and _y, which nothing read.
* **The goniometer:** omega about (-1, 0, 0) -- (0, 1, 0) on detector E-32-0105,
  as the fixer has it -- whatever the master's goniometer group says.
* **The scan:** from 0 in steps of omega_range_average rounded to 0.01 degree, one
  a frame of the linked data files (or nimages times ntrigger without them, said
  so). The master's omega_start is not used, as dials.import does not use it;
  said so.
* The rest -- the beam, the sensor, mu, the trusted range from the count cutoff --
  as for any master.

The frame reader follows the links in order (`docs/spots.md`).
`python/tests/test_nearly_nexus.py` plants such a series.

## Overrides

`--wavelength A`; `--distance MM`, moving the detector along its normal;
`--beam-centre X,Y`, pixels fast and slow, moving it in its plane so that the
beam meets it there; `--mu`; `--trusted-max`; `--image-range A,B`. Each override
used is said.

## Against dials.import

    dials.import master.nxs output.experiments=dials.expt
    mxi_import master.nxs
    mxeq compare-expt imported.rflx dials.expt     # mxeq reads either kind

prints each model's parts, "same" or the difference: origins in mm, axes in
degrees, numbers relatively. On insulin, against dials.import of the full 16M
master, the differences were the origin and image size -- a different file --
and the exposure time, 0.0026 s here where dials.import wrote 0, since settled:
mxi_import now takes frame_time, as dials.import does, and insulin's master has
none.

Two things learned from Diamond's Eiger masters. Their offsets come without
`offset_units`, in metres, and had been taken as millimetres -- the detector a
thousandth of the way out; now read in the transformation's units, as nxmx
reads them. And their count limit and bit depth are links into the `_meta.h5`
beside the master: without that file the links cannot be followed, and
mxi_import had taken them for absent and fallen back to 65534 -- itself the
bad-pixel marker, and a 16-bit limit wrongly applied to 32-bit data. Now it
says where a link leads, takes the lower of the two limits when they can be
read, and 2147483647 when neither can, as dxtbx does.

## Untested

A detector of several modules (each becomes a panel; nothing downstream has
been run on one); a moving detector; a beam direction or polarisation from the
file; scans about any axis but the outermost's; a real CdTe or GaAs master (mu
checked against cctbx's values on planted ones).
