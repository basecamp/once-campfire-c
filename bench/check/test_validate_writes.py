"""Reject the HTTP 200 error pages that inflated the original POST benchmark."""
import json
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import unittest


class WriteValidationTests(unittest.TestCase):
    def test_success_requires_a_message_body_and_search_entry(self):
        with tempfile.TemporaryDirectory() as directory:
            database = Path(directory) / 'test.sqlite3'
            with sqlite3.connect(database) as db:
                db.executescript('''
                    CREATE TABLE messages(id INTEGER PRIMARY KEY, room_id INTEGER);
                    CREATE TABLE action_text_rich_texts(record_id INTEGER, record_type TEXT, name TEXT, body TEXT);
                    CREATE VIRTUAL TABLE message_search_index USING fts5(body);
                ''')
            result = {'ok': 1, 'errors': 0, 'invalid_responses': 0, 'statuses': {'200': 1}}

            def check():
                return subprocess.run(
                    [sys.executable, str(Path(__file__).with_name('validate_writes.py')),
                     'check', str(database), '2', '0'],
                    input=json.dumps(result), text=True, capture_output=True,
                )

            self.assertNotEqual(check().returncode, 0)  # HTTP 200 without a write.
            with sqlite3.connect(database) as db:
                db.execute('INSERT INTO messages VALUES (1, 2)')
            self.assertNotEqual(check().returncode, 0)  # Message without its body/index.
            with sqlite3.connect(database) as db:
                db.execute("INSERT INTO action_text_rich_texts VALUES (1, 'Message', 'body', 'bench write actual-content')")
            self.assertNotEqual(check().returncode, 0)  # Body without its search entry.
            with sqlite3.connect(database) as db:
                db.execute("INSERT INTO message_search_index(rowid,body) VALUES (1,'bench write actual-content')")
            passed = check()
            self.assertEqual(passed.returncode, 0, passed.stderr)
            self.assertEqual(json.loads(passed.stdout)['persisted_messages'], 1)
            result['ok'] = 2
            self.assertNotEqual(check().returncode, 0)  # One write cannot count as two.


if __name__ == '__main__':
    unittest.main()
