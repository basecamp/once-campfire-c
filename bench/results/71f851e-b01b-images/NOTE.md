# Comparison-image validation (B01b prep)

Rails reference image booted the way bench/run starts it (host network, CPUs
8-11, seed volumes, parity/reference.env) against the imported seed; the
pinned preflight passed. Files here are boot/preflight/build evidence, not
benchmark results. Provenance: docs/devel/evidence/B01b-images.md.

- rails-boot.log: container stdout/stderr from the validation boot
- rails-preflight.json: pinned validate.py preflight output
- rails-build.log: full docker build log (BUILD_EXIT=0)
