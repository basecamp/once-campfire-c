# Seed divergence (disclosed)

The benchmark seed omits jason's avatar attachment so the fallback avatar path
is exercised; `/users/<token>/avatar` therefore serves a generated initials
SVG (image/svg+xml) rather than the reference's processed webp variant.  The
avatar workload row must be read with this representation difference in mind.

- seed: `/home/msaraiva/dev/mark/Proj/Misc/once-campfire-c/bench/seed/.seed/default`
- seed db sha256: `a763014643b8e26d38f153e98647500e42ef2c305fdbaf24145fccdbcb9ae030`
- c revision: `ec73dd1` (binary sha256 `2e3c17d1d0fdea9ccf6888be8311a6cc9bb1d459d93e3852da86700fcad0ab3a`)
