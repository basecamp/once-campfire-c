# Oracle for crates/campfire/src/integrations/web_push: run with
#   parity/bin/reference runner crates/campfire/src/integrations/testdata/oracle/web_push.rb
# It writes ../web_push_expected.json (the app logs to stdout).
require "web_push/notification"

receiver = OpenSSL::PKey::EC.generate("prime256v1")
p256dh = WebPush.encode64(receiver.public_key.to_bn.to_s(2))
auth = WebPush.encode64(Random.new.bytes(16))

notification = WebPush::Notification.new(
  title: "Designers <&> \"quotes\" é 😀", body: "Kevin: line\nbreak\ttab   \u001f / \\ ", path: "/rooms/1",
  badge: 3, endpoint: "https://fcm.googleapis.com/fcm/send/abc", endpoint_ip_resolver: -> { "142.250.185.206" },
  p256dh_key: p256dh, auth_key: auth)
message = notification.send(:encoded_message)
vapid = notification.send(:vapid_identification)

ciphertext = WebPush::Encryption.encrypt(message, p256dh, auth)

frozen = Time.at(1_700_000_000)
Time.singleton_class.prepend(Module.new { define_method(:now) { frozen } })
request = WebPush::Request.new(message: message,
  subscription: { endpoint: "https://fcm.googleapis.com/fcm/send/abc", keys: { p256dh: p256dh, auth: auth } },
  vapid: vapid, urgency: "high")
headers = request.headers
jwt = headers["Authorization"][/t=([^,]+)/, 1]
header_segment, payload_segment, _signature = jwt.split(".")

File.write(File.expand_path("../web_push_expected.json", __dir__), JSON.pretty_generate(
  receiver_private_key: WebPush.encode64(receiver.private_key.to_s(2)),
  p256dh: p256dh,
  auth: auth,
  message: message,
  ciphertext: WebPush.encode64(ciphertext),
  vapid_subject: vapid[:subject],
  now: frozen.to_i,
  headers: headers.except("Authorization").to_a,
  authorization_k: headers["Authorization"][/k=(.+)\z/, 1],
  jwt_header_segment: header_segment,
  jwt_payload_segment: payload_segment,
  test_notification_title: "Campfire Test"
))
