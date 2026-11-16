#!/usr/bin/env python3
"""A stand-in for the bits of Google the Vita's Drive client uses (src/drive.c), for testing it in
Vita3K without a Google account: the device sign-in, tokens, and Drive v3 (list in pages, folders,
resumable uploads, downloads, trash), all in memory.

    python3 -I vita/tests/drive_mock.py <port> <seed folder> [<dump folder>]

The seed folder plays the PC's vault, already in Drive as "Lewd Vault". A debug build of the Vita app
talks to it when ux0:data/LWDV00001/debug_drive.txt holds http://127.0.0.1:<port>. GET /debug/dump
writes what Drive holds now into the dump folder (the paths below "Lewd Vault").
"""
import hashlib
import json
import os
import sys
import threading
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT = int(sys.argv[1])
SEED = sys.argv[2]
DUMP = sys.argv[3] if len(sys.argv) > 3 else None
FOLDER = "application/vnd.google-apps.folder"
PAGE = 7  # (small pages: the client must follow nextPageToken)

lock = threading.Lock()
files = {}  # id -> {name, parents, mimeType, data, trashed}
sessions = {}  # id -> {meta, existing, total, data}
polls = {"n": 0}
counter = [100]


def new_id():
    counter[0] += 1
    return "f%06d" % counter[0]


def seed():
    root = new_id()
    files[root] = {"name": "Lewd Vault", "parents": ["root"], "mimeType": FOLDER, "data": b"", "trashed": False}
    ids = {"": root}
    for d, dirs, names in os.walk(SEED):
        rel = os.path.relpath(d, SEED).replace(os.sep, "/")
        rel = "" if rel == "." else rel
        for n in sorted(dirs):
            fid = new_id()
            files[fid] = {"name": n, "parents": [ids[rel]], "mimeType": FOLDER, "data": b"", "trashed": False}
            ids[(rel + "/" if rel else "") + n] = fid
        for n in sorted(names):
            with open(os.path.join(d, n), "rb") as f:
                data = f.read()
            files[new_id()] = {"name": n, "parents": [ids[rel]], "mimeType": "application/octet-stream", "data": data, "trashed": False}
    return root


ROOT = seed()


def info(fid):
    f = files[fid]
    o = {"id": fid, "name": f["name"], "mimeType": f["mimeType"], "parents": f["parents"], "trashed": f["trashed"]}
    if f["mimeType"] != FOLDER:
        o["md5Checksum"] = hashlib.md5(f["data"]).hexdigest()
        o["size"] = str(len(f["data"]))
    return o


def path_of(fid):
    parts = []
    while fid != ROOT:
        f = files.get(fid)
        if not f:
            return None
        parts.append(f["name"])
        fid = f["parents"][0]
    return "/".join(reversed(parts))


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        sys.stderr.write("mock: %s %s\n" % (self.command, self.path[:120]))

    def send(self, code, body=b"", headers=None, ctype="application/json"):
        if isinstance(body, (dict, list)):
            body = json.dumps(body).encode()
        self.send_response(code)
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def body(self):
        n = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(n) if n else b""

    def authed(self):
        if self.headers.get("Authorization") != "Bearer at-1":
            self.send(401, {"error": {"code": 401}})
            return False
        return True

    def do_POST(self):
        u = urllib.parse.urlparse(self.path)
        q = urllib.parse.parse_qs(u.query)
        raw = self.body()
        with lock:
            if u.path == "/device/code":
                form = urllib.parse.parse_qs(raw.decode())
                if form.get("client_id", [""])[0] != "test-client":
                    return self.send(401, {"error": "invalid_client"})
                return self.send(200, {"device_code": "dev-1", "user_code": "WXYZ-ABCD", "verification_url": "https://www.google.com/device", "expires_in": 1800, "interval": 5})
            if u.path == "/token":
                form = urllib.parse.parse_qs(raw.decode())
                g = form.get("grant_type", [""])[0]
                if g.endswith("device_code"):
                    polls["n"] += 1
                    if polls["n"] < 2:
                        return self.send(428, {"error": "authorization_pending"})
                    return self.send(200, {"access_token": "at-1", "refresh_token": "rt-1", "expires_in": 3599})
                if g == "refresh_token" and form.get("refresh_token", [""])[0] == "rt-1":
                    return self.send(200, {"access_token": "at-1", "expires_in": 3599})
                return self.send(400, {"error": "invalid_grant"})
            if u.path == "/revoke":
                return self.send(200, {})
            if not self.authed():
                return
            if u.path == "/drive/v3/files":
                meta = json.loads(raw or b"{}")
                fid = new_id()
                files[fid] = {"name": meta["name"], "parents": meta.get("parents", ["root"]), "mimeType": meta.get("mimeType", "application/octet-stream"), "data": b"", "trashed": False}
                return self.send(200, {"id": fid})
            if u.path == "/upload/drive/v3/files" and q.get("uploadType") == ["resumable"]:
                sid = new_id()
                sessions[sid] = {"meta": json.loads(raw or b"{}"), "existing": None, "total": int(self.headers.get("X-Upload-Content-Length")), "data": b""}
                return self.send(200, {}, {"Location": "http://127.0.0.1:%d/upload/session/%s" % (PORT, sid)})
        self.send(404, {"error": "not here"})

    def do_PATCH(self):
        u = urllib.parse.urlparse(self.path)
        raw = self.body()
        with lock:
            if not self.authed():
                return
            parts = u.path.split("/")
            if u.path.startswith("/upload/drive/v3/files/"):
                fid = parts[-1]
                if fid not in files:
                    return self.send(404, {})
                sid = new_id()
                sessions[sid] = {"meta": {}, "existing": fid, "total": int(self.headers.get("X-Upload-Content-Length")), "data": b""}
                return self.send(200, {}, {"Location": "http://127.0.0.1:%d/upload/session/%s" % (PORT, sid)})
            if u.path.startswith("/drive/v3/files/"):
                fid = parts[-1]
                if fid not in files:
                    return self.send(404, {})
                meta = json.loads(raw or b"{}")
                if meta.get("trashed"):
                    files[fid]["trashed"] = True
                return self.send(200, info(fid))
        self.send(404, {})

    def do_PUT(self):
        u = urllib.parse.urlparse(self.path)
        raw = self.body()
        with lock:
            sid = u.path.split("/")[-1]
            s = sessions.get(sid)
            if not s or not u.path.startswith("/upload/session/"):
                return self.send(404, {})
            rng = self.headers.get("Content-Range", "")
            if rng != "bytes */0":
                a = int(rng.split(" ")[1].split("-")[0])
                if a != len(s["data"]):
                    return self.send(400, {"error": "out of order"})
                s["data"] += raw
            if len(s["data"]) < s["total"]:
                return self.send(308, b"", {"Range": "bytes=0-%d" % (len(s["data"]) - 1)})
            if s["existing"]:
                fid = s["existing"]
                files[fid]["data"] = s["data"]
            else:
                fid = new_id()
                m = s["meta"]
                files[fid] = {"name": m["name"], "parents": m["parents"], "mimeType": "application/octet-stream", "data": s["data"], "trashed": False}
            del sessions[sid]
            o = info(fid)
            return self.send(200, {"id": fid, "md5Checksum": o["md5Checksum"]})

    def do_GET(self):
        u = urllib.parse.urlparse(self.path)
        q = urllib.parse.parse_qs(u.query)
        with lock:
            if u.path == "/debug/dump":
                n = 0
                for fid, f in files.items():
                    p = path_of(fid) if f["parents"][0] != "root" else None
                    if p is None or f["trashed"] or f["mimeType"] == FOLDER:
                        continue
                    # (a file in a trashed folder doesn't count either)
                    up, gone = f["parents"][0], False
                    while up != ROOT:
                        gone |= files[up]["trashed"]
                        up = files[up]["parents"][0]
                    if gone:
                        continue
                    out = os.path.join(DUMP, p)
                    os.makedirs(os.path.dirname(out), exist_ok=True)
                    with open(out, "wb") as w:
                        w.write(f["data"])
                    n += 1
                return self.send(200, {"files": n})
            if not self.authed():
                return
            if u.path == "/drive/v3/files":
                query = q.get("q", [""])[0]
                live = [fid for fid, f in files.items() if not f["trashed"]]
                if "name = '" in query:
                    name = query.split("name = '")[1].split("' and")[0].replace("\\'", "'")
                    hits = [fid for fid in live if files[fid]["name"] == name and files[fid]["mimeType"] == FOLDER and files[fid]["parents"] == ["root"]]
                    return self.send(200, {"files": [{"id": h} for h in hits]})
                start = int(q.get("pageToken", ["0"])[0])
                page = live[start:start + PAGE]
                o = {"files": [info(fid) for fid in page]}
                if start + PAGE < len(live):
                    o["nextPageToken"] = str(start + PAGE)
                return self.send(200, o)
            if u.path.startswith("/drive/v3/files/"):
                fid = u.path.split("/")[-1]
                if fid not in files:
                    return self.send(404, {"error": {"code": 404}})
                if q.get("alt") == ["media"]:
                    return self.send(200, files[fid]["data"], ctype="application/octet-stream")
                return self.send(200, info(fid))
        self.send(404, {})


ThreadingHTTPServer(("127.0.0.1", PORT), H).serve_forever()
