#!/usr/bin/env python3
"""Writes a ClipItTo history database at schema version 1 (before nested groups), with
grouped clips, so the 001 -> 002 migration can be checked through the real app:

    python3 make-v1-history.py /tmp/v1.db
    python3 driver.py start --history /tmp/v1.db   # the tree must show Work > Servers
"""
import hashlib
import sqlite3
import sys
from pathlib import Path

SCHEMA_1 = Path(__file__).resolve().parents[3] / "src/core/db/migrations/001_init.sql"

path = Path(sys.argv[1])
path.unlink(missing_ok=True)
db = sqlite3.connect(path)
db.executescript(SCHEMA_1.read_text())
for content, group in [("ssh admin@db-01", "Work/Servers"), ("deploy notes", "Work"), ("loose copy", None)]:
    db.execute("INSERT INTO clips(content, content_hash, group_name) VALUES (?, ?, ?)",
               (content, hashlib.sha256(content.encode()).hexdigest(), group))
db.execute("PRAGMA user_version = 1")
db.commit()
print(f"wrote {path} (schema 1: {db.execute('SELECT count(*) FROM clips').fetchone()[0]} clips)")
