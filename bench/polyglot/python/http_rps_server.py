#!/usr/bin/env python3
"""Contract-compatible server for the Ember soak-client RPS benchmark."""

from __future__ import annotations

import argparse
from copy import deepcopy
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import threading
import time
from typing import Optional, Tuple
from urllib.parse import parse_qs, urlsplit


REQUIRED_FIELDS = {
    "sku",
    "name",
    "description",
    "category",
    "status",
    "price_cents",
    "currency",
    "stock_count",
    "weight_grams",
    "active",
    "manufacturer",
    "country_code",
    "barcode",
    "color",
    "size",
    "rating_milli",
    "tags",
    "metadata",
}
PATCH_FIELDS = REQUIRED_FIELDS
INTEGER_FIELDS = {
    "price_cents",
    "stock_count",
    "weight_grams",
    "rating_milli",
}
STRING_FIELDS = REQUIRED_FIELDS - INTEGER_FIELDS - {"active", "tags", "metadata"}


def schema_error(item: object, *, patch: bool = False) -> Optional[str]:
    if not isinstance(item, dict):
        return "item must be an object"
    keys = set(item)
    if keys - PATCH_FIELDS:
        return "unknown attribute"
    if patch:
        if not keys:
            return None
    elif keys != REQUIRED_FIELDS:
        return "missing required attributes"
    for field in STRING_FIELDS:
        if field in item and not isinstance(item[field], str):
            return f"{field} must be a string"
    for field in INTEGER_FIELDS:
        if field in item and type(item[field]) is not int:
            return f"{field} must be an integer"
    if "active" in item and type(item["active"]) is not bool:
        return "active must be boolean"
    if "tags" in item:
        tags = item["tags"]
        if not isinstance(tags, list) or any(not isinstance(tag, str) for tag in tags):
            return "tags must be an array of strings"
    if "metadata" in item:
        metadata = item["metadata"]
        if not isinstance(metadata, dict) or set(metadata) != {"source", "batch", "fragile"}:
            return "metadata has invalid fields"
        if not isinstance(metadata["source"], str) or not isinstance(metadata["batch"], str):
            return "metadata strings are invalid"
        if type(metadata["fragile"]) is not bool:
            return "metadata.fragile must be boolean"
    return None


def bounded_text(value: str, minimum: int, maximum: int) -> bool:
    return minimum <= len(value) <= maximum


def model_error(item: dict) -> Optional[str]:
    checks = (
        ("sku", bounded_text(item["sku"], 3, 64) and all(c in "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_" for c in item["sku"])),
        ("name", bounded_text(item["name"], 1, 160)),
        ("description", bounded_text(item["description"], 1, 2000)),
        ("category", item["category"] in {"hardware", "software", "service", "accessory"}),
        ("status", item["status"] in {"draft", "active", "paused", "retired"}),
        ("price_cents", 0 <= item["price_cents"] <= 1_000_000_000),
        ("currency", len(item["currency"]) == 3 and item["currency"].isupper() and item["currency"].isascii()),
        ("stock_count", 0 <= item["stock_count"] <= 1_000_000),
        ("weight_grams", 1 <= item["weight_grams"] <= 10_000_000),
        ("manufacturer", bounded_text(item["manufacturer"], 1, 120)),
        ("country_code", len(item["country_code"]) == 2 and item["country_code"].isupper() and item["country_code"].isascii()),
        ("barcode", bounded_text(item["barcode"], 8, 32) and item["barcode"].isascii() and item["barcode"].isdigit()),
        ("color", bounded_text(item["color"], 1, 40)),
        ("size", bounded_text(item["size"], 1, 40)),
        ("rating_milli", 0 <= item["rating_milli"] <= 5000),
        ("tags", len(item["tags"]) <= 16 and all(bounded_text(tag, 1, 32) for tag in item["tags"])),
        ("metadata", bounded_text(item["metadata"]["source"], 1, 64) and bounded_text(item["metadata"]["batch"], 1, 64)),
    )
    return next((field for field, valid in checks if not valid), None)


class Store:
    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.next_id = 1
        self.items: dict[int, dict] = {}
        self.skus: dict[str, int] = {}

    @staticmethod
    def public(record: dict) -> dict:
        return deepcopy(record)


STORE = Store()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "polyglot-python"
    sys_version = ""

    def log_message(self, _format: str, *_args: object) -> None:
        return

    def send_payload(self, status: int, payload: Optional[object] = None, *, close: bool = False) -> None:
        body = b"" if payload is None else json.dumps(payload, separators=(",", ":")).encode()
        self.send_response(status)
        if body:
            self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        if close:
            self.send_header("Connection", "close")
            self.close_connection = True
        self.end_headers()
        if body:
            self.wfile.write(body)

    def problem(self, status: int, code: str = "request_error", field: Optional[str] = None) -> None:
        details = [] if field is None else [{"field": field, "code": "invalid"}]
        self.send_payload(status, {"error": {"code": code, "message": code, "details": details}})

    def read_body(self) -> Optional[bytes]:
        try:
            length = int(self.headers.get("content-length", "0"))
        except ValueError:
            self.problem(400)
            return None
        if length > 65536:
            self.rfile.read(length)
            self.problem(413)
            return None
        return self.rfile.read(length)

    def read_item(self, body: bytes, *, patch: bool = False) -> Optional[dict]:
        if not self.headers.get("content-type", "").lower().startswith("application/json"):
            self.problem(415)
            return None
        try:
            document = json.loads(body)
        except (UnicodeDecodeError, json.JSONDecodeError):
            self.problem(400)
            return None
        if not isinstance(document, dict) or not isinstance(document.get("item"), dict):
            self.problem(400)
            return None
        item = document["item"]
        error = schema_error(item, patch=patch)
        if error is not None:
            self.problem(400)
            return None
        return item

    def checked_if_match(self, record: dict) -> Optional[int]:
        text = self.headers.get("if-match")
        if text is None:
            self.problem(428)
            return None
        try:
            version = int(text)
        except ValueError:
            self.problem(400)
            return None
        if version != record["version"]:
            self.problem(409)
            return None
        return version

    def route(self) -> Optional[Tuple[str, Optional[int], dict]]:
        parsed = urlsplit(self.path)
        if parsed.path == "/health/ready":
            return "health", None, parse_qs(parsed.query, keep_blank_values=True)
        if parsed.path == "/v1/items":
            return "collection", None, parse_qs(parsed.query, keep_blank_values=True)
        prefix = "/v1/items/"
        if parsed.path.startswith(prefix):
            text = parsed.path[len(prefix):]
            if not text.isdigit():
                self.problem(400)
                return None
            return "member", int(text), parse_qs(parsed.query, keep_blank_values=True)
        self.problem(404)
        return None

    def authorize_host(self) -> bool:
        host = self.headers.get("host", "")
        if host.startswith("127.0.0.1") or host.startswith("localhost") or host.startswith("[::1]"):
            return True
        self.problem(403)
        return False

    def do_GET(self) -> None:
        if not self.authorize_host():
            return
        route = self.route()
        if route is None:
            return
        kind, item_id, query = route
        if kind == "health":
            self.send_payload(200, {"status": "ready"})
            return
        if kind == "collection":
            if set(query) - {"status"}:
                self.problem(400)
                return
            status = query.get("status", [None])[0]
            if status is not None and status not in {"draft", "active", "paused", "retired"}:
                self.problem(422, "validation_failed", "status")
                return
            with STORE.lock:
                values = [Store.public(item) for item in STORE.items.values() if status is None or item["status"] == status]
            self.send_payload(200, {"data": values})
            return
        with STORE.lock:
            record = STORE.items.get(item_id)
            payload = None if record is None else Store.public(record)
        if payload is None:
            self.problem(404)
        else:
            self.send_payload(200, payload)

    def do_POST(self) -> None:
        body = self.read_body()
        if body is None:
            return
        if not self.authorize_host():
            return
        route = self.route()
        if route is None:
            return
        if route[0] != "collection":
            self.problem(405)
            return
        item = self.read_item(body)
        if item is None:
            return
        invalid = model_error(item)
        if invalid is not None:
            self.problem(422, "validation_failed", invalid)
            return
        now = str(time.time_ns())
        with STORE.lock:
            if item["sku"] in STORE.skus:
                self.problem(409)
                return
            item_id = STORE.next_id
            STORE.next_id += 1
            record = deepcopy(item)
            record.update({"id": item_id, "version": 1, "created_at": now, "updated_at": now})
            STORE.items[item_id] = record
            STORE.skus[item["sku"]] = item_id
            payload = Store.public(record)
        self.send_payload(201, payload)

    def mutate(self, *, patch: bool) -> None:
        body = self.read_body()
        if body is None:
            return
        if not self.authorize_host():
            return
        route = self.route()
        if route is None:
            return
        kind, item_id, _query = route
        if kind != "member":
            self.problem(405)
            return
        with STORE.lock:
            current = STORE.items.get(item_id)
            snapshot = None if current is None else Store.public(current)
        if snapshot is None:
            self.problem(404)
            return
        if self.checked_if_match(snapshot) is None:
            return
        incoming = self.read_item(body, patch=patch)
        if incoming is None:
            return
        if patch and not incoming:
            self.problem(422, "validation_failed")
            return
        candidate = deepcopy(snapshot) if patch else deepcopy(incoming)
        if patch:
            candidate.update(incoming)
        invalid = model_error(candidate)
        if invalid is not None:
            self.problem(422, "validation_failed", invalid)
            return
        with STORE.lock:
            current = STORE.items.get(item_id)
            if current is None:
                self.problem(404)
                return
            if current["version"] != snapshot["version"]:
                self.problem(409)
                return
            old_sku = current["sku"]
            new_sku = candidate["sku"]
            owner = STORE.skus.get(new_sku)
            if owner is not None and owner != item_id:
                self.problem(409)
                return
            record = {field: deepcopy(candidate[field]) for field in REQUIRED_FIELDS}
            record.update({
                "id": item_id,
                "version": current["version"] + 1,
                "created_at": current["created_at"],
                "updated_at": str(time.time_ns()),
            })
            if old_sku != new_sku:
                STORE.skus.pop(old_sku, None)
                STORE.skus[new_sku] = item_id
            STORE.items[item_id] = record
            payload = Store.public(record)
        self.send_payload(200, payload)

    def do_PUT(self) -> None:
        self.mutate(patch=False)

    def do_PATCH(self) -> None:
        self.mutate(patch=True)

    def do_DELETE(self) -> None:
        if not self.authorize_host():
            return
        route = self.route()
        if route is None:
            return
        kind, item_id, _query = route
        if kind != "member":
            self.problem(405)
            return
        with STORE.lock:
            current = STORE.items.get(item_id)
            snapshot = None if current is None else Store.public(current)
        if snapshot is None:
            self.problem(404)
            return
        if self.checked_if_match(snapshot) is None:
            return
        with STORE.lock:
            current = STORE.items.pop(item_id, None)
            if current is None:
                self.problem(404)
                return
            STORE.skus.pop(current["sku"], None)
        self.send_payload(204)

    def do_TRACE(self) -> None:
        if not self.authorize_host():
            return
        self.problem(405)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=3400)
    args = parser.parse_args()
    server = ThreadingHTTPServer((args.host, args.port), Handler)
    server.daemon_threads = True
    server.serve_forever(poll_interval=0.1)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
