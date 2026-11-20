# Clustering data sets by their intensities: mxi correlation

STATUS: built, 15 October 2026 -- the correlation matrix, the cosym
coordinates, OPTICS, each cluster as its own .rflx, and the report.

`mxi correlation` is to do what dials.correlation_matrix does (Thompson et al.
2025, Acta Cryst. D81, 278-290): from a scaled file whose lattices are each a
data set -- many crystals, as in Graeme's COWS_PIGS_PEOPLE tutorial -- find
the groups of data sets that belong together, by their intensities, where
unit cells cannot tell them apart.

## Stage 1: the correlation matrix (built)

Each data set is a lattice experiment. Its scaled reflections -- scaled, not
outliers, partiality at least 0.4 (`--min-partiality`), intensity.scale.value
and its variance -- are merged by symmetry-unique index, Friedel mates
together, as cctbx's merge_equivalents by default: one observation as it is;
several, the inverse-variance weighted mean, its sigma sqrt(max(V_w / n,
1 / sum w)), V_w GSL's weighted variance (use_internal_variance, cctbx's
default). Every pair's correlation over the reflections both have is DIALS's
weighted_cchalf: each reflection weighted 1 / (s_i^2 + s_j^2), normalised; r
the weighted covariance over the weighted standard deviations; Kish's
effective sample size n_eff = 1 / sum w^2. The cosym target's weights are
DIALS's default `count` weights: n_eff, rescaled to sum to the number of pairs
that have one; none on the diagonal or for a pair with fewer than three
reflections in common (`--min-pairs`). Pairs a row a thread.

Checked: an independent Python implementation -- gemmi's asymmetric unit,
cctbx's merging formula and DIALS's correlation written afresh -- gives the
same merged counts and every r to 3e-15, on insulin as four sweeps. Insulin's
four sweeps of one crystal: r 0.993 to 0.994; the two insulin crystals of
ins2_27: 0.980. Unit tests: the pair correlation against the formula written
out, the weights' rescaling, two planted groups told apart
(tests/test_correlation.cc).

## Stage 2: the coordinates (built)

Minimise f = 1/2 sum_ij w_ij (r_ij - x_i . x_j)^2 over the whole matrix, its
gradient -2 X (W.R - W.(X^T X)), for coordinates x_i in d dimensions, by
L-BFGS: ten pairs remembered, a backtracking line search, and as the starting
inverse Hessian at every iteration 1 / the curvature, c_ki = 2 sum_j x_kj^2
w_ji, as DIALS's minimiser (scitbx's lbfgs, diag_mode "always"); stopped at 100
iterations or |g| under 1e-5 of max(1, |x|). The start is DIALS's exactly:
numpy's np.random.seed(230); np.random.rand(...), drawn the same way from the
Mersenne twister.

The dimension as dials.correlation_matrix chooses it -- not by DIALS #3248's
projection, which only dials.cosym uses (`--by-projection` has it): a
minimisation at each dimension from 1 to min(n, 50), each scored with outlier
rejection (the elements w (r - x.x)^2 below q2 + (q3 - q1), halved), and the
elbow of that curve, as distl's resolution method 1, at least 2. Then the
minimisation at that dimension, and the coordinates' principal components,
centred as sklearn's PCA, with the variance each explains.

Two mistakes on the way, both in reading DIALS: its scan's
`min(20, max_calls) if max_calls else max_calls` limits a dimension's
minimisation to 20 calls only where max_calls is set, which by default it is
not -- read as 20, the curve was rough and the elbow 14 or 16 of 18 for three
planted groups; and without the curvatures a minimisation got almost nowhere
in those calls. Now three planted groups of six: the curve 0.0072, 0.0044,
2.7e-7 and flat, the elbow 3, the variance 0.51, 0.49, 0.00, each group its own
place. Checked: the start numpy's to the bit; the gradient finite
differences'; the minima scipy's L-BFGS-B's from the same start, 0.00973797
against mxi's 0.00973796 in 2 dimensions, 6.78e-7 against 6.72e-7 in 3.
Insulin's four sweeps of one crystal: 2 dimensions, the first component
everything -- one group.

## Stage 3: OPTICS (built)

OPTICS as scikit-learn's, ported from its source (sklearn/cluster/_optics.py),
on the coordinates as minimised (not their principal components), Euclidean:
core distances to the min_samples-th nearest point, itself the first, infinite
beyond max_eps; the ordering from the lowest index, each next the unprocessed
point of least reachability, ties to the lowest index; reachability
max(distance, core distance) to the points within max_eps; both rounded as
np.around to 15 decimals. Clusters by xi (0.05): steep areas,
_extend_region, _update_filter_sdas, predecessor correction, the minimum
cluster size min_samples; labels -1 for noise. The silhouette as
scikit-learn's silhouette_score.

How DIALS uses it, from its source (analysis.py, cluster_cosine_coords and
optimise_clustering), with its defaults: max_eps 0.5 (`max_distance`),
min_points_buffer 0.5. Under five data sets, min_samples n. Otherwise the
paper's initial guess max(5, int(n / d x 0.5)), then -- `optimise_input`, the
default -- min_samples searched from 5 to min(n, 50), each scored
silhouette x (1 - f^1.5), f the fraction labelled noise, -1 where there is one
cluster or none. DIALS searches that range by optuna, 20 random trials and 30
Bayesian; mxi scores every value, which is deterministic and finds the best,
a tie to the initial guess (DIALS's first trial) then the smallest. The best
below zero, DIALS's single-cluster heuristic, ported line for line, Python's
truthiness and all; no trial at all, OPTICS with no max_eps.

Checked: four point sets -- three blobs with noise, with and without max_eps,
a grid of exactly tied distances, clusters of differing density -- against
scikit-learn 1.8's OPTICS: every ordering position, predecessor and label the
same, every distance to 1e-12, the silhouette too
(tests/tools/make_optics_cases.py writes tests/optics_cases.hh). Three planted
groups of eight through all three stages: three clusters, each a group, no
outliers. Insulin's four sweeps of one crystal: one cluster.

`mxi correlation` prints the clusters, each with its data sets by experiment,
and the outliers; `--xi`, `--max-distance`, `--min-samples` (no search);
`--json` adds the labels, the reachability plot in order, and the search's
scores.

## Against dials.correlation_matrix (15 October)

Graeme's 100 K bovine, porcine and human insulin, 36 crystals, 12 of each
(the paper's section 3.2.1), all from dials.symmetry; then either dials.scale
or mxi convert and mxi_scale. On mxi's scaling, mxi correlation: 3
dimensions, min_samples 6, the three species exactly, silhouette 0.837. On
DIALS's scaling, dials.correlation_matrix: 3 dimensions, min_samples 6, the
same three clusters, silhouette 0.833 -- every min_samples from 6 to 12 giving
the same, 5 making six data sets noise, 13 and up one cluster, as mxi's
exhaustive search finds too.

The matrices differ in detail. Prototyped on DIALS's scaled data (in Python,
not committed): mxi's method gives every r within a median 0.0034 of DIALS's,
and through mxi's stages 2 and 3 the functional 0.258, 0.045, 0.0019 for
DIALS's 0.314, 0.050, 0.0016, the same dimension and clusters. DIALS's
preparation, from its source: ε = 1 under the lattice's Patterson group (I m
-3 m here), a common resolution limit from its Resolutionizer (1.27 A), and
each data set normalised by mmtbx's ML isotropic Wilson fit, I x exp(B d*^2 /
4 - p_scale). The first two barely change the matrix. The normalisation could
not be reproduced: with DIALS's own printed B values it made the agreement
worse (median 0.019), as did every variant tried, though the remaining
differences track |B_a - B_b| (correlation 0.59); and dials.correlation_matrix
with normalisation=None gave correlations higher still (0.879 to 0.996, the
functional 0.184, 0.047, 0.0012), no closer to mxi's at any resolution limit.
So some step of DIALS's preparation is not yet understood. Not pursued: the
clusters and their choice agree. To settle it, dump DIALS's prepared
intensities (CosymAnalysis.intensities and dataset_ids, after __init__) and
compare data set by data set.

**Normalised since 20 October.** Each data set's merged intensities are now
divided by a Wilson model fitted to it by maximum likelihood before their
correlations -- the normalisation mxi symmetry uses (laue.hh: dials.symmetry's
ml_aniso, a light atom's scattering left in), not mmtbx's isotropic fit above;
`--no-normalise` for none. Found on the twelve cows from mxi's own pipeline:
unnormalised, the data set with the most extreme fall-off correlated worst
(0.863 to 0.940) and fell out as an outlier, at four dimensions, where
dials.correlation_matrix on the same scaled data found one cluster of twelve.
Normalised: one cluster of twelve, three dimensions, as DIALS; that data set's
correlations 0.924 and up. The 36 crystals: three clusters of twelve, as
before. The B values printed -- the isotropic equivalent, 2 tr(beta G) / 3,
of the anisotropic fit -- are 16-20 A^2 on the cows where DIALS prints 11-16,
in its own convention (exp(B d*^2 / 4)), its ranking not the same: item 76.

mxi's own scaling of these crystals gives lower correlations than
dials.scale's (0.72 against 0.84 at the least): a thing to look at in
multi-crystal scaling, apart from the clustering.

## Stage 4: output

**The clusters' files (built).** `--write-clusters` writes each cluster as
its own .rflx, `cluster_0.rflx` and on (`--prefix`): its data sets, any
crystal-less sweep experiment of theirs (the same image set and scan, so that
mxi's own files keep their sweeps), and their reflections, the ids renumbered
and the identifier map to match (keep_experiments, which dials_form now
uses); and its own crystals' history -- each step's record in the input's
summary cut to the cluster's sweeps and experiments, renumbered as the
cluster's file numbers them, figures only the whole run had named as left
out -- with mxi correlation's own record after it (src/summary_cut.cc), so
that a cluster's report is its crystals'. On Graeme's 36 crystals: three
files of 12, about 420000 rows each; the second scaled on its own by
mxi scale, completeness 90.1 per cent and multiplicity 9.8, as
dials.correlation_matrix reports that cluster (90.1, 9.74); R merge 0.146
against 0.225 with the three species together.

**The report (built).** mxi correlation writes its own HTML, as
dials.correlation_matrix does -- mxi_correlation.html, `--html` to name it;
Plotly, as mxi report: the clusters' table; the correlation matrix and the
cosine-angle matrix between the coordinates, each a heat map in its Ward
dendrogram's order with the dendrogram above (distances 1 - value, as DIALS);
the reachability plot in OPTICS's order, by cluster; the coordinates' first
two principal components, by cluster; the dimension functional, the chosen
dimension marked. Ward linkage as scipy's (the Lance-Williams update on
distances) and the dendrogram's leaf order and brackets as scipy's dendrogram:
on nine random points every merge to 1e-12 and the same leaves. On Graeme's 36
crystals both dendrograms put each species together: 24-35, 0-11, 12-23.
