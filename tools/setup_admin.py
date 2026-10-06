#!/usr/bin/env python3
"""Create the first AI Box web administrator from a local terminal."""
import getpass
import hashlib
import json
import os
import secrets
import sys
from pathlib import Path

data = Path(os.environ.get("AIBOX_DATA", "./data"))
target = data / "admin.json"
if target.exists():
    sys.exit("Administrator already exists; remove it deliberately to reset credentials.")
first = getpass.getpass("New administrator password (12+ characters): ")
second = getpass.getpass("Repeat password: ")
if first != second or len(first) < 12:
    sys.exit("Passwords must match and contain at least 12 characters.")
data.mkdir(parents=True, exist_ok=True)
salt = secrets.token_bytes(16)
record = {"salt": salt.hex(), "hash": hashlib.scrypt(first.encode(), salt=salt, n=2**14, r=8, p=1).hex()}
fd = os.open(target, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
with os.fdopen(fd, "w") as f:
    json.dump(record, f)
print("Administrator created.")
