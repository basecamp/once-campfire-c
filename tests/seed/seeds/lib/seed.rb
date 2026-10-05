# The seed builder's toolkit. Run by parity/seeds/build.rb inside the reference app
# (`bin/rails runner`, production env) on a freshly prepared, empty database.
#
# A seed is a Ruby file in parity/seeds/ evaluated in the context of a Parity::Seed. Seeds build on
# each other with `based_on "default"`. Everything goes through the reference app's own models, so
# callbacks, rich text canonicalization, Active Storage analysis and variants are Ruby's.
#
# Determinism rules:
#   * The clock is stubbed for the whole build. Every write happens at an explicit instant (`at`),
#     and no two rows that are ordered by time share a timestamp.
#   * Things Rails would randomize and the UI shows (client_message_id, bot tokens, the deactivated
#     email suffix) are set explicitly.
#   * Background jobs are discarded (push, webhooks, banned-content removal): the seed is the state
#     the app would be in if they had nowhere to deliver to.
require "active_support/testing/time_helpers"
require "active_support/core_ext/digest/uuid"
require "json"

module Parity
  class Seed
    include ActiveSupport::Testing::TimeHelpers

    # The seed clock. Visual scenarios freeze the server (libfaketime / Clock) and the browser
    # (page.clock.setFixedTime) at this instant.
    NOW = Time.utc(2026, 3, 2, 16, 0, 0)

    SEEDS = File.expand_path("..", __dir__)
    FIXTURES = Rails.root.join("test/fixtures")
    FILES = FIXTURES.join("files")

    attr_reader :labels

    def self.build(name, labels_path:)
      new.build(name, labels_path: labels_path)
    end

    def initialize
      @labels = { "clock.now" => NOW.iso8601 }
      @loaded = []
      @client_message_ids = 0
    end

    def build(name, labels_path:)
      ActiveJob::Base.queue_adapter = :test
      deterministic_blob_keys
      based_on(name)
      at NOW
      settle_sql_timestamps
      pin_nondeterministic_columns
      travel_back
      checkpoint
      File.write(labels_path, JSON.pretty_generate(labels.sort.to_h) + "\n")
    end

    # Evaluate another seed file first (once).
    def based_on(name)
      name = name.to_s
      return if @loaded.include?(name)
      @loaded << name
      path = File.join(SEEDS, "#{name}.rb")
      raise ArgumentError, "no seed named #{name} (#{path})" unless File.exist?(path)
      instance_eval File.read(path), path
    end

    # -- Clock --------------------------------------------------------------------------------

    def at(time)
      settle_sql_timestamps
      travel_to time
      @now = time
      block_given? ? yield : time
    end

    # insert_all (Room#memberships.grant_to, User#grant_membership_to_open_rooms) and schema setup
    # stamp rows with SQLite's CURRENT_TIMESTAMP, which travel_to doesn't reach. Give those rows the
    # instant they were written at in seed time.
    def settle_sql_timestamps
      return unless @now
      cutoff = NOW + 1.day # seed time never passes NOW
      Membership.where(created_at: cutoff..).update_all(created_at: @now, updated_at: @now)
      Membership.where(updated_at: cutoff..).update_all(updated_at: @now)
    end

    # -- Labels -------------------------------------------------------------------------------

    def label(table, name, value)
      key = "#{table}.#{name}"
      raise ArgumentError, "label #{key} already taken" if labels.key?(key)
      labels[key] = value.respond_to?(:id) ? value.id : value
      value
    end

    def id_for(table, name)
      labels.fetch("#{table}.#{name}") { raise KeyError, "no label #{table}.#{name}" }
    end

    def user(name) = User.find(id_for(:users, name))
    def room(name) = Room.find(id_for(:rooms, name))
    def message(name) = Message.find(id_for(:messages, name))

    # -- Fixtures -----------------------------------------------------------------------------

    # Load reference/test/fixtures exactly as the test suite does (`fixtures :all`), with the clock at
    # NOW so relative times ("1.hour.ago") become absolute, then record every fixture label.
    def load_fixtures
      names = Dir[FIXTURES.join("**/*.yml").to_s].sort.map { |f| f.delete_prefix("#{FIXTURES}/").delete_suffix(".yml") }
      at NOW
      ActiveRecord::FixtureSet.create_fixtures(FIXTURES.to_s, names).each do |set|
        set.fixtures.each_key do |fixture|
          label set.table_name, fixture, ActiveRecord::FixtureSet.identify(fixture)
        end
      end
    end

    # Fixtures without explicit timestamps all get the same `now`. Spread each table's rows out,
    # in fixture file order, so every time-ordered query has exactly one answer.
    def spread_fixture_timestamps(table, from:, step: 1.minute)
      model = model_for_table(table)
      fixture_ids(table).each_with_index do |id, index|
        time = from + index * step
        model.where(id: id).update_all(created_at: time, updated_at: time)
      end
    end

    def fixture_ids(table)
      labels.select { |key, _| key.start_with?("#{table}.") }.values
    end

    def model_for_table(table)
      ActiveRecord::Base.descendants.find { |model| !model.abstract_class? && model.table_name == table.to_s && model.base_class == model } ||
        raise(ArgumentError, "no model for #{table}")
    end

    # Fixtures bypass callbacks, so fixture messages aren't in the search index.
    def index_messages_for_search
      Message.find_each { |message| message.send(:create_in_index) }
    end

    # -- Content ------------------------------------------------------------------------------

    # A deterministic stand-in for the UUID the composer generates.
    def next_client_message_id
      @client_message_ids += 1
      Digest::UUID.uuid_v5(Digest::UUID::URL_NAMESPACE, "https://parity.campfire.test/messages/#{@client_message_ids}")
    end

    # Post a message the way MessagesController#create and the bot API do.
    def post(room, creator, time, body = nil, attachment: nil, as: nil)
      at time
      attributes = { creator: creator, body: body, client_message_id: next_client_message_id }
      message = if attachment
        room.messages.create_with_attachment!(attributes.merge(attachment: attachment)).tap { |m| process_poster(m) }
      else
        room.messages.create!(attributes)
      end
      as ? label(:messages, as, message) : message
    end

    # A video's poster is a different representation from the one Message::Attachment processes
    # (app/helpers/messages/attachment_presentation.rb adds resize_to_limit), so the app would make
    # it on the first request, under a random blob key, on every server. Process it here instead.
    def process_poster(message)
      if message.attachment.video?
        message.attachment.preview(format: :webp, resize_to_limit: [ Message::THUMBNAIL_MAX_WIDTH, Message::THUMBNAIL_MAX_HEIGHT ]).processed
      end
    end

    # Rewrite a message body in place, bypassing Action Text canonicalization: for bodies saved by
    # older versions of the app (Trix era, Rails 7 SGIDs) that today's editor can't produce.
    def store_raw_body(message, html)
      message.body.update_column(:body, html)
      message.send(:update_in_index)
    end

    def boost(message, booster, content, time)
      at time
      message.boosts.create!(booster: booster, content: content)
    end

    def file(name, content_type)
      { io: File.open(FILES.join(name)), filename: name, content_type: content_type }
    end

    def text_file(name, text)
      { io: StringIO.new(text), filename: name, content_type: "text/plain" }
    end

    # The Lexxy serialization of a mention (see test/test_helpers/mention_test_helper.rb).
    def mention(user)
      content = ApplicationController.render(partial: "users/mention", locals: { user: user })
      %(<action-text-attachment sgid="#{user.attachable_sgid}" content-type="application/vnd.campfire.mention" content="#{ERB::Util.html_escape(content)}"></action-text-attachment>)
    end

    # A mention as Rails 7 signed it: a Marshal-serialized GlobalID. Its signature no longer
    # verifies, which lib/rails_ext/action_text_attachables.rb deliberately tolerates for users.
    def marshal_era_mention(user)
      data = Base64.strict_encode64(Marshal.dump(user.to_global_id.to_s))
      envelope = Base64.strict_encode64({ "_rails" => { "message" => data, "exp" => nil, "pur" => "attachable" } }.to_json)
      %(<action-text-attachment sgid="#{envelope}--0123456789abcdef0123456789abcdef01234567" content-type="application/vnd.campfire.mention"></action-text-attachment>)
    end

    # The Lexxy serialization of an unfurled link (app/javascript/controllers/unfurl_controller.js).
    def opengraph_embed(href:, title:, description:, image: nil, twitter_avatar: false)
      image_html = image ? %(<div class="og-embed__image"><img src="#{image}" class="image center" alt="" /></div>) : ""
      content = <<~HTML.squish
        <actiontext-opengraph-embed><div class="og-embed gap #{"og-embed--twitter-avatar" if twitter_avatar}">
        <div class="og-embed__content"><div class="og-embed__title"><a href="#{href}" rel="noreferrer" target="_blank">#{ERB::Util.html_escape(title)}</a></div>
        <div class="og-embed__description">#{ERB::Util.html_escape(description)}</div></div>#{image_html}</div></actiontext-opengraph-embed>
      HTML
      %(<action-text-attachment content-type="application/vnd.actiontext.opengraph-embed" content="#{ERB::Util.html_escape(content)}"></action-text-attachment>)
    end

    # -- People -------------------------------------------------------------------------------

    def attach_avatar(user, attachable)
      user.avatar.attach(attachable)
      user.avatar.analyze
      user.avatar_variant
    end

    def unread!(room_name, user_name, time)
      membership(room_name, user_name).update_columns(unread_at: time)
    end

    def membership(room_name, user_name)
      Membership.find_by!(room_id: id_for(:rooms, room_name), user_id: id_for(:users, user_name))
    end

    def transfer_id_at(user, time)
      at(time) { user.transfer_id }
    end

    # -- Output -------------------------------------------------------------------------------

    # Blob keys (the storage/ file names) come from SecureRandom; number them instead so a rebuilt seed
    # has an identical storage tree.
    def deterministic_blob_keys
      sequence = 0
      ActiveStorage::Blob.singleton_class.prepend(Module.new do
        define_method(:generate_unique_secure_token) do |length: ActiveStorage::Blob::MINIMUM_TOKEN_LENGTH|
          sequence += 1
          Digest::SHA256.hexdigest("parity-blob-#{sequence}").to_i(16).to_s(36).first(length)
        end
      end)
    end

    # "secret123456" hashed once (bcrypt cost 12): every build then writes identical user rows.
    PASSWORD_DIGEST = "$2a$12$Gz1V3e1PgXe5HFJ5lLlH8eP5BlvdNLdGGdQyacJSn7rvquw4Lt0ue"

    def pin_nondeterministic_columns
      User.where.not(password_digest: nil).update_all(password_digest: PASSWORD_DIGEST)
      ActiveRecord::Base.connection.execute(
        ActiveRecord::Base.sanitize_sql([ "UPDATE ar_internal_metadata SET created_at = ?, updated_at = ?", NOW, NOW ]))
    end

    def checkpoint
      connection = ActiveRecord::Base.connection
      connection.execute("PRAGMA wal_checkpoint(TRUNCATE)")
    end
  end
end
