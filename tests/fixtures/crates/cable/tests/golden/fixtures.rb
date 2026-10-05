# Fixtures for recording tests/golden/reference.json: prints the session cookie and the values
# the script needs. Run with `parity/bin/reference runner --port N crates/cable/tests/golden/fixtures.rb`.
Account.first || Account.create!(name: "Campfire")
user = User.find_by(email_address: "cable@example.com") ||
  User.create!(name: "Cable Tester", email_address: "cable@example.com", password: "secret123456", role: :administrator)
other = User.find_by(email_address: "other@example.com") ||
  User.create!(name: "Other", email_address: "other@example.com", password: "secret123456")
room = Rooms::Open.find_by(name: "Golden") || Rooms::Open.create_for({ name: "Golden", creator: user }, users: [ user ])
closed = Rooms::Closed.find_by(name: "Private") || Rooms::Closed.create_for({ name: "Private", creator: other }, users: [ other ])
closed.memberships.revoke_from(user)
session = user.sessions.start!(user_agent: "golden", ip_address: "127.0.0.1")

request = ActionDispatch::Request.new(Rails.application.env_config.merge("HTTP_HOST" => "127.0.0.1", "rack.input" => StringIO.new))
request.cookie_jar.signed[:session_token] = session.token

puts JSON.generate(
  cookie: "session_token=#{CGI.escape(request.cookie_jar[:session_token])}",
  tokens: {
    "USER_ID" => user.id.to_s,
    "USER_NAME" => user.name,
    "ROOM_ID" => room.id.to_s,
    "CLOSED_ROOM_ID" => closed.id.to_s,
    "ROOMS_SIGNED" => Turbo::StreamsChannel.signed_stream_name(:rooms),
    "ROOM_MESSAGES_SIGNED" => Turbo::StreamsChannel.signed_stream_name([ room, :messages ])
  }
)
