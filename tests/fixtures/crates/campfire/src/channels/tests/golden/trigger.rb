# Server-side events for recording reference.json (see ../golden.rs), run inside the recording
# instance: `parity/bin/reference runner --port N .../trigger.rb EVENT ID...`.
event, *ids = ARGV
ids = ids.map(&:to_i)

case event
when "unread"
  Message.find(ids[0]).send(:broadcast_unread_room)
when "remove_message"
  Message.find(ids[0]).broadcast_remove
when "revoke"
  Room.find(ids[0]).memberships.revoke_from(User.find(ids[1]))
when "deactivate"
  User.find(ids[0]).deactivate
else
  raise "unknown event #{event}"
end
