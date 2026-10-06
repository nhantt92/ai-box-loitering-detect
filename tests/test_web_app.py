"""Integration checks for login-gated FastAPI proxy routes."""
import importlib
import json
import os
import sys
import tempfile
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from fastapi.testclient import TestClient


TOKEN = "a" * 64
FRAME = b"\xff\xd8fake-jpeg\xff\xd9"
STREAM = (b"--frame\r\nContent-Type: image/jpeg\r\nContent-Length: "
          + str(len(FRAME)).encode() + b"\r\n\r\n" + FRAME + b"\r\n")


class CoreHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_args):
        pass

    def do_GET(self):
        if self.headers.get("Authorization") != f"Bearer {TOKEN}":
            self.send_error(401)
            return
        if self.path.startswith("/api/stream.mjpg"):
            body, content_type = STREAM, "multipart/x-mixed-replace; boundary=frame"
        elif self.path.startswith("/api/frame.jpg") or self.path.endswith("/snapshot"):
            body, content_type = FRAME, "image/jpeg"
        elif self.path.startswith("/api/events.csv"):
            body, content_type = b"id,type\r\ne1,loitering\r\n", "text/csv; charset=utf-8"
        elif self.path.startswith("/api/status"):
            body, content_type = json.dumps({"core": "online", "camera": "connected"}).encode(), "application/json"
        elif self.path.startswith("/api/config"):
            body, content_type = json.dumps({"revision": 7}).encode(), "application/json"
        elif self.path.startswith("/api/events"):
            body, content_type = json.dumps({"items": []}).encode(), "application/json"
        else:
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)
        self.close_connection = True

    def do_PUT(self):
        if self.headers.get("Authorization") != f"Bearer {TOKEN}":
            self.send_error(401)
            return
        if self.path not in {"/api/config", "/api/config/camera"}:
            self.send_error(404)
            return
        request = json.loads(self.rfile.read(int(self.headers.get("Content-Length", "0"))))
        response = {"revision": request["revision"] + 1,
                    "state": "applying" if self.path == "/api/config" else "reconnecting"}
        if self.path == "/api/config/camera":
            response["camera"] = request["camera"]
        body = json.dumps(response).encode()
        self.send_response(202)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)
        self.close_connection = True


class WebProxyIntegrationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.data_dir = Path(tempfile.mkdtemp(prefix="aibox-web-tests-"))
        cls.token_path = cls.data_dir / "core-api.token"
        cls.token_path.write_text(TOKEN + "\n")
        cls.token_path.chmod(0o600)
        cls.core = ThreadingHTTPServer(("127.0.0.1", 0), CoreHandler)
        cls.core_thread = threading.Thread(target=cls.core.serve_forever, daemon=True)
        cls.core_thread.start()
        os.environ["AIBOX_DATA"] = str(cls.data_dir)
        os.environ["AIBOX_CORE_URL"] = f"http://127.0.0.1:{cls.core.server_port}"
        cls.web_app = importlib.import_module("web.app")
        cls.client = TestClient(cls.web_app.app)

    @classmethod
    def tearDownClass(cls):
        cls.client.close()
        cls.core.shutdown()
        cls.core.server_close()
        cls.core_thread.join(timeout=2)
        import shutil
        shutil.rmtree(cls.data_dir, ignore_errors=True)

    def setUp(self):
        password = "test-admin-password"
        saved = self.web_app.password_hash(password)
        self.web_app.ADMIN.write_text(json.dumps(saved))
        self.password = password
        self.client.cookies.clear()
        self.web_app.LOGIN_ATTEMPTS.clear()

    def login(self):
        response = self.client.post("/api/login", json={"password": self.password})
        self.assertEqual(response.status_code, 200, response.text)

    def test_routes_require_session_and_proxy_core_responses(self):
        self.assertEqual(self.client.get("/api/status").status_code, 401)
        self.assertEqual(self.client.put("/api/config/camera", json={}).status_code, 401)
        self.login()
        self.assertEqual(self.client.get("/api/status").json()["camera"], "connected")
        self.assertEqual(self.client.get("/api/config").json()["revision"], 7)
        update = self.client.put("/api/config/camera", json={
            "revision": 7, "camera": {"url": "rtsp://192.168.1.88:554/profile2"}})
        self.assertEqual(update.status_code, 202, update.text)
        self.assertEqual(update.json()["camera"]["url"], "rtsp://192.168.1.88:554/profile2")
        behavior = self.client.put("/api/config", json={
            "revision": 8, "zones": [{"id": "entry"}],
        })
        self.assertEqual(behavior.status_code, 202, behavior.text)
        self.assertEqual(behavior.json()["revision"], 9)
        self.assertEqual(self.client.get("/api/events?offset=0&limit=50").json(), {"items": []})
        csv_response = self.client.get("/api/events.csv?limit=1000")
        self.assertEqual(csv_response.status_code, 200)
        self.assertIn(b"e1,loitering", csv_response.content)
        self.assertEqual(self.client.get("/api/frame.jpg").content, FRAME)
        snapshot = self.client.get("/api/events/0123456789abcdef0123456789abcdef-1/snapshot")
        self.assertEqual(snapshot.content, FRAME)

    def test_mjpeg_proxy_relays_stream_and_releases_client_slot(self):
        self.login()
        response = self.client.get("/api/stream.mjpg")
        self.assertEqual(response.status_code, 200)
        self.assertIn(b"--frame", response.content)
        self.assertIn(FRAME, response.content)
        self.assertEqual(self.web_app.PREVIEW_CLIENTS, 0)

    def test_invalid_event_pagination_is_rejected_before_proxy(self):
        self.login()
        response = self.client.get("/api/events?offset=-1&limit=50")
        self.assertEqual(response.status_code, 422)

    def test_login_rate_limit_is_server_side(self):
        for _ in range(5):
            response = self.client.post("/api/login", json={"password": "wrong-password"})
            self.assertEqual(response.status_code, 401)
        self.assertEqual(self.client.post("/api/login", json={"password": self.password}).status_code, 429)


if __name__ == "__main__":
    unittest.main()
