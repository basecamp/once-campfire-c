//! Channel tests over the reference fixtures, driven through a real WebSocket client (the way
//! reference/test/channels drives them through ActionCable::Channel::TestCase), plus a replay of
//! frames recorded from the reference app (`golden`).

mod broadcasts_test;
mod channels_test;
mod golden;
mod revocation_test;
pub mod support;
