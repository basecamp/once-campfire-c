Diagnostic scaffolding run (B01b prep, 1 rep, HTTP_SECS=2, HTTP_CONCS="1 16",
SUITES=http). Not headline measurements: the ~41 ms keep-alive stall and the
gzip asymmetry documented in docs/devel/evidence/B01b-prep.md apply. Kept as
raw evidence for the cache-arm invocation.

The seed was later extended with Rails migration metadata (db sha256
a763014643b8e26d38f153e98647500e42ef2c305fdbaf24145fccdbcb9ae030); these raw
files keep the seed hash the run actually used (f0ed1d0f...).
