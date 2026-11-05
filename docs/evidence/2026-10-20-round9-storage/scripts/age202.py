#!/usr/bin/env python3
"""Move every operation in a stopped resident's journal back DAYS days: age202.py DB DAYS"""
import datetime, json, sqlite3, sys
db, days = sys.argv[1], int(sys.argv[2])
c = sqlite3.connect(db)
for oid, data in c.execute('SELECT id, data FROM operations').fetchall():
    d = json.loads(data)
    t = datetime.datetime.fromisoformat(d['created'].replace('Z', '+00:00')) - datetime.timedelta(days=days)
    d['created'] = t.strftime('%Y-%m-%dT%H:%M:%S.') + f'{t.microsecond // 1000:03d}Z'
    c.execute('UPDATE operations SET data=? WHERE id=?', (json.dumps(d), oid))
    c.execute('UPDATE history SET created=? WHERE operation_id=?', (d['created'], oid))
    print(oid, d['state'], d['created'])
c.commit()
