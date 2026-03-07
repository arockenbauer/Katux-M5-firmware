#!/usr/bin/env python3

import argparse
import asyncio
import concurrent.futures
import contextlib
import logging
import threading
import time
import uuid
from dataclasses import dataclass, field
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from io import BytesIO
from typing import Dict, List, Optional, Tuple
from urllib.parse import parse_qs, urlparse

from PIL import Image
from playwright.async_api import Browser, BrowserContext, Page, async_playwright

logger = logging.getLogger("rbrowser_server")

FOCUS_SCRIPT = """() => {
    const el = document.activeElement;
    if (!el) return false;
    if (el.isContentEditable) return true;
    const tag = (el.tagName || '').toLowerCase();
    if (tag === 'textarea') return true;
    if (tag !== 'input') return false;
    const blocked = new Set(['button', 'submit', 'checkbox', 'radio', 'file', 'image', 'reset', 'range', 'color', 'hidden']);
    return !blocked.has((el.type || 'text').toLowerCase());
}"""

FIT_PAGE_SCRIPT = """(size) => {
    const html = document.documentElement;
    const body = document.body;
    if (!html) return {scale: 1, width: 0, height: 0};
    html.style.transform = 'scale(1)';
    html.style.transformOrigin = 'top left';
    html.style.overflow = 'hidden';
    if (body) {
        body.style.overflow = 'hidden';
    }
    const naturalWidth = Math.max(html.scrollWidth || 0, html.clientWidth || 0, body ? body.scrollWidth || 0 : 0, body ? body.clientWidth || 0 : 0, 1);
    const naturalHeight = Math.max(html.scrollHeight || 0, html.clientHeight || 0, body ? body.scrollHeight || 0 : 0, body ? body.clientHeight || 0 : 0, 1);
    let scale = Math.min(size.width / naturalWidth, size.height / naturalHeight);
    if (!Number.isFinite(scale) || scale <= 0 || scale > 1) scale = 1;
    html.style.width = `${naturalWidth}px`;
    html.style.minHeight = `${naturalHeight}px`;
    html.style.transform = `scale(${scale})`;
    window.scrollTo(0, 0);
    return {scale, width: naturalWidth, height: naturalHeight};
}"""


@dataclass
class Rect:
    x: int
    y: int
    w: int
    h: int

    @property
    def right(self) -> int:
        return self.x + self.w

    @property
    def bottom(self) -> int:
        return self.y + self.h

    @property
    def area(self) -> int:
        return self.w * self.h


@dataclass
class SessionState:
    id: str
    context: BrowserContext
    page: Page
    width: int
    height: int
    home_url: str = "https://www.google.com"
    current_url: str = "https://www.google.com"
    title: str = "Google"
    focus_text: bool = False
    frame: bytes = b""
    frame_seq: int = 0
    delivered_seq: int = 0
    pending_full: bool = True
    pending_regions: List[Rect] = field(default_factory=list)
    last_capture_at: float = 0.0
    last_activity_at: float = field(default_factory=time.monotonic)
    closed: bool = False
    capture_task: Optional[asyncio.Task] = None


def normalize_url(value: str) -> str:
    text = (value or "").strip()
    if not text:
        return "about:blank"
    if text.startswith(("http://", "https://", "about:", "file://", "data:")):
        return text
    return f"https://{text}"


def header_safe(value: str, limit: int) -> str:
    return (value or "").replace("\r", " ").replace("\n", " ")[:limit]


def clamp_int(value: int, lo: int, hi: int) -> int:
    if value < lo:
        return lo
    if value > hi:
        return hi
    return value


def rects_touch_or_overlap(a: Rect, b: Rect, gap: int = 1) -> bool:
    return not (
        a.right + gap < b.x or b.right + gap < a.x or a.bottom + gap < b.y or b.bottom + gap < a.y
    )


def union_rect(a: Rect, b: Rect) -> Rect:
    x1 = min(a.x, b.x)
    y1 = min(a.y, b.y)
    x2 = max(a.right, b.right)
    y2 = max(a.bottom, b.bottom)
    return Rect(x1, y1, x2 - x1, y2 - y1)


def merge_rects(rects: List[Rect], limit: int = 64) -> List[Rect]:
    work = [Rect(r.x, r.y, r.w, r.h) for r in rects if r.w > 0 and r.h > 0]
    changed = True
    while changed and len(work) > 1:
        changed = False
        merged: List[Rect] = []
        while work:
            base = work.pop()
            index = 0
            while index < len(work):
                if rects_touch_or_overlap(base, work[index], gap=1):
                    base = union_rect(base, work.pop(index))
                    changed = True
                else:
                    index += 1
            merged.append(base)
        work = merged
    work.sort(key=lambda item: (item.y, item.x))
    if len(work) <= limit:
        return work
    collapsed = work[:]
    while len(collapsed) > limit:
        best_i = 0
        best_j = 1
        best_cost = union_rect(collapsed[0], collapsed[1]).area - collapsed[0].area - collapsed[1].area
        for i in range(len(collapsed)):
            for j in range(i + 1, len(collapsed)):
                candidate = union_rect(collapsed[i], collapsed[j])
                cost = candidate.area - collapsed[i].area - collapsed[j].area
                if cost < best_cost:
                    best_cost = cost
                    best_i = i
                    best_j = j
        merged_rect = union_rect(collapsed[best_i], collapsed[best_j])
        next_rects: List[Rect] = []
        for index, rect in enumerate(collapsed):
            if index not in (best_i, best_j):
                next_rects.append(rect)
        next_rects.append(merged_rect)
        collapsed = next_rects
    collapsed.sort(key=lambda item: (item.y, item.x))
    return collapsed


def rgb565_from_image(image: Image.Image) -> bytes:
    rgb = image.convert("RGB").tobytes()
    out = bytearray((len(rgb) // 3) * 2)
    write = 0
    for index in range(0, len(rgb), 3):
        r = rgb[index]
        g = rgb[index + 1]
        b = rgb[index + 2]
        value = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        out[write] = value & 0xFF
        out[write + 1] = (value >> 8) & 0xFF
        write += 2
    return bytes(out)


def detect_dirty_regions(previous: Optional[bytes], current: bytes, width: int, height: int) -> Tuple[bool, List[Rect], int]:
    if previous is None or len(previous) != len(current):
        return True, [Rect(0, 0, width, height)], width * height
    changed_pixels = 0
    row_rects: List[Rect] = []
    for y in range(height):
        row_offset = y * width * 2
        x = 0
        while x < width:
            pixel_offset = row_offset + x * 2
            if previous[pixel_offset : pixel_offset + 2] == current[pixel_offset : pixel_offset + 2]:
                x += 1
                continue
            start = x
            x += 1
            while x < width:
                pixel_offset = row_offset + x * 2
                if previous[pixel_offset : pixel_offset + 2] == current[pixel_offset : pixel_offset + 2]:
                    break
                x += 1
            width_run = x - start
            changed_pixels += width_run
            row_rects.append(Rect(start, y, width_run, 1))
    if not row_rects:
        return False, [], 0
    total_pixels = width * height
    if changed_pixels >= int(total_pixels * 0.28):
        return True, [Rect(0, 0, width, height)], changed_pixels
    merged = merge_rects(row_rects, limit=48)
    merged_area = sum(rect.area for rect in merged)
    if merged_area >= int(total_pixels * 0.55) or len(merged) > 24:
        return True, [Rect(0, 0, width, height)], changed_pixels
    return False, merged, changed_pixels


def encode_row_tokens(row: bytes, width: int) -> bytes:
    out = bytearray()
    pixels = [row[index * 2 : index * 2 + 2] for index in range(width)]
    index = 0
    while index < width:
        run_len = 1
        while index + run_len < width and run_len < 128 and pixels[index + run_len] == pixels[index]:
            run_len += 1
        if run_len >= 3:
            out.append(0x80 | (run_len - 1))
            out.extend(pixels[index])
            index += run_len
            continue
        literal_start = index
        literal_len = 0
        while index < width and literal_len < 128:
            next_run = 1
            while index + next_run < width and next_run < 128 and pixels[index + next_run] == pixels[index]:
                next_run += 1
            if literal_len > 0 and next_run >= 3:
                break
            index += 1
            literal_len += 1
        out.append(literal_len - 1)
        for pixel in pixels[literal_start : literal_start + literal_len]:
            out.extend(pixel)
    return bytes(out)


def encode_regions(frame: bytes, frame_width: int, regions: List[Rect]) -> bytes:
    out = bytearray()
    for rect in regions:
        payload = bytearray()
        for row in range(rect.h):
            row_y = rect.y + row
            start = (row_y * frame_width + rect.x) * 2
            end = start + rect.w * 2
            row_bytes = frame[start:end]
            encoded = encode_row_tokens(row_bytes, rect.w)
            payload.extend(len(encoded).to_bytes(2, "little"))
            payload.extend(encoded)
        out.extend(rect.x.to_bytes(2, "little"))
        out.extend(rect.y.to_bytes(2, "little"))
        out.extend(rect.w.to_bytes(2, "little"))
        out.extend(rect.h.to_bytes(2, "little"))
        out.extend(len(payload).to_bytes(4, "little"))
        out.extend(payload)
    return bytes(out)


class BrowserService:
    def __init__(self) -> None:
        self.loop: Optional[asyncio.AbstractEventLoop] = None
        self.thread: Optional[threading.Thread] = None
        self.playwright = None
        self.browser: Optional[Browser] = None
        self.sessions: Dict[str, SessionState] = {}
        self._loop_ready = threading.Event()

    def start(self) -> None:
        logger.info("Starting browser service thread")
        self.thread = threading.Thread(target=self._thread_main, name="rbrowser-loop", daemon=True)
        self.thread.start()
        self._loop_ready.wait()
        self._submit(self._async_start()).result()
        logger.info("Browser service thread ready")

    def close(self) -> None:
        if not self.loop:
            return
        logger.info("Stopping browser service")
        self._submit(self._async_close()).result()
        self.loop.call_soon_threadsafe(self.loop.stop)
        if self.thread:
            self.thread.join(timeout=2.0)
        logger.info("Browser service stopped")

    def _thread_main(self) -> None:
        self.loop = asyncio.new_event_loop()
        asyncio.set_event_loop(self.loop)
        self._loop_ready.set()
        self.loop.run_forever()
        pending = asyncio.all_tasks(self.loop)
        for task in pending:
            task.cancel()
        with contextlib.suppress(Exception):
            self.loop.run_until_complete(asyncio.gather(*pending, return_exceptions=True))
        self.loop.close()

    def _submit(self, coro):
        if not self.loop:
            raise RuntimeError("Event loop not started")
        return asyncio.run_coroutine_threadsafe(coro, self.loop)

    async def _async_start(self) -> None:
        logger.info("Launching headless Chromium")
        self.playwright = await async_playwright().start()
        self.browser = await self.playwright.chromium.launch(
            headless=True,
            args=[
                "--disable-dev-shm-usage",
                "--disable-gpu",
                "--disable-background-networking",
                "--disable-renderer-backgrounding",
                "--disable-background-timer-throttling",
            ],
        )
        logger.info("Headless Chromium launched")

    async def _async_close(self) -> None:
        session_ids = list(self.sessions.keys())
        logger.info("Closing %d active session(s)", len(session_ids))
        for session_id in session_ids:
            with contextlib.suppress(Exception):
                await self._close_session(session_id)
        if self.browser is not None:
            await self.browser.close()
            self.browser = None
            logger.info("Chromium closed")
        if self.playwright is not None:
            await self.playwright.stop()
            self.playwright = None
            logger.info("Playwright stopped")

    def open_session(self, width: int, height: int) -> Dict[str, str]:
        return self._submit(self._open_session(width, height)).result()

    def resize_session(self, session_id: str, width: int, height: int) -> bool:
        return self._submit(self._resize_session(session_id, width, height)).result()

    def pointer(self, session_id: str, x: int, y: int) -> bool:
        return self._submit(self._pointer(session_id, x, y)).result()

    def click(self, session_id: str, x: int, y: int) -> bool:
        return self._submit(self._click(session_id, x, y)).result()

    def scroll(self, session_id: str, delta_y: int) -> bool:
        return self._submit(self._scroll(session_id, delta_y)).result()

    def navigate(self, session_id: str, url: str) -> bool:
        return self._submit(self._navigate(session_id, url)).result()

    def action(self, session_id: str, kind: str) -> bool:
        return self._submit(self._action(session_id, kind)).result()

    def send_text(self, session_id: str, text: str) -> bool:
        return self._submit(self._send_text(session_id, text)).result()

    def close_session(self, session_id: str) -> bool:
        try:
            return self._submit(self._close_session_safe(session_id)).result()
        except (asyncio.CancelledError, concurrent.futures.CancelledError):
            logger.warning("Session %s close future cancelled, treating as closed", session_id)
            self.sessions.pop(session_id, None)
            return True

    def frame_packet(self, session_id: str, client_seq: int, full_requested: bool) -> Tuple[int, Dict[str, str], bytes]:
        return self._submit(self._frame_packet(session_id, client_seq, full_requested)).result()

    async def _open_session(self, width: int, height: int) -> Dict[str, str]:
        if self.browser is None:
            raise RuntimeError("Browser not started")
        viewport = {"width": max(4, int(width)), "height": max(4, int(height))}
        context = await self.browser.new_context(viewport=viewport, ignore_https_errors=True, java_script_enabled=True)
        page = await context.new_page()
        page.set_default_timeout(15000)
        session_id = uuid.uuid4().hex[:16]
        session = SessionState(id=session_id, context=context, page=page, width=viewport["width"], height=viewport["height"])
        session.home_url = normalize_url("google.com")
        session.current_url = session.home_url
        self.sessions[session_id] = session
        await page.goto(session.home_url, wait_until="domcontentloaded")
        await self._fit_page_to_viewport(session)
        session.capture_task = asyncio.create_task(self._capture_loop(session))
        await self._capture(session, force_full=True)
        logger.info("Session %s opened (%dx%d)", session.id, session.width, session.height)
        return {
            "session": session.id,
            "url": session.current_url,
            "title": session.title,
            "width": str(session.width),
            "height": str(session.height),
        }

    async def _close_session(self, session_id: str) -> None:
        session = self._get_session(session_id)
        session.closed = True
        current_task = asyncio.current_task()
        if session.capture_task is not None and session.capture_task is not current_task:
            session.capture_task.cancel()
            with contextlib.suppress(asyncio.CancelledError, Exception):
                await session.capture_task
        await session.context.close()
        self.sessions.pop(session_id, None)
        logger.info("Session %s closed", session_id)

    async def _close_session_safe(self, session_id: str) -> bool:
        session = self.sessions.get(session_id)
        if session is None or session.closed:
            logger.info("Session %s already closed", session_id)
            self.sessions.pop(session_id, None)
            return True
        await self._close_session(session_id)
        return True

    async def _resize_session(self, session_id: str, width: int, height: int) -> bool:
        session = self._get_session(session_id)
        session.width = max(4, int(width))
        session.height = max(4, int(height))
        await session.page.set_viewport_size({"width": session.width, "height": session.height})
        await self._fit_page_to_viewport(session)
        session.frame = b""
        session.pending_full = True
        session.pending_regions.clear()
        session.last_activity_at = time.monotonic()
        await self._capture(session, force_full=True)
        logger.info("Session %s resized to %dx%d", session.id, session.width, session.height)
        return True

    async def _pointer(self, session_id: str, x: int, y: int) -> bool:
        session = self._get_session(session_id)
        px = clamp_int(int(x), 0, session.width - 1)
        py = clamp_int(int(y), 0, session.height - 1)
        await session.page.mouse.move(px, py)
        session.last_activity_at = time.monotonic()
        return True

    async def _click(self, session_id: str, x: int, y: int) -> bool:
        session = self._get_session(session_id)
        px = clamp_int(int(x), 0, session.width - 1)
        py = clamp_int(int(y), 0, session.height - 1)
        await session.page.mouse.move(px, py)
        await session.page.mouse.click(px, py)
        session.last_activity_at = time.monotonic()
        await asyncio.sleep(0.03)
        await self._capture(session)
        return True

    async def _scroll(self, session_id: str, delta_y: int) -> bool:
        session = self._get_session(session_id)
        await session.page.mouse.wheel(0, int(delta_y))
        session.last_activity_at = time.monotonic()
        await asyncio.sleep(0.03)
        await self._capture(session)
        return True

    async def _navigate(self, session_id: str, url: str) -> bool:
        session = self._get_session(session_id)
        target = normalize_url(url)
        session.home_url = target
        logger.info("Session %s navigate -> %s", session.id, header_safe(target, 190))
        await session.page.goto(target, wait_until="domcontentloaded")
        await self._fit_page_to_viewport(session)
        session.last_activity_at = time.monotonic()
        session.pending_full = True
        session.pending_regions.clear()
        await self._capture(session, force_full=True)
        return True

    async def _action(self, session_id: str, kind: str) -> bool:
        session = self._get_session(session_id)
        action = (kind or "").strip().lower()
        logger.info("Session %s action=%s", session.id, action)
        if action == "back":
            await session.page.go_back(wait_until="domcontentloaded")
        elif action == "forward":
            await session.page.go_forward(wait_until="domcontentloaded")
        elif action == "reload":
            await session.page.reload(wait_until="domcontentloaded")
        elif action == "home":
            await session.page.goto(session.home_url or "about:blank", wait_until="domcontentloaded")
        else:
            raise ValueError("Unknown action")
        await self._fit_page_to_viewport(session)
        session.last_activity_at = time.monotonic()
        session.pending_full = True
        session.pending_regions.clear()
        await self._capture(session, force_full=True)
        return True

    async def _send_text(self, session_id: str, text: str) -> bool:
        session = self._get_session(session_id)
        logger.info("Session %s text input (%d chars)", session.id, len(text or ""))
        await session.page.keyboard.insert_text(text or "")
        session.last_activity_at = time.monotonic()
        await asyncio.sleep(0.03)
        await self._capture(session)
        return True

    async def _frame_packet(self, session_id: str, client_seq: int, full_requested: bool) -> Tuple[int, Dict[str, str], bytes]:
        session = self._get_session(session_id)
        if not session.frame:
            await self._capture(session, force_full=True)
        elif time.monotonic() - session.last_capture_at >= 0.16:
            await self._capture(session)
        if not session.frame:
            return 204, {}, b""
        must_send_full = full_requested or session.pending_full or int(client_seq) != session.delivered_seq
        if not must_send_full and not session.pending_regions:
            return 204, {}, b""
        regions = [Rect(0, 0, session.width, session.height)] if must_send_full else merge_rects(session.pending_regions)
        body = encode_regions(session.frame, session.width, regions)
        if must_send_full:
            logger.info("Session %s frame seq=%d full=%s regions=%d bytes=%d client_seq=%d", session.id, session.frame_seq, must_send_full, len(regions), len(body), int(client_seq))
        session.delivered_seq = session.frame_seq
        session.pending_full = False
        session.pending_regions.clear()
        headers = {
            "X-RBrowser-Seq": str(session.frame_seq),
            "X-RBrowser-Width": str(session.width),
            "X-RBrowser-Height": str(session.height),
            "X-RBrowser-Regions": str(len(regions)),
            "X-RBrowser-Focus-Text": "1" if session.focus_text else "0",
            "X-RBrowser-Full": "1" if must_send_full else "0",
            "X-RBrowser-Url": header_safe(session.current_url, 190),
            "X-RBrowser-Title": header_safe(session.title, 43),
        }
        return 200, headers, body

    async def _capture_loop(self, session: SessionState) -> None:
        try:
            while not session.closed:
                await self._capture(session)
                if time.monotonic() - session.last_activity_at > 1800:
                    logger.info("Session %s expired after inactivity", session.id)
                    break
                await asyncio.sleep(0.08)
        except asyncio.CancelledError:
            raise
        except Exception:
            session.pending_full = True
            logger.exception("Capture loop failed for session %s", session.id)
        finally:
            if not session.closed and session.id in self.sessions:
                with contextlib.suppress(Exception):
                    await self._close_session(session.id)

    async def _fit_page_to_viewport(self, session: SessionState) -> None:
        if session.closed:
            return
        try:
            fit = await session.page.evaluate(FIT_PAGE_SCRIPT, {"width": session.width, "height": session.height})
            if isinstance(fit, dict):
                logger.debug(
                    "Session %s fit scale=%.4f natural=%sx%s viewport=%dx%d",
                    session.id,
                    float(fit.get("scale", 1.0)),
                    fit.get("width", 0),
                    fit.get("height", 0),
                    session.width,
                    session.height,
                )
        except Exception:
            logger.exception("Session %s fit-to-window failed", session.id)

    async def _capture(self, session: SessionState, force_full: bool = False) -> bool:
        if session.closed:
            return False
        await self._fit_page_to_viewport(session)
        png = await session.page.screenshot(type="png")
        with Image.open(BytesIO(png)) as image:
            if image.size != (session.width, session.height):
                image = image.resize((session.width, session.height))
            frame = rgb565_from_image(image)
        current_url = session.page.url or session.current_url or "about:blank"
        try:
            title = await session.page.title()
        except Exception:
            title = session.title
        try:
            focus_text = bool(await session.page.evaluate(FOCUS_SCRIPT))
        except Exception:
            focus_text = False
        changed = False
        full_frame = False
        regions: List[Rect] = []
        if not session.frame or len(session.frame) != len(frame):
            changed = True
            full_frame = True
            regions = [Rect(0, 0, session.width, session.height)]
        elif frame != session.frame:
            changed = True
            full_frame, regions, _ = detect_dirty_regions(session.frame, frame, session.width, session.height)
        session.current_url = (current_url or "about:blank")[:190]
        session.title = (title or "R-Browser")[:43] or "R-Browser"
        session.focus_text = focus_text
        session.last_capture_at = time.monotonic()
        if changed:
            session.frame = frame
            session.frame_seq += 1
            if force_full or full_frame or session.pending_full:
                session.pending_full = True
                session.pending_regions.clear()
            else:
                session.pending_regions = merge_rects(session.pending_regions + regions)
        return changed

    def _get_session(self, session_id: str) -> SessionState:
        session = self.sessions.get(session_id)
        if session is None or session.closed:
            raise KeyError("Unknown session")
        return session


class RBrowserHandler(BaseHTTPRequestHandler):
    server_version = "KatuxRBrowser/1.0"
    service: BrowserService = None

    def log_message(self, format: str, *args) -> None:
        return

    def do_GET(self) -> None:
        try:
            parsed = urlparse(self.path)
            params = parse_qs(parsed.query)
            if parsed.path == "/rb/discover":
                logger.info("Discover request from %s", self.client_address[0])
                body = "Katux R-Browser\nport=16767\ntransport=http\n"
                self._send_text(200, body)
                return
            if parsed.path == "/rb/session/frame":
                session_id = self._required_param(params, "session")
                seq = int(self._param(params, "seq", "0"))
                full = self._param(params, "full", "0") == "1"
                status, headers, body = self.service.frame_packet(session_id, seq, full)
                if status == 204:
                    self.send_response(204)
                    self.end_headers()
                    return
                self._send_binary(status, body, headers)
                return
            logger.warning("Unknown GET path from %s: %s", self.client_address[0], parsed.path)
            self._send_text(404, "Not found")
        except KeyError as exc:
            logger.warning("GET %s -> unknown session: %s", self.path, exc)
            self._send_text(404, str(exc))
        except ValueError as exc:
            logger.warning("GET %s -> bad request: %s", self.path, exc)
            self._send_text(400, str(exc))
        except Exception as exc:
            logger.exception("GET %s failed", self.path)
            self._send_text(500, str(exc))

    def do_POST(self) -> None:
        try:
            parsed = urlparse(self.path)
            params = parse_qs(parsed.query)
            body = self._read_body()
            if parsed.path == "/rb/session/open":
                width = int(self._param(params, "width", "140"))
                height = int(self._param(params, "height", "90"))
                logger.info("Session open request from %s (%dx%d)", self.client_address[0], width, height)
                payload = self.service.open_session(width, height)
                response = "\n".join(f"{key}={str(value).replace(chr(10), ' ').replace(chr(13), ' ')}" for key, value in payload.items()) + "\n"
                self._send_text(200, response)
                return
            session_id = self._required_param(params, "session")
            if parsed.path == "/rb/session/resize":
                width = int(self._param(params, "width", "140"))
                height = int(self._param(params, "height", "90"))
                self.service.resize_session(session_id, width, height)
                self._send_text(200, "ok\n")
                return
            if parsed.path == "/rb/session/pointer":
                x = int(self._param(params, "x", "0"))
                y = int(self._param(params, "y", "0"))
                self.service.pointer(session_id, x, y)
                self._send_text(200, "ok\n")
                return
            if parsed.path == "/rb/session/click":
                x = int(self._param(params, "x", "0"))
                y = int(self._param(params, "y", "0"))
                logger.info("Session %s click at %d,%d", session_id, x, y)
                self.service.click(session_id, x, y)
                self._send_text(200, "ok\n")
                return
            if parsed.path == "/rb/session/scroll":
                dy = int(self._param(params, "dy", "0"))
                logger.info("Session %s scroll dy=%d", session_id, dy)
                self.service.scroll(session_id, dy)
                self._send_text(200, "ok\n")
                return
            if parsed.path == "/rb/session/navigate":
                text = body.decode("utf-8", errors="ignore")
                logger.info("Session %s navigate request -> %s", session_id, header_safe(text, 190))
                self.service.navigate(session_id, text)
                self._send_text(200, "ok\n")
                return
            if parsed.path == "/rb/session/action":
                kind = self._required_param(params, "kind")
                self.service.action(session_id, kind)
                self._send_text(200, "ok\n")
                return
            if parsed.path == "/rb/session/text":
                text = body.decode("utf-8", errors="ignore")
                logger.info("Session %s text request (%d chars)", session_id, len(text))
                self.service.send_text(session_id, text)
                self._send_text(200, "ok\n")
                return
            if parsed.path == "/rb/session/close":
                logger.info("Session %s close request from %s", session_id, self.client_address[0])
                self.service.close_session(session_id)
                self._send_text(200, "ok\n")
                return
            logger.warning("Unknown POST path from %s: %s", self.client_address[0], parsed.path)
            self._send_text(404, "Not found")
        except KeyError as exc:
            logger.warning("POST %s -> unknown session: %s", self.path, exc)
            self._send_text(404, str(exc))
        except ValueError as exc:
            logger.warning("POST %s -> bad request: %s", self.path, exc)
            self._send_text(400, str(exc))
        except Exception as exc:
            logger.exception("POST %s failed", self.path)
            self._send_text(500, str(exc))

    def _read_body(self) -> bytes:
        length = int(self.headers.get("Content-Length", "0"))
        if length <= 0:
            return b""
        return self.rfile.read(length)

    def _param(self, params: Dict[str, List[str]], name: str, default: str = "") -> str:
        values = params.get(name)
        if not values:
            return default
        return values[0]

    def _required_param(self, params: Dict[str, List[str]], name: str) -> str:
        value = self._param(params, name, "")
        if not value:
            raise ValueError(f"Missing parameter: {name}")
        return value

    def _send_text(self, status: int, text: str) -> None:
        payload = text.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(payload)

    def _send_binary(self, status: int, payload: bytes, headers: Dict[str, str]) -> None:
        self.send_response(status)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Cache-Control", "no-store")
        for key, value in headers.items():
            self.send_header(key, value)
        self.end_headers()
        self.wfile.write(payload)


def main() -> None:
    parser = argparse.ArgumentParser(prog="rbrowser_server")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=16767)
    args = parser.parse_args()

    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )

    logger.info("Starting R-Browser server on %s:%d", args.host, args.port)
    service = BrowserService()
    service.start()
    RBrowserHandler.service = service
    server = ThreadingHTTPServer((args.host, args.port), RBrowserHandler)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        logger.info("Keyboard interrupt received, shutting down")
    finally:
        server.server_close()
        service.close()
        logger.info("R-Browser server stopped")


if __name__ == "__main__":
    main()
