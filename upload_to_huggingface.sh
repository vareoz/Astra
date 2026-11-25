#!/usr/bin/env bash
set -euo pipefail

# Secure upload script for Astra Browser Core -> Hugging Face Hub
# Uses the official Hugging Face Hub HTTP Commit API via Python stdlib
# (preserves git history, .gitattributes, and LFS pointers without needing git-lfs).
# Reads credentials strictly from environment variables (never hardcoded).
#
# Usage:
#   export HF_TOKEN="your_write_token"
#   export HF_REPO_ID="thevareoz/astra-browser-core"
#   export HF_REPO_TYPE="model"   # model | dataset | space
#   bash astra/upload_to_huggingface.sh "Release Astra Browser Core v7"

TOKEN="${HF_TOKEN:-${HF_KEY:-${HFKEY:-${hfkey:-${HUGGINGFACE_TOKEN:-${HUGGING_FACE_HUB_TOKEN:-}}}}}}"
if [[ -z "$TOKEN" ]]; then
  echo "Error: HF_TOKEN (or HF_KEY / hfkey) environment variable is not set." >&2
  exit 1
fi

HF_REPO_ID="${HF_REPO_ID:-thevareoz/astra-browser-core}"
REPO_TYPE="${HF_REPO_TYPE:-model}"
COMMIT_MSG="${1:-Update Astra Browser Core v7 (Session 7: WebSocket RFC 6455, CSS vars, Flexbox, ES6 classes)}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

export _ASTRA_HF_TOKEN="$TOKEN"
export _ASTRA_HF_REPO_ID="$HF_REPO_ID"
export _ASTRA_HF_REPO_TYPE="$REPO_TYPE"
export _ASTRA_HF_COMMIT_MSG="$COMMIT_MSG"
export _ASTRA_SRC_DIR="$SCRIPT_DIR"

python3 - <<'PYEOF'
import base64
import hashlib
import json
import os
import sys
import urllib.error
import urllib.request

token = os.environ["_ASTRA_HF_TOKEN"].strip()
repo_id = os.environ["_ASTRA_HF_REPO_ID"].strip()
repo_type = os.environ["_ASTRA_HF_REPO_TYPE"].strip()
commit_msg = os.environ["_ASTRA_HF_COMMIT_MSG"].strip()
src_dir = os.environ["_ASTRA_SRC_DIR"]

type_plural = {"model": "models", "dataset": "datasets", "space": "spaces"}.get(repo_type)
if not type_plural:
    sys.stderr.write(f"Error: invalid HF_REPO_TYPE '{repo_type}'\n")
    sys.exit(1)

headers = {
    "Authorization": f"Bearer {token}",
    "User-Agent": "AstraBrowserCore/7.0",
}

# 1. Fetch existing remote tree recursively to skip unchanged files & avoid re-uploading LFS binaries
remote_files = {}

def fetch_tree(subpath=""):
    url = f"https://huggingface.co/api/{type_plural}/{repo_id}/tree/main"
    if subpath:
        url += "/" + subpath
    req = urllib.request.Request(url, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=15) as resp:
            items = json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        if e.code == 404:
            return
        raise
    for item in items:
        if item.get("type") == "directory":
            fetch_tree(item["path"])
        elif item.get("type") == "file":
            remote_files[item["path"]] = {
                "oid": item.get("oid", ""),
                "size": item.get("size", -1),
                "lfs": item.get("lfs"),
            }

fetch_tree()

def git_blob_sha1(data: bytes) -> str:
    h = hashlib.sha1()
    h.update(f"blob {len(data)}\0".encode("utf-8"))
    h.update(data)
    return h.hexdigest()

def sha256_hex(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()

EXCLUDE_DIRS = {"build", ".git", "__pycache__", "node_modules"}
EXCLUDE_EXTS = {".o", ".pem", ".key"}
LFS_BIN_EXTS = {".mp4", ".wav", ".png", ".jpg", ".jpeg", ".pdf", ".bin"}

ops = [{"key": "header", "value": {"summary": commit_msg}}]
changed_paths = []
skipped_count = 0

for root, dirs, files in os.walk(src_dir):
    dirs[:] = sorted(d for d in dirs if d not in EXCLUDE_DIRS)
    for fname in sorted(files):
        lower = fname.lower()
        if lower == ".env" or lower.startswith(".env.") or "secret" in lower:
            continue
        _, ext = os.path.splitext(lower)
        if ext in EXCLUDE_EXTS:
            continue

        full_path = os.path.join(root, fname)
        rel_path = os.path.relpath(full_path, src_dir).replace(os.sep, "/")

        with open(full_path, "rb") as f:
            data = f.read()

        rem = remote_files.get(rel_path)
        if rem:
            if rem.get("lfs") and rem["lfs"].get("oid") == sha256_hex(data):
                skipped_count += 1
                continue
            if not rem.get("lfs") and rem.get("oid") == git_blob_sha1(data):
                skipped_count += 1
                continue
            # If binary is already tracked by LFS on remote with same size, skip re-uploading raw blob
            if ext in LFS_BIN_EXTS and rem.get("size") == len(data):
                skipped_count += 1
                continue

        # Skip large binaries > 5 MB from direct non-LFS commit API
        if len(data) > 5 * 1024 * 1024:
            sys.stderr.write(f"[AstraHF] Skipping file > 5MB (use git-lfs): {rel_path}\n")
            continue

        ops.append({
            "key": "file",
            "value": {
                "path": rel_path,
                "content": base64.b64encode(data).decode("ascii"),
                "encoding": "base64",
            },
        })
        changed_paths.append(rel_path)

if not changed_paths:
    print(json.dumps({
        "ok": True,
        "repoId": repo_id,
        "commitUrl": f"https://huggingface.co/{repo_id}",
        "filesUploaded": 0,
        "skippedUnchanged": skipped_count,
        "message": "Repository is already up to date with local astra/ files."
    }))
    sys.exit(0)

ndjson_body = "\n".join(json.dumps(op, ensure_ascii=False) for op in ops).encode("utf-8")
commit_url = f"https://huggingface.co/api/{type_plural}/{repo_id}/commit/main"
commit_req = urllib.request.Request(
    commit_url,
    data=ndjson_body,
    method="POST",
    headers={
        **headers,
        "Content-Type": "application/x-ndjson",
    },
)

try:
    with urllib.request.urlopen(commit_req, timeout=60) as resp:
        result = json.loads(resp.read().decode("utf-8"))
        print(json.dumps({
            "ok": True,
            "repoId": repo_id,
            "commitUrl": result.get("commitUrl", f"https://huggingface.co/{repo_id}"),
            "commitOid": result.get("commitOid", ""),
            "filesUploaded": len(changed_paths),
            "uploadedPaths": changed_paths,
            "skippedUnchanged": skipped_count,
        }))
except urllib.error.HTTPError as e:
    err_body = e.read().decode("utf-8", errors="replace")
    sys.stderr.write(f"HTTP {e.code}: {err_body}\n")
    sys.exit(1)
PYEOF
