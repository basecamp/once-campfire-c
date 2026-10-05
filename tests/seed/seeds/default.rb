# default: the test fixtures plus a scenario that puts every screen state within reach.
# See parity/seeds/README.md for the tour and the labels screens can use.

load_fixtures

# -- Fixture rows, spread out in time (fixture file order) ----------------------------------------

spread_fixture_timestamps :accounts,           from: NOW - 60.days
spread_fixture_timestamps :users,              from: NOW - 59.days, step: 1.hour
spread_fixture_timestamps :rooms,              from: NOW - 58.days, step: 1.hour   # pets first: Room.original
spread_fixture_timestamps :memberships,        from: NOW - 57.days
spread_fixture_timestamps :webhooks,           from: NOW - 56.days
spread_fixture_timestamps :push_subscriptions, from: NOW - 55.days
spread_fixture_timestamps :sessions,           from: NOW - 54.days
spread_fixture_timestamps :searches,           from: NOW - 3.days

# Fixture messages keep their relative times (now absolute); nothing has edited them.
Message.update_all("updated_at = created_at")
ActionText::RichText.where(record_type: "Message").find_each do |rich_text|
  time = Message.find(rich_text.record_id).created_at
  rich_text.update_columns(created_at: time, updated_at: time)
end
fixture_ids(:boosts).each_with_index do |id, index|
  boost = Boost.find(id)
  time = boost.message.created_at + 1.minute + index.seconds
  boost.update_columns(created_at: time, updated_at: time)
end

index_messages_for_search

# Randomized by the fixture ERB; the bot pages show them.
user(:bender).update_columns(bot_token: "BenderBot123")

david, jason, jz, kevin, bender = %i[ david jason jz kevin bender ].map { |name| user(name) }

# -- More people ---------------------------------------------------------------------------------

at NOW - 40.days
rita = label :users, :rita, User.create!(name: "Rita Lopez", email_address: "rita@37signals.com", password: "secret123456", bio: "Support")
at NOW - 39.days
mallory = label :users, :mallory, User.create!(name: "Mallory Banned", email_address: "mallory@example.com", password: "secret123456")
at NOW - 38.days
deploy_bot = label :users, :deploy_bot, User.create_bot!(name: "Deploy Bot")
deploy_bot.update_columns(bot_token: "DeployBot456")
at NOW - 37.days
old_bot = label :users, :old_bot, User.create_bot!(name: "Old Bot", webhook_url: "https://example.com/old-bot")
old_bot.update_columns(bot_token: "OldBot789abc")

at NOW - 36.days
attach_avatar jason, file("moon.jpg", "image/jpeg")
at NOW - 36.days + 1.minute
attach_avatar deploy_bot, file("moon.jpg", "image/jpeg")

# -- More rooms ----------------------------------------------------------------------------------

at NOW - 35.days
quiet = label :rooms, :quiet, Rooms::Closed.create_for({ name: "Quiet Corner", creator: kevin }, users: [ kevin, david ])
at NOW - 34.days
archive = label :rooms, :archive, Rooms::Open.create_for({ name: "Archive", creator: david }, users: User.active)
at NOW - 33.days
broken = label :rooms, :broken, Rooms::Closed.create_for({ name: "Broken", creator: david }, users: [ david ])
at NOW - 32.days
group = label :rooms, :group_direct, Rooms::Direct.create_for({ creator: david }, users: [ david, jason, jz, kevin ])

designers, watercooler = room(:designers), room(:watercooler)

at NOW - 31.days
designers.memberships.grant_to [ rita, deploy_bot ]
at NOW - 31.days + 1.minute
designers.memberships.grant_to [ mallory ]

# -- Designers: one message of every presentation type, on the day before the fixtures' ------------

day = NOW.beginning_of_day - 1.day + 9.hours # 2026-03-01 09:00 UTC
t = ->(minutes) { day + minutes.minutes }

post designers, jason, t[0], "<p>Morning! Anyone around?</p>", as: :plain
post designers, jason, t[2], "<p>Coffee first, then the launch plan.</p>", as: :threaded

post designers, jz, t[10], <<~HTML.squish, as: :long
  <p>Here's the plan for the <strong>spring launch</strong>, as discussed on <em>Friday</em>:</p>
  <ol><li>Finish the onboarding copy</li><li>Record the walkthrough video</li><li>Ship it on <code>2026-03-16</code></li></ol>
  <blockquote>Simplicity is prerequisite for reliability.</blockquote>
  <p>Everything else can wait. The details are on <a href="https://example.com/launch">the launch page</a>.</p>
  <ul><li>Design review on Tuesday</li><li>Copy review on Wednesday</li></ul>
  <h2>Open questions</h2>
  <p>We still need to decide whether the walkthrough video should autoplay, how long the free trial runs,
  and who owns the announcement post. None of these block the build, but all of them block the launch, so
  let's get answers by Thursday. If you have opinions, now is the time to share them, preferably with
  reasons, alternatives, and a rough idea of what each option would cost us in time and attention.</p>
HTML

post designers, kevin, t[20], "<p>🎉🔥</p>", as: :emoji
post designers, david, t[30], %(<p>The helper we talked about:</p><pre data-language="ruby">def greet(name)<br>  "Hello, \#{name}!"<br>end</pre>), as: :code
post designers, jz, t[36], %(<pre>plain preformatted\n  text without a language</pre>), as: :code_plain
post designers, jason, t[40], <<~HTML.squish, as: :table
  <figure class="lexxy-content__table-wrapper"><table><tbody>
  <tr><th><p>Name</p></th><th><p>Role</p></th><th><p>Points</p></th></tr>
  <tr><td><p>Jason</p></td><td><p>Design</p></td><td><p>10</p></td></tr>
  <tr><td><p>Kevin</p></td><td><p>Programming</p></td><td><p>7</p></td></tr>
  </tbody></table></figure>
HTML
post designers, jz, t[50], "<p>Hello <s>struck</s> <u>underlined</u> <mark>marked</mark> and <strong>bold</strong> <em>italic</em> <del>deleted</del>.</p>", as: :formatting
post designers, david, t[60], "<p>Hey #{mention(kevin)}, can you check the table above?</p>", as: :mention
post designers, jason, t[70], "<p>placeholder</p>", as: :mention_marshal
store_raw_body message(:mention_marshal), "<div>Thanks #{marshal_era_mention(david)}, that was from the old days.</div>"
post designers, kevin, t[80], "/play 56k", as: :sound_image
post designers, jz, t[90], "/play bell", as: :sound_text

post designers, david, t[100], <<~HTML.squish, as: :opengraph
  <p>This is what we're up against: <a href="https://example.com/cookies">https://example.com/cookies</a></p>
  #{opengraph_embed(href: "https://example.com/cookies", title: "Free cookies for everyone", description: "A bakery gives away a cookie to anyone who asks nicely, every day at noon.", image: "https://example.com/og/cookies.jpg")}
HTML
post designers, jason, t[110], <<~HTML.squish, as: :solo_unfurl
  <p><a href="https://example.com/launch">https://example.com/launch</a></p>
  #{opengraph_embed(href: "https://example.com/launch", title: "Spring launch", description: "Everything you need to know about the spring launch.", image: "https://example.com/og/launch.jpg")}
HTML
post designers, kevin, t[120], "<p>placeholder</p>", as: :opengraph_trix_tweet
store_raw_body message(:opengraph_trix_tweet), %(<div>https://twitter.com/37signals/status/1750290547908952568<action-text-attachment content-type="application/vnd.actiontext.opengraph-embed" url="https://pbs.twimg.com/profile_images/1671940407633010689/avatar_200x200.jpg" href="https://twitter.com/37signals/status/1750290547908952568" filename="37signals (@37signals)" caption="We're back up on all apps, everyone."></action-text-attachment></div>)
post designers, jz, t[125], "<p>Docs are at https://example.com/docs and the old ones at http://example.org/docs</p>", as: :autolink

post designers, kevin, t[130], nil, attachment: file("moon.jpg", "image/jpeg"), as: :image
post designers, jason, t[140], nil, attachment: file("black_hole.jpg", "image/jpeg"), as: :image_large
post designers, jz, t[150], nil, attachment: file("alpha-centuri.mov", "video/quicktime"), as: :video
post designers, jason, t[160], nil, attachment: text_file("launch-notes.txt", "Launch notes\n\n1. Copy\n2. Video\n3. Ship\n"), as: :file
post designers, kevin, t[170], nil, attachment: file("pixel.bmp", "image/bmp"), as: :file_bmp

post designers, deploy_bot, t[180], "<p>Deployed campfire@4f2c1e9 to production ✅</p>", as: :bot
post designers, rita, t[190], "<p>I'll be out of office next week.</p>", as: :by_deactivated_user

post designers, david, t[200], "<p>Launch is on the 15th.</p>", as: :edited
at t[203]
message(:edited).update!(body: "<p>Launch is on the 16th.</p>")

post designers, jz, t[210], "<p>Nobody boosted this one.</p>", as: :unboosted
post designers, kevin, t[220], "<p>Big news: the beta is open!</p>", as: :boosted_many
post designers, jason, t[230], "<p>Lunch is here.</p>", as: :boosted_one
post designers, jz, t[240], "<p>Who's reviewing the copy?</p>", as: :boosted_by_david

boosted_many = message(:boosted_many)
boost boosted_many, david,  "👍", t[221]
boost boosted_many, jz,     "🎉", t[222]
boost boosted_many, jason,  "Finally!", t[223]
boost boosted_many, kevin,  "❤️", t[224]
boost boosted_many, rita,   "😂", t[225]
boost boosted_many, david,  "+1", t[226]
label :boosts, :jz_on_boosted_one, boost(message(:boosted_one), jz, "🍕", t[231])
label :boosts, :david_on_boosted_by_david, boost(message(:boosted_by_david), david, "Me!", t[241])
boost message(:image), jason, "🌕", t[131]

# -- Watercooler: the busy room ------------------------------------------------------------------
# 120 older messages before the fixtures' 10, so it pages (40 per page) and a deep link into the
# middle loads the full page_around window of 81.

lines = [
  "Did anyone see the game last night?", "Coffee machine is fixed!", "I'm heading out for lunch.",
  "New plants in the kitchen.", "Who took my stapler?", "Friday demo is at 3pm.",
  "The wifi is flaky again.", "Congrats on the launch!", "Anyone up for a walk?", "Back in 5."
]
busy_start = NOW.beginning_of_day - 2.days + 8.hours # 2026-02-28 08:00 UTC
1.upto(120) do |i|
  author = [ david, jason, david, jason, bender ][i % 5]
  post watercooler, author, busy_start + (i * 7).minutes, "<p>#{format("%03d", i)}. #{lines[i % lines.size]}</p>", as: format("busy_%03d", i)
end
post watercooler, bender, NOW - 20.minutes, "<p>Build 1043 passed.</p>", as: :bot_in_watercooler

# -- Direct rooms --------------------------------------------------------------------------------

david_and_jason, david_and_kevin, bender_and_kevin = room(:david_and_jason), room(:david_and_kevin), room(:bender_and_kevin)

post david_and_jason, jason, NOW - 5.hours, "<p>Got a minute?</p>", as: :direct_first
post david_and_jason, david, NOW - 5.hours + 2.minutes, "<p>Sure, what's up?</p>"
post david_and_jason, jason, NOW - 5.hours + 3.minutes, "<p>The pricing page. Let's talk after lunch.</p>"
post bender_and_kevin, bender, NOW - 4.hours, "<p>Your nightly report is ready.</p>"
post group, jz, NOW - 3.hours, "<p>Group ping: who's in for Thursday?</p>", as: :group_direct_first
post david_and_kevin, kevin, NOW - 2.hours, "<p>Can you approve my PR?</p>", as: :direct_unread

# -- The unrenderable message: its author row is gone ---------------------------------------------

at NOW - 6.hours
ActiveRecord::Base.connection.disable_referential_integrity do
  broken_message = post(broken, david, NOW - 6.hours, "<p>This message lost its author.</p>", as: :unrenderable)
  broken_message.update_columns(creator_id: 999_999)
end

# -- Settings and people states -------------------------------------------------------------------

at NOW - 30.days
rita.deactivate
rita.update_columns(email_address: "rita-deactivated-00000000-0000-4000-8000-000000000000@37signals.com")
at NOW - 29.days
old_bot.deactivate
old_bot.update_columns(email_address: nil)
at NOW - 28.days
mallory.update!(status: :banned)
label :bans, :mallory, mallory.bans.create!(ip_address: "203.0.113.9")

# Someone in no rooms at all sees the welcome page
at NOW - 27.days
loner = label :users, :loner, User.create!(name: "Lonely Lou", email_address: "lou@37signals.com", password: "secret123456")
loner.memberships.delete_all

# Involvement for David covers every bell state
membership(:designers, :david).update_columns(involvement: "mentions")
membership(:pets, :david).update_columns(involvement: "everything")
membership(:watercooler, :david).update_columns(involvement: "everything")
membership(:hq, :david).update_columns(involvement: "nothing")
membership(:archive, :david).update_columns(involvement: "invisible")
membership(:david_and_kevin, :david).update_columns(involvement: "nothing")

# Push subscriptions (inserted like fixtures: validation resolves the endpoint host in DNS)
at NOW - 20.days
Push::Subscription.insert_all!([
  { user_id: david.id, endpoint: "https://updates.push.services.mozilla.com/wpush/v2/parity-firefox",
    p256dh_key: "BFirefoxKeyRIXcMgkdjhRnFZaYjjGvo00dydRQbCpQTuXFjLaCPSE7ofxi19awgGc3Doqa1RmYQqsbQDfQTifFZgc",
    auth_key: "firefoxAuthKeyAAAAAAAAA", user_agent: "Mozilla/5.0 (Macintosh; Intel Mac OS X 14.3; rv:124.0) Gecko/20100101 Firefox/124.0",
    created_at: NOW - 20.days, updated_at: NOW - 20.days }
])
label :push_subscriptions, :david_firefox, Push::Subscription.find_by!(user: david, endpoint: "https://updates.push.services.mozilla.com/wpush/v2/parity-firefox")

# Recent searches (fixture: david "pizza", 3 days ago)
at(NOW - 2.days) { david.searches.record("Borgias") }
at(NOW - 1.day) { david.searches.record("cuckoo") }

# -- Final bookkeeping ----------------------------------------------------------------------------

# Room#updated_at is its last message's creation (belongs_to :room, touch: true), which the fixture
# messages never touched.
Room.find_each do |room|
  if (last = room.messages.maximum(:created_at)) && last > room.updated_at
    room.update_columns(updated_at: last)
  end
end

# Nobody is connected; unread flags are exactly these.
at NOW
Membership.update_all(unread_at: nil, connected_at: nil, connections: 0)
unread! :watercooler, :david, message(:bot_in_watercooler).created_at
unread! :david_and_kevin, :david, message(:direct_unread).created_at
unread! :group_direct, :david, message(:group_direct_first).created_at
unread! :designers, :kevin, message(:boosted_by_david).created_at

# Strings screens and tools need
label :transfers, :david, transfer_id_at(david, NOW)                    # valid until NOW + 4h
label :transfers, :david_expired, transfer_id_at(david, NOW - 5.hours) # expired at NOW - 1h
label :transfers, :kevin, transfer_id_at(kevin, NOW)
label :bot_keys, :bender, bender.reload.bot_key
label :bot_keys, :deploy_bot, deploy_bot.reload.bot_key
label :join_codes, :signal, Account.first.join_code
label :avatar_tokens, :david, david.avatar_token
label :avatar_tokens, :jason, jason.avatar_token
label :passwords, :all, "secret123456"
label :ips, :banned, "203.0.113.9"

# Sign-in emails for every labeled person, so screens can sign in `as:` a label
labels.select { |key, _| key.start_with?("users.") }.each do |key, id|
  email = User.find(id).email_address
  label :emails, key.delete_prefix("users."), email if email
end
