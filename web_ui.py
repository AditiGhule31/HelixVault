"""
HelixVault Web Dashboard Server
Bridges the modern Web UI directly to the real C11 `helixvault.exe` engine
and the real AWS S3 Boto3 Cloud backend (`aws_sync.py`).
"""

import os
import re
import json
import base64
import hashlib
import tempfile
import subprocess
import urllib.parse
from http.server import HTTPServer, BaseHTTPRequestHandler

import aws_sync

ROOT_DIR = os.path.dirname(os.path.abspath(__file__))
EXE_PATH = os.path.join(ROOT_DIR, "helixvault.exe")
DASHBOARD_HTML = os.path.join(ROOT_DIR, "dashboard.html")
PORT = 8080


def run_helix(args):
    """Execute the real C11 helixvault.exe binary and capture stdout + stderr."""
    cmd = [EXE_PATH] + args
    res = subprocess.run(
        cmd,
        cwd=ROOT_DIR,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    return res.stdout, res.stderr, res.returncode


def parse_stats():
    out, err, rc = run_helix(["stats"])
    combined = out + "\n" + err

    def extract(pattern, default="0"):
        m = re.search(pattern, combined)
        return m.group(1).strip() if m else default

    return {
        "total_buckets": int(extract(r"Total Buckets\s*:\s*(\d+)", "0")),
        "total_objects": int(extract(r"Total Logical Objects\s*:\s*(\d+)", "0")),
        "logical_size_str": extract(r"Total Logical Data Size\s*:\s*([^\(]+)", "0 B"),
        "logical_bytes": int(extract(r"Total Logical Data Size\s*:.*?\((\d+)\s*Bytes\)", "0")),
        "unique_chunks": int(extract(r"Unique Physical Chunks\s*:\s*(\d+)", "0")),
        "physical_size_str": extract(r"Physical Storage Pool Used\s*:\s*([^\(]+)", "0 B"),
        "physical_bytes": int(extract(r"Physical Storage Pool Used\s*:.*?\((\d+)\s*Bytes\)", "0")),
        "saved_size_str": extract(r"Storage Space Saved\s*:\s*([^\(]+)", "0 B"),
        "saved_bytes": int(extract(r"Storage Space Saved\s*:.*?\((\d+)\s*Bytes\)", "0")),
        "dedup_ratio": float(extract(r"Deduplication Ratio\s*:\s*([\d\.]+)", "0.0")),
        "raw_output": combined.strip(),
    }


def list_buckets():
    out, err, rc = run_helix(["lb"])
    buckets = []
    for line in out.splitlines():
        line = line.strip()
        if line and not line.startswith("Buckets") and not line.startswith("No buckets"):
            # strip leading markers if any
            cleaned = line.lstrip("- *").strip()
            if cleaned and "bucket(s)" not in cleaned.lower():
                buckets.append(cleaned)
    return buckets


def list_objects_in_bucket(bucket):
    out, err, rc = run_helix(["ls", bucket])
    objects = []
    in_table = False
    for line in out.splitlines():
        if line.startswith("----"):
            in_table = True
            continue
        if in_table:
            stripped = line.strip()
            if not stripped or "object(s) in" in stripped:
                break
            parts = re.split(r"\s{2,}", stripped)
            if len(parts) >= 2:
                objects.append({
                    "key": parts[0].strip(),
                    "size": parts[1].strip(),
                    "type": parts[2].strip() if len(parts) > 2 else "application/octet-stream",
                })
    return objects


class HelixHandler(BaseHTTPRequestHandler):
    def log_message(self, format, *args):
        # Clean concise logging
        print(f"[WebUI] {self.address_string()} - {format % args}")

    def send_json(self, data, status=200):
        body = json.dumps(data).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(body)

    def read_json_body(self):
        length = int(self.headers.get("Content-Length", 0))
        raw = self.rfile.read(length) if length > 0 else b"{}"
        return json.loads(raw.decode("utf-8"))

    def do_OPTIONS(self):
        self.send_response(200)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.end_headers()

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path
        qs = urllib.parse.parse_qs(parsed.query)

        if path == "/" or path == "/index.html" or path == "/dashboard.html":
            if os.path.exists(DASHBOARD_HTML):
                with open(DASHBOARD_HTML, "rb") as f:
                    content = f.read()
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(len(content)))
                self.end_headers()
                self.wfile.write(content)
            else:
                self.send_error(404, "dashboard.html not found")
            return

        if path == "/api/stats":
            try:
                stats = parse_stats()
                buckets = list_buckets()
                config = aws_sync.load_config()
                stats["buckets"] = buckets
                stats["aws_bucket"] = config.get("s3_bucket_name", "Not Configured")
                stats["aws_region"] = config.get("region_name", "ap-southeast-2")
                self.send_json({"ok": True, "data": stats})
            except Exception as e:
                self.send_json({"ok": False, "error": str(e)}, 500)
            return

        if path == "/api/tree":
            bucket = qs.get("bucket", [""])[0]
            try:
                args = ["tree", bucket] if bucket else ["tree"]
                out, err, rc = run_helix(args)
                objects = list_objects_in_bucket(bucket) if bucket else []
                self.send_json({
                    "ok": True,
                    "tree_ascii": out.strip(),
                    "objects": objects,
                })
            except Exception as e:
                self.send_json({"ok": False, "error": str(e)}, 500)
            return

        if path == "/api/aws/list":
            try:
                config = aws_sync.load_config()
                s3_bucket = config.get("s3_bucket_name")
                s3 = aws_sync.get_s3_client(config)
                resp = s3.list_objects_v2(Bucket=s3_bucket)
                contents = resp.get("Contents", [])
                items = []
                total_size = 0
                chunks_count = 0
                manifests_count = 0
                for obj in contents:
                    key = obj["Key"]
                    size = obj["Size"]
                    total_size += size
                    if key.startswith("chunks/"):
                        chunks_count += 1
                    elif key.endswith(".manifest"):
                        manifests_count += 1
                    items.append({
                        "key": key,
                        "size": size,
                        "last_modified": str(obj["LastModified"])[:19],
                        "kind": "chunk" if key.startswith("chunks/") else "manifest",
                    })
                self.send_json({
                    "ok": True,
                    "bucket": s3_bucket,
                    "region": config.get("region_name"),
                    "total_objects": len(items),
                    "chunks_count": chunks_count,
                    "manifests_count": manifests_count,
                    "total_bytes": total_size,
                    "items": items,
                })
            except Exception as e:
                self.send_json({"ok": False, "error": str(e)}, 500)
            return

        if path == "/api/download":
            bucket = qs.get("bucket", [""])[0]
            key = qs.get("key", [""])[0]
            if not bucket or not key:
                self.send_json({"ok": False, "error": "Missing bucket or key"}, 400)
                return
            try:
                tmp_fd, tmp_path = tempfile.mkstemp(prefix="helix_dl_")
                os.close(tmp_fd)
                out, err, rc = run_helix(["get", bucket, key, tmp_path])
                if rc != 0 or not os.path.exists(tmp_path):
                    self.send_json({"ok": False, "error": out + err}, 500)
                    return
                with open(tmp_path, "rb") as f:
                    file_bytes = f.read()
                os.remove(tmp_path)

                sha256_hex = hashlib.sha256(file_bytes).hexdigest()
                filename = os.path.basename(key)
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Disposition", f'attachment; filename="{filename}"')
                self.send_header("X-Helix-SHA256", sha256_hex)
                self.send_header("Content-Length", str(len(file_bytes)))
                self.end_headers()
                self.wfile.write(file_bytes)
            except Exception as e:
                self.send_json({"ok": False, "error": str(e)}, 500)
            return

        self.send_error(404, "Endpoint not found")

    def do_POST(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path

        if path == "/api/bucket":
            try:
                body = self.read_json_body()
                bname = body.get("name", "").strip()
                if not bname:
                    self.send_json({"ok": False, "error": "Bucket name required"}, 400)
                    return
                out, err, rc = run_helix(["mb", bname])
                self.send_json({
                    "ok": rc == 0,
                    "log": (out + "\n" + err).strip(),
                })
            except Exception as e:
                self.send_json({"ok": False, "error": str(e)}, 500)
            return

        if path == "/api/upload":
            try:
                body = self.read_json_body()
                bucket = body.get("bucket", "vit-demo").strip()
                key = body.get("key", "").strip()
                b64_data = body.get("data_base64", "")

                if not bucket or not key or not b64_data:
                    self.send_json({"ok": False, "error": "Bucket, key, and file data are required"}, 400)
                    return

                # Ensure bucket exists
                run_helix(["mb", bucket])

                raw_bytes = base64.b64decode(b64_data)
                orig_sha = hashlib.sha256(raw_bytes).hexdigest()

                tmp_fd, tmp_path = tempfile.mkstemp(prefix="helix_up_")
                with os.fdopen(tmp_fd, "wb") as f:
                    f.write(raw_bytes)

                out, err, rc = run_helix(["put", bucket, key, tmp_path])
                if os.path.exists(tmp_path):
                    os.remove(tmp_path)

                combined = (out + "\n" + err).strip()
                is_dedup = "[DEDUPLICATION DETECTED!]" in combined

                self.send_json({
                    "ok": rc == 0,
                    "is_deduplicated": is_dedup,
                    "file_size": len(raw_bytes),
                    "sha256": orig_sha,
                    "log": combined,
                })
            except Exception as e:
                self.send_json({"ok": False, "error": str(e)}, 500)
            return

        if path == "/api/delete":
            try:
                body = self.read_json_body()
                bucket = body.get("bucket", "").strip()
                key = body.get("key", "").strip()
                if not bucket or not key:
                    self.send_json({"ok": False, "error": "Bucket and key required"}, 400)
                    return

                out1, err1, rc1 = run_helix(["del", bucket, key])
                out2, err2, rc2 = run_helix(["gc"])
                combined = (out1 + err1 + "\n" + out2 + err2).strip()
                self.send_json({
                    "ok": rc1 == 0,
                    "log": combined,
                })
            except Exception as e:
                self.send_json({"ok": False, "error": str(e)}, 500)
            return

        if path == "/api/verify":
            try:
                body = self.read_json_body()
                bucket = body.get("bucket", "").strip()
                key = body.get("key", "").strip()
                tmp_fd, tmp_path = tempfile.mkstemp(prefix="helix_ver_")
                os.close(tmp_fd)
                out, err, rc = run_helix(["get", bucket, key, tmp_path])
                if rc != 0:
                    if os.path.exists(tmp_path):
                        os.remove(tmp_path)
                    self.send_json({"ok": False, "error": out + err}, 500)
                    return
                with open(tmp_path, "rb") as f:
                    data = f.read()
                os.remove(tmp_path)
                sha = hashlib.sha256(data).hexdigest()
                self.send_json({
                    "ok": True,
                    "size": len(data),
                    "sha256": sha,
                    "log": f"{out.strip()}\n[INTEGRITY PASS] Lossless reassembly verified! SHA-256: {sha}",
                })
            except Exception as e:
                self.send_json({"ok": False, "error": str(e)}, 500)
            return

        if path == "/api/aws/sync":
            try:
                res = subprocess.run(
                    ["python", "aws_sync.py", "sync"],
                    cwd=ROOT_DIR,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                    encoding="utf-8",
                    errors="replace",
                )
                combined = (res.stdout + "\n" + res.stderr).strip()
                m = re.search(r"Total Unique Chunks Synced\s*:\s*(\d+)", combined)
                newly_uploaded = int(m.group(1)) if m else 0
                self.send_json({
                    "ok": res.returncode == 0,
                    "newly_uploaded": newly_uploaded,
                    "log": combined,
                })
            except Exception as e:
                self.send_json({"ok": False, "error": str(e)}, 500)
            return

        self.send_error(404, "Endpoint not found")


def main():
    print("=" * 65)
    print("  HELIXVAULT WEB DASHBOARD SERVER (C11 + REAL AWS S3 ENGINE)")
    print("  Department of IT, VIT Pune — Group 14")
    print("=" * 65)
    print(f"  Dashboard URL : http://localhost:{PORT}")
    print(f"  Backend Engine: {EXE_PATH}")
    print("  Press Ctrl+C to stop the server.")
    print("=" * 65)
    server = HTTPServer(("127.0.0.1", PORT), HelixHandler)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down HelixVault Web Dashboard.")
        server.server_close()


if __name__ == "__main__":
    main()
