#!/usr/bin/env python3
"""Require benchmark POSTs to create persistent messages, not merely return 200."""
import gzip
import http.client
import json
from pathlib import Path
import sqlite3
import sys
import urllib.parse
import uuid


def count(database, room):
    with sqlite3.connect(database) as db:
        return db.execute('SELECT COUNT(*) FROM messages WHERE room_id=?', (room,)).fetchone()[0]


def preflight(base, cookie, room, database, output, csrf=""):
    with sqlite3.connect(database) as db:
        assert db.execute('SELECT COUNT(*) FROM memberships WHERE room_id=? AND user_id=1', (room,)).fetchone()[0] == 1, 'Posting user must belong to the write room'
        assert not db.execute('SELECT 1 FROM memberships m LEFT JOIN rooms r ON r.id=m.room_id LEFT JOIN users u ON u.id=m.user_id WHERE r.id IS NULL OR u.id IS NULL LIMIT 1').fetchone(), 'Seed contains orphan memberships'
    token = 'verify-write-' + uuid.uuid4().hex
    before = count(database, room)
    uri = urllib.parse.urlsplit(base)
    connection = http.client.HTTPConnection(uri.hostname, uri.port, timeout=30)
    body = urllib.parse.urlencode({'message[body]': token, 'message[client_message_id]': token, 'authenticity_token': csrf})
    connection.request('POST', f'/rooms/{room}/messages', body, {
        'Cookie': cookie, 'Content-Type': 'application/x-www-form-urlencoded',
        'Accept': 'text/vnd.turbo-stream.html, text/html, application/xhtml+xml',
        'Sec-Fetch-Site': 'same-origin', 'Accept-Encoding': 'gzip', 'X-CSRF-Token': csrf,
    })
    response = connection.getresponse()
    payload = response.read()
    encoding = response.getheader('Content-Encoding')
    content_type = response.getheader('Content-Type', '')
    if encoding == 'gzip':
        payload = gzip.decompress(payload)
    connection.close()
    assert response.status == 200 and content_type.startswith('text/vnd.turbo-stream.html'), (response.status, content_type)
    assert token.encode() in payload and b'This room was deleted.' not in payload, 'POST must render the new message'
    after = count(database, room)
    assert after == before + 1, (before, after)
    with sqlite3.connect(database) as db:
        stored = db.execute('SELECT m.id,r.body FROM messages m JOIN action_text_rich_texts r ON r.record_id=m.id AND r.record_type=\'Message\' AND r.name=\'body\' WHERE m.room_id=? AND m.client_message_id=?', (room, token)).fetchall()
    assert len(stored) == 1 and token in stored[0][1], 'POST must persist its actual body and client message ID'
    result = {'passed': True, 'messages_before': before, 'messages_after': after, 'status': response.status, 'content_type': content_type, 'persisted_message_and_body': True}
    Path(output).write_text(json.dumps(result, indent=2) + '\n')


if __name__ == '__main__':
    mode, *args = sys.argv[1:]
    if mode == 'count':
        print(count(*args))
    elif mode == 'check':
        database, room, before = args
        result = json.load(sys.stdin)
        after = count(database, room)
        persisted = after - int(before)
        assert result['errors'] == 0 and result.get('invalid_responses', 0) == 0 and set(result['statuses']) == {'200'}, result
        assert result['ok'] > 0 and persisted == result['ok'], {'counted_successes': result['ok'], 'persisted_messages': persisted}
        with sqlite3.connect(database) as db:
            missing = db.execute(
                "SELECT COUNT(*) FROM (SELECT id FROM messages WHERE room_id=? ORDER BY id DESC LIMIT ?) m "
                "WHERE NOT EXISTS (SELECT 1 FROM action_text_rich_texts r WHERE r.record_type='Message' AND r.record_id=m.id AND r.name='body' AND r.body LIKE '%bench write %') "
                "OR NOT EXISTS (SELECT 1 FROM message_search_index idx WHERE idx.rowid=m.id)",
                (room, persisted),
            ).fetchone()[0]
        assert missing == 0, 'Successful posts must persist their bodies and search entries'
        result['persisted_messages'] = persisted
        print(json.dumps(result))
    elif mode == 'preflight':
        preflight(*args)
    else:
        raise SystemExit('usage: validate_writes.py count|check|preflight ...')
