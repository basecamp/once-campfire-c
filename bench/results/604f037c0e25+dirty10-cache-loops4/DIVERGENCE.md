# Seed divergence (disclosed)

The benchmark seed omits jason's avatar attachment so the fallback avatar path
is exercised; `/users/<token>/avatar` therefore serves a generated initials
SVG (image/svg+xml) rather than the reference's processed webp variant.  The
avatar workload row must be read with this representation difference in mind.

- seed: `/home/msaraiva/dev/mark/Proj/Misc/once-campfire-c/bench/seed/.seed/default`
- seed db sha256: `a763014643b8e26d38f153e98647500e42ef2c305fdbaf24145fccdbcb9ae030`
- c revision: `604f037c0e25+dirty10` (binary sha256 `48f93d27594a4c65bdcab845993b31ef0f526d9d646aaf3831d0a6e168b01f6e`)
