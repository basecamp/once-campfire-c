import importlib.util
from pathlib import Path
import sqlite3
import tempfile
import unittest

path = Path(__file__).with_name('import_seed.py')
spec = importlib.util.spec_from_file_location('import_seed', path)
seed = importlib.util.module_from_spec(spec)
spec.loader.exec_module(seed)


class SeedTests(unittest.TestCase):
    def test_memberships_reference_existing_users_and_rooms(self):
        with tempfile.TemporaryDirectory() as directory:
            result = seed.build(Path(directory) / 'seed', seed.DEFAULT_SCHEMA, seed.load_secret_key_base(None))
            with sqlite3.connect(result['db']) as db:
                self.assertEqual(db.execute('SELECT COUNT(*) FROM memberships').fetchone()[0], 15)
                self.assertFalse(db.execute('SELECT 1 FROM memberships m LEFT JOIN rooms r ON r.id=m.room_id LEFT JOIN users u ON u.id=m.user_id WHERE r.id IS NULL OR u.id IS NULL').fetchall())
                self.assertEqual(db.execute('SELECT involvement FROM memberships WHERE room_id=2 AND user_id=1').fetchall(), [('nothing',)])
                self.assertFalse(db.execute('SELECT room_id,user_id FROM memberships GROUP BY room_id,user_id HAVING COUNT(*)>1').fetchall())

    def test_import_is_deterministic(self):
        with tempfile.TemporaryDirectory() as directory:
            first = seed.build(Path(directory) / 'a', seed.DEFAULT_SCHEMA, seed.load_secret_key_base(None))
            second = seed.build(Path(directory) / 'b', seed.DEFAULT_SCHEMA, seed.load_secret_key_base(None))
            self.assertEqual(first['db_sha256'], second['db_sha256'])


if __name__ == '__main__':
    unittest.main()
