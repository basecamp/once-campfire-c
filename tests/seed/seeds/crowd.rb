# crowd: default plus 520 more members, past every list threshold: the room user filter (> 20),
# the direct-ping placeholders (20), and the account user list's second page (500 per page).
based_on "default"

FIRST = %w[ Ada Ben Cleo Dev Eli Fay Gus Hana Ivo Jun Kai Lea Milo Nia Oto Pia Quin Rex Sol Tia ]
LAST  = %w[ Abbott Brooks Castro Diaz Evans Fox Gray Hale Ito Jones Khan Lund Moss Nash Ortiz Park
            Quinn Reyes Stone Toth Ueda Voss Wolfe Xu Young Zhou ]

digest = User.first.password_digest
open_room_ids = Rooms::Open.pluck(:id)

rows = 520.times.map do |i|
  time = NOW - 20.days + i.minutes
  { name: "#{FIRST[i % FIRST.size]} #{LAST[i / FIRST.size]}", email_address: format("member%03d@example.com", i + 1),
    password_digest: digest, role: User.roles[:member], status: User.statuses[:active], created_at: time, updated_at: time }
end
at NOW - 20.days
User.insert_all!(rows)

# What User#grant_membership_to_open_rooms does for each of them
User.where(email_address: rows.map { |row| row[:email_address] }).find_each do |member|
  Membership.insert_all!(open_room_ids.map { |room_id|
    { room_id: room_id, user_id: member.id, involvement: "mentions", created_at: member.created_at, updated_at: member.created_at } })
end
label :users, :crowd_first, User.find_by!(email_address: "member001@example.com")
label :users, :crowd_last, User.find_by!(email_address: "member520@example.com")
