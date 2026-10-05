# restricted: default with "Must be admin to create new rooms" switched on (as AccountsController#update
# does it).
based_on "default"

at NOW - 8.days
Account.first.update!(settings: { restrict_room_creation_to_administrators: true })
