# Current production comparison: shared response contracts and exact persistent-write audit.
require "rbconfig"
rust = ENV.fetch("RUST_ROOT", File.expand_path("../once-campfire-rust", File.expand_path("..", __dir__)))
exec RbConfig.ruby, File.join(rust, "bench/compare.rb"), "--apps", "c", *ARGV
