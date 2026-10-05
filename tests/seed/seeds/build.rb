# bin/rails runner parity/seeds/build.rb NAME
#
# Builds seed NAME into this instance's storage (storage/db + storage/files) and writes its labels
# to storage/db/labels.json, which parity/bin/seed moves next to the seed. Run it through
# `parity/bin/seed build`, not directly.
require_relative "lib/seed"

name = ARGV.fetch(0) { abort "usage: build.rb NAME" }
Parity::Seed.build(name, labels_path: Rails.root.join("storage/db/labels.json").to_s)
