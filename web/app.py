"""LAN control surface. Detection and RTSP handling belong in ai-box-core."""
import hashlib
import hmac
import json
import os
import secrets
import stat
import asyncio
from collections import deque
import time
from pathlib import Path

from fastapi import FastAPI, HTTPException, Request, Response
from fastapi.responses import FileResponse
from fastapi.responses import StreamingResponse
from starlette.middleware.sessions import SessionMiddleware
import httpx

ROOT = Path(os.environ.get("AIBOX_DATA", "./data"))
ROOT.mkdir(parents=True, exist_ok=True)
ADMIN = ROOT / "admin.json"
STATIC = Path(__file__).parent / "static"
CORE_URL = os.environ.get("AIBOX_CORE_URL", "http://127.0.0.1:8091").rstrip("/")
CORE_TOKEN_FILE = Path(os.environ.get("AIBOX_CORE_TOKEN_FILE", ROOT / "core-api.token"))
PREVIEW_CLIENTS = 0
PREVIEW_CLIENTS_LOCK = asyncio.Lock()
LOGIN_ATTEMPTS = {}
LOGIN_LOCK = asyncio.Lock()
COOKIE_SECURE = os.environ.get("AIBOX_COOKIE_SECURE", "0").lower() in {"1", "true", "yes"}
app = FastAPI(title="AI Box Local API", version="0.1.0")
_session_key = os.environ.get("AIBOX_SESSION_KEY")
if not _session_key:
    _key_file = ROOT / "session.key"
    if _key_file.exists():
        _session_key = _key_file.read_text().strip()
    else:
        _session_key = secrets.token_urlsafe(48)
        fd = os.open(_key_file, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, "w") as _f:
            _f.write(_session_key)
app.add_middleware(SessionMiddleware, secret_key=_session_key, same_site="strict", https_only=COOKIE_SECURE)


def password_hash(password: str, salt: bytes = None) -> dict:
    salt = salt or secrets.token_bytes(16)
    return {"salt": salt.hex(), "hash": hashlib.scrypt(password.encode(), salt=salt, n=2**14, r=8, p=1).hex()}


def require_login(request: Request):
    if not request.session.get("admin"):
        raise HTTPException(401, "Authentication required")


def core_headers() -> dict:
    try:
        mode = stat.S_IMODE(CORE_TOKEN_FILE.stat().st_mode)
        if mode & 0o077:
            raise HTTPException(503, "Core API token file permissions must be 0600")
        token = CORE_TOKEN_FILE.read_text().strip()
    except HTTPException:
        raise
    except OSError as exc:
        raise HTTPException(503, "ai-box-core API token is not available") from exc
    if len(token) < 32:
        raise HTTPException(503, "ai-box-core API token is invalid")
    return {"Authorization": f"Bearer {token}"}


async def core_get(path: str, params: dict = None) -> httpx.Response:
    headers = core_headers()
    try:
        async with httpx.AsyncClient(base_url=CORE_URL, timeout=5.0) as client:
            upstream = await client.get(path, params=params, headers=headers)
    except httpx.HTTPError as exc:
        raise HTTPException(503, "ai-box-core API is unavailable") from exc
    if upstream.status_code >= 400:
        try:
            detail = upstream.json().get("error", "Core API request failed")
        except Exception:
            detail = "Core API request failed"
        raise HTTPException(upstream.status_code, detail)
    return upstream


@app.post("/api/login")
async def login(request: Request):
    now = time.monotonic()
    client_ip = request.client.host if request.client else "unknown"
    async with LOGIN_LOCK:
        for address, attempts in list(LOGIN_ATTEMPTS.items()):
            while attempts and now - attempts[0] >= 60:
                attempts.popleft()
            if not attempts:
                LOGIN_ATTEMPTS.pop(address, None)
        history = LOGIN_ATTEMPTS.get(client_ip)
        if history is None:
            if len(LOGIN_ATTEMPTS) >= 1024:
                LOGIN_ATTEMPTS.pop(next(iter(LOGIN_ATTEMPTS)))
            history = deque()
            LOGIN_ATTEMPTS[client_ip] = history
        if len(history) >= 5: raise HTTPException(429, "Try again later")
        history.append(now)
    body = await request.json()
    try: saved = json.loads(ADMIN.read_text()); salt = bytes.fromhex(saved["salt"])
    except Exception: raise HTTPException(503, "Initial admin setup required")
    attempt = password_hash(body.get("password", ""), salt)["hash"]
    if not hmac.compare_digest(attempt, saved["hash"]):
        raise HTTPException(401, "Invalid credentials")
    request.session.clear(); request.session["admin"] = True
    return {"ok": True}


@app.post("/api/logout")
async def logout(request: Request):
    require_login(request); request.session.clear(); return {"ok": True}


@app.middleware("http")
async def csrf_origin(request: Request, call_next):
    if request.method in {"POST", "PUT", "DELETE"} and request.url.path != "/api/login":
        origin = request.headers.get("origin")
        if origin and origin.rstrip("/") != str(request.base_url).rstrip("/"):
            return Response("Origin rejected", status_code=403)
    return await call_next(request)


@app.get("/api/status")
async def status(request: Request):
    require_login(request)
    upstream = await core_get("/api/status")
    return upstream.json()


@app.get("/api/config")
async def get_config(request: Request):
    require_login(request)
    upstream = await core_get("/api/config")
    return upstream.json()


@app.put("/api/config", status_code=202)
async def put_config(request: Request):
    require_login(request)
    try:
        body = await request.json()
    except Exception as exc:
        raise HTTPException(422, "Invalid JSON body") from exc
    try:
        async with httpx.AsyncClient(base_url=CORE_URL, timeout=5.0) as client:
            upstream = await client.put("/api/config", json=body,
                                        headers=core_headers())
    except httpx.HTTPError as exc:
        raise HTTPException(503, "ai-box-core API is unavailable") from exc
    if upstream.status_code >= 400:
        try:
            detail = upstream.json().get("error", "Core API request failed")
        except Exception:
            detail = "Core API request failed"
        raise HTTPException(upstream.status_code, detail)
    return upstream.json()


@app.put("/api/config/camera", status_code=202)
async def put_camera_config(request: Request):
    require_login(request)
    try:
        body = await request.json()
    except Exception as exc:
        raise HTTPException(422, "Invalid JSON body") from exc
    try:
        async with httpx.AsyncClient(base_url=CORE_URL, timeout=5.0) as client:
            upstream = await client.put("/api/config/camera", json=body,
                                        headers=core_headers())
    except httpx.HTTPError as exc:
        raise HTTPException(503, "ai-box-core API is unavailable") from exc
    if upstream.status_code >= 400:
        try:
            detail = upstream.json().get("error", "Core API request failed")
        except Exception:
            detail = "Core API request failed"
        raise HTTPException(upstream.status_code, detail)
    return upstream.json()


@app.get("/api/config/jobs/{job_id}")
async def job(job_id: str, request: Request):
    require_login(request); raise HTTPException(503, "Config jobs require the ai-box-core API")


@app.get("/api/events")
async def events(request: Request, offset: int = 0, limit: int = 50):
    require_login(request)
    if not 1 <= limit <= 200 or not 0 <= offset <= 1000000:
        raise HTTPException(422, "offset must be 0..1000000 and limit must be 1..200")
    upstream = await core_get("/api/events", {"offset": offset, "limit": limit})
    return upstream.json()


@app.get("/api/events.csv")
async def events_csv(request: Request, limit: int = 1000):
    require_login(request)
    if not 1 <= limit <= 1000: raise HTTPException(422, "limit must be 1..1000")
    upstream = await core_get("/api/events.csv", {"limit": limit})
    return Response(upstream.content, media_type="text/csv; charset=utf-8",
                    headers={"Content-Disposition": "attachment; filename=ai-box-events.csv"})


@app.get("/api/events/{event_id}/snapshot")
async def snapshot(event_id: str, request: Request):
    require_login(request)
    from urllib.parse import quote
    upstream = await core_get(f"/api/events/{quote(event_id, safe='')}/snapshot")
    return Response(upstream.content, media_type="image/jpeg",
                    headers={"Cache-Control": "private, no-store"})


@app.get("/api/frame.jpg")
async def frame(request: Request):
    require_login(request)
    upstream = await core_get("/api/frame.jpg")
    headers = {"Cache-Control": "private, no-store"}
    if "x-sequence" in upstream.headers: headers["X-Sequence"] = upstream.headers["x-sequence"]
    return Response(upstream.content, media_type="image/jpeg", headers=headers)


@app.get("/api/stream.mjpg")
async def stream(request: Request):
    global PREVIEW_CLIENTS
    require_login(request)
    async with PREVIEW_CLIENTS_LOCK:
        if PREVIEW_CLIENTS >= 3:
            raise HTTPException(503, "Preview client limit reached")
        PREVIEW_CLIENTS += 1
    client = httpx.AsyncClient(timeout=None)
    try:
        headers = core_headers()
        upstream = await client.send(client.build_request(
            "GET", f"{CORE_URL}/api/stream.mjpg",
            headers={**headers, "Accept-Encoding": "identity"}), stream=True)
        if upstream.status_code != 200:
            await upstream.aclose()
            raise HTTPException(503, "Core preview stream is unavailable")
    except Exception:
        await client.aclose()
        async with PREVIEW_CLIENTS_LOCK:
            PREVIEW_CLIENTS -= 1
        raise

    async def relay():
        global PREVIEW_CLIENTS
        try:
            async for chunk in upstream.aiter_bytes():
                if await request.is_disconnected(): break
                yield chunk
        except httpx.HTTPError:
            return
        finally:
            await upstream.aclose()
            await client.aclose()
            async with PREVIEW_CLIENTS_LOCK:
                PREVIEW_CLIENTS -= 1

    return StreamingResponse(relay(), media_type="multipart/x-mixed-replace; boundary=frame",
                             headers={"Cache-Control": "no-store, no-transform",
                                      "X-Accel-Buffering": "no"})


@app.get("/{path:path}")
async def static(path: str):
    target = (STATIC / (path or "index.html")).resolve()
    if not target.is_relative_to(STATIC.resolve()) or not target.is_file(): target = STATIC / "index.html"
    return FileResponse(target)
