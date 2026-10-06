# Seed divergence (disclosed)

The benchmark seed omits jason's avatar attachment so the fallback avatar path
is exercised; `/users/<token>/avatar` therefore serves a generated initials
SVG (image/svg+xml) rather than the reference's processed webp variant.  The
avatar workload row must be read with this representation difference in mind.

- seed: `/home/msaraiva/dev/mark/Proj/Misc/once-campfire-c/bench/seed/.seed/default`
- seed db sha256: `f0ed1d0f060e70f1540061f056b983807ab8c53245026414e40e847fb2b9e5b9`
- c revision: `7a4c24e877c1` (binary sha256 `d947f3674c80e563acb69a1be4cb0d38575f5d53881d4310808643da791bf645`)
