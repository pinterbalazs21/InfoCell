"""InfoCell viewer: a local web front end for infocell-cli.

    python tools/web/server.py            then open http://127.0.0.1:8765

Lists the ARC-AGI-1 / ARC-AGI-2 tasks and runs infocell-cli for them. Which parts the page shows
depends on the commands this infocell-cli has (`/api/capabilities`). Results are cached in
build/web-cache and recomputed when infocell-cli is rebuilt. Standard library only.
"""

import argparse
import json
import re
import subprocess
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent

def parse_args():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--arc1", default=str(REPO.parent / "ARC-AGI" / "data"), help="ARC-AGI-1 data folder (with training/ and evaluation/)")
    p.add_argument("--arc2", default=str(REPO.parent / "ARC-AGI-2" / "data"), help="ARC-AGI-2 data folder (with training/ and evaluation/)")
    exe = REPO / "build" / "Release" / "bin" / "infocell-cli.exe"
    if not exe.exists():
        exe = exe.with_suffix("")
    p.add_argument("--exe", default=str(exe), help="infocell-cli executable")
    p.add_argument("--port", type=int, default=8765)
    p.add_argument("--timeout", type=int, default=900, help="seconds allowed per run")
    return p.parse_args()

ARGS = parse_args()
CACHE = REPO / "build" / "web-cache"
SETS = {
    "arc1-training": ("ARC-AGI-1 · training", Path(ARGS.arc1) / "training"),
    "arc1-evaluation": ("ARC-AGI-1 · evaluation", Path(ARGS.arc1) / "evaluation"),
    "arc2-training": ("ARC-AGI-2 · training", Path(ARGS.arc2) / "training"),
    "arc2-evaluation": ("ARC-AGI-2 · evaluation", Path(ARGS.arc2) / "evaluation"),
}
LIMITS = {"depth": (1, 50), "beam": (1, 20000), "finishers": (0, 100)}
_locks = {}
_locks_guard = threading.Lock()
_caps = {"mtime": None, "commands": set()}

def exe_mtime():
    try:
        return Path(ARGS.exe).stat().st_mtime
    except OSError:
        return None

def commands():
    mtime = exe_mtime()
    if _caps["mtime"] != mtime:
        try:
            out = subprocess.run([ARGS.exe, "--help"], capture_output=True, text=True, timeout=60).stdout
            sub = out.split("SUBCOMMANDS:", 1)[-1] if "SUBCOMMANDS:" in out else ""
            _caps["commands"] = set(re.findall(r"^\s{2}([a-z0-9-]+)\s", sub, re.M))
        except (OSError, subprocess.TimeoutExpired):
            _caps["commands"] = set()
        _caps["mtime"] = mtime
    return _caps["commands"]

def task_file(set_key, task_id):
    if set_key not in SETS or not task_id.replace("-", "").isalnum():
        return None
    f = SETS[set_key][1] / f"{task_id}.json"
    return f if f.exists() else None

def fresh(f):
    mtime = exe_mtime()
    return f.exists() and (mtime is None or f.stat().st_mtime >= mtime)

def cached_status(set_key, task_id):
    f = CACHE / set_key / f"{task_id}.json"
    if not fresh(f):
        return None
    try:
        return json.loads(f.read_text(encoding="utf8")).get("status")
    except (OSError, ValueError):
        return None

def run(args):
    try:
        r = subprocess.run([ARGS.exe, *args], capture_output=True, text=True, timeout=ARGS.timeout)
    except subprocess.TimeoutExpired:
        return 504, {"error": f"infocell-cli took longer than {ARGS.timeout} s"}
    except OSError as e:
        return 500, {"error": f"cannot run {ARGS.exe}: {e}"}
    line = next((l for l in reversed(r.stdout.splitlines()) if l.startswith("{")), None)
    if r.returncode != 0 or not line:
        return 500, {"error": f"infocell-cli {args[0]} failed (exit code {r.returncode})", "stderr": r.stderr[-2000:]}
    return 200, json.loads(line)

def run_cached(out, args):
    with _locks_guard:
        lock = _locks.setdefault(out, threading.Lock())
    with lock:
        if fresh(out):
            return 200, json.loads(out.read_text(encoding="utf8"))
        code, body = run(args)
        if code == 200:
            out.parent.mkdir(parents=True, exist_ok=True)
            out.write_text(json.dumps(body), encoding="utf8")
        return code, body

def solve(set_key, task_id, query, partial=False):
    if "explain" not in commands():
        return 501, {"error": "this infocell-cli has no solver (explain command)"}
    f = task_file(set_key, task_id)
    if not f:
        return 404, {"error": "unknown task"}
    args, tag = ["explain", str(f)] + (["--partial"] if partial else []), ""
    for name, (low, high) in LIMITS.items():
        if name in query:
            try:
                value = int(query[name])
            except ValueError:
                return 400, {"error": f"{name} must be a whole number"}
            if not low <= value <= high:
                return 400, {"error": f"{name} must be between {low} and {high}"}
            args += [f"--{name}", str(value)]
            tag += f".{name[0]}{value}"
    return run_cached(CACHE / set_key / f"{task_id}{'.partial' if partial else ''}{tag}.json", args)

def types():
    code, body = run_cached(CACHE / "types.json", ["types"])
    if code == 200 and "vocabulary" in commands():
        vcode, vocab = run_cached(CACHE / "vocabulary.json", ["vocabulary"])
        if vcode == 200:
            body = {**body, "solver": vocab}
    return code, body

def toolfinder(query):
    if "toolfinder" not in commands():
        return 501, {"error": "this infocell-cli has no toolfinder command"}
    ints = re.compile(r"^-?\d+$")
    if "sequence" in query:
        values = [v for v in query["sequence"].split(",") if v]
        if len(values) < 2 or not all(ints.match(v) for v in values):
            return 400, {"error": "sequence: at least two whole numbers, separated by commas"}
        return run(["toolfinder", "--sequence", *values])
    pairs = [p for p in query.get("pairs", "").split(",") if p]
    if not pairs or not all(re.match(r"^-?\d+:-?\d+$", p) for p in pairs):
        return 400, {"error": "pairs: from:to examples, separated by commas"}
    args = ["toolfinder", "--pairs", *pairs]
    if query.get("test"):
        if not ints.match(query["test"]):
            return 400, {"error": "test must be a whole number"}
        args += ["--test", query["test"]]
    return run(args)


FORMAL = REPO / "data" / "formal"


def grammar_examples():
    out = []
    for f in sorted(FORMAL.glob("*.txt")) if FORMAL.exists() else []:
        lines = f.read_text(encoding="utf8").splitlines()
        out.append({"name": f.stem, "title": next((l.lstrip("# ").strip() for l in lines if l.startswith("#")), f.stem),
                    "strings": [l.strip() for l in lines if l.strip() and not l.startswith("#")]})
    return out


def grammar(query):
    if "grammar" not in commands():
        return 501, {"error": "this infocell-cli has no grammar command"}
    strings = [s.strip() for s in query.get("strings", "").split("\n") if s.strip()]
    if not strings:
        return 400, {"error": "give at least one string"}
    if len(strings) > 2000 or any(len(s) > 200 for s in strings):
        return 400, {"error": "at most 2000 strings of at most 200 symbols"}
    f = CACHE / "grammar" / f"{abs(hash(tuple(strings)))}.txt"
    f.parent.mkdir(parents=True, exist_ok=True)
    f.write_text("\n".join(strings) + "\n", encoding="utf8")
    return run(["grammar", str(f), "--json"])

class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass

    def send(self, code, body, ctype="application/json"):
        data = body if isinstance(body, bytes) else json.dumps(body).encode("utf8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        url = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(url.query).items()}
        if url.path in ("/", "/index.html"):
            return self.send(200, (HERE / "index.html").read_bytes(), "text/html; charset=utf-8")
        if url.path == "/concept":
            return self.send(200, (HERE / "concept.html").read_bytes(), "text/html; charset=utf-8")
        if url.path == "/api/capabilities":
            cmds = commands()
            return self.send(200, {"solve": "explain" in cmds, "toolfinder": "toolfinder" in cmds, "types": "types" in cmds, "grammar": "grammar" in cmds})
        if url.path == "/api/grammar-examples":
            return self.send(200, grammar_examples())
        if url.path == "/api/grammar":
            return self.send(*grammar(q))
        if url.path == "/api/sets":
            sets = []
            for key, (label, folder) in SETS.items():
                ids = sorted(p.stem for p in folder.glob("*.json")) if folder.exists() else []
                sets.append({"key": key, "label": label, "found": folder.exists(),
                             "tasks": [{"id": i, "status": cached_status(key, i)} for i in ids]})
            return self.send(200, sets)
        if url.path == "/api/task":
            f = task_file(q.get("set", ""), q.get("id", ""))
            return self.send(200, f.read_bytes()) if f else self.send(404, {"error": "unknown task"})
        if url.path == "/api/solve":
            return self.send(*solve(q.get("set", ""), q.get("id", ""), q))
        if url.path == "/api/partial":
            return self.send(*solve(q.get("set", ""), q.get("id", ""), q, partial=True))
        if url.path == "/api/types":
            return self.send(*types())
        if url.path == "/api/toolfinder":
            return self.send(*toolfinder(q))
        self.send(404, {"error": "not found"})

if __name__ == "__main__":
    for key, (label, folder) in SETS.items():
        print(f"{label:24} {folder}  {'ok' if folder.exists() else 'MISSING'}")
    print(f"infocell-cli: {ARGS.exe}  {'ok' if Path(ARGS.exe).exists() else 'MISSING'}")
    print(f"open http://127.0.0.1:{ARGS.port}")
    ThreadingHTTPServer(("127.0.0.1", ARGS.port), Handler).serve_forever()
