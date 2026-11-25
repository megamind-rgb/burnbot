"""Burnbot's level, from Claude Code's session logs: the Python twin of app/Tracker.swift + app/Metrics.swift.

Reads ~/.claude/projects/**/*.jsonl into per-minute buckets, then scores each minute against your own
last 30 days (burn, typing, actions) to get the level 0-5 (or autopilot 6), intensity, flame and rate.
Skipped from the Mac version: the daily goal, the "usual" level, reactions and the robot's sayings,
and "hands" from answering Claude's questions (only interrupts count).
"""
import bisect, glob, json, os, re, time
from datetime import datetime

ROOT = os.path.join(os.path.expanduser("~"), ".claude", "projects")
DAYS = 30
NAMES = ["ASLEEP", "CHILLING", "WARMING UP", "COOKING", "LOCKED IN", "ON FIRE", "AUTOPILOT"]
CUTS = (30, 75, 93)
AUTOPILOT_AFTER = 25        # shortcut: fixed; the Mac learns it from your prompt gaps (15-60)
TAG = re.compile(r"<([a-z][a-z0-9_-]*)[^>]*>.*?</\1>", re.S)

TOK, CALLS, PROMPTS, CHARS, TOOLS, HANDS = range(6)


class Logs:
    def __init__(self):
        self.files = {}         # path -> [offset, size]
        self.min = {}           # epoch minute -> bucket
        self.seen = {}          # response id -> [minute, sum, output]
        self.sessions = {}      # main session -> last minute
        self.walked = 0

    def bucket(self, m):
        b = self.min.get(m)
        if b is None:
            b = self.min[m] = [0] * 6
        return b

    def poll(self):
        """Reads what the logs gained. True when anything did."""
        now = time.time()
        if now - self.walked > 20:
            self.walked = now
            for p in glob.glob(os.path.join(ROOT, "**", "*.jsonl"), recursive=True):
                try:
                    if now - os.path.getmtime(p) < DAYS * 86400:
                        self.files.setdefault(p, [0, 0])
                except OSError:
                    pass
        changed = False
        for p, st in list(self.files.items()):
            try:
                size = os.path.getsize(p)
            except OSError:
                del self.files[p]
                continue
            if size < st[0]:
                st[0] = 0
            if size == st[0]:
                continue
            with open(p, "rb") as f:
                f.seek(st[0])
                data = f.read()
            end = data.rfind(b"\n") + 1     # a half-written last line waits
            st[0] += end
            side = "subagents" in p.replace("\\", "/")
            for raw in data[:end].splitlines():
                if len(raw) > 20 and self.line(raw, side, os.path.splitext(os.path.basename(p))[0]):
                    changed = True
        return changed

    def line(self, raw, side, stem):
        try:
            d = json.loads(raw)
            ts = d["timestamp"].replace("Z", "+00:00")
            m = int(datetime.fromisoformat(ts).timestamp() // 60)
        except Exception:
            return False
        t = d.get("type")
        if not side and t in ("assistant", "user"):
            sid = d.get("sessionId") or stem
            self.sessions[sid] = max(self.sessions.get(sid, 0), m)
        msg = d.get("message") or {}
        if t == "assistant":
            u = msg.get("usage")
            if not u or msg.get("model") == "<synthetic>":
                return False
            total = sum(int(u.get(k) or 0) for k in ("input_tokens", "output_tokens",
                                                       "cache_creation_input_tokens", "cache_read_input_tokens"))
            out = int(u.get("output_tokens") or 0)
            rid = msg.get("id") or d.get("requestId") or d.get("uuid")
            old = self.seen.get(rid)
            if old and out <= old[2] and total <= old[1]:
                return False
            b = self.bucket(old[0] if old else m)
            b[TOK] += total - (old[1] if old else 0)
            if not old:
                b[CALLS] += 1
            b[TOOLS] += sum(1 for c in msg.get("content") or [] if isinstance(c, dict) and c.get("type") == "tool_use")
            self.seen[rid] = [old[0] if old else m, total, out]
            return True
        if t != "user" or side or d.get("isSidechain") or d.get("isMeta") or d.get("isCompactSummary") \
                or d.get("toolUseResult") is not None:
            return False
        c = msg.get("content")
        if isinstance(c, list):
            if any(isinstance(x, dict) and x.get("type") == "tool_result" for x in c):
                return False
            c = " ".join(x.get("text", "") for x in c if isinstance(x, dict) and x.get("type") == "text")
        if not isinstance(c, str):
            return False
        if c.startswith("[Request interrupted by user"):
            self.bucket(m)[HANDS] += 1
            return True
        typed = len(TAG.sub("", c).strip())
        if typed:
            b = self.bucket(m)
            b[PROMPTS] += 1
            b[CHARS] += typed
        return bool(typed)


def soft(v, ref):
    return 100 * (1 - 2 ** (-v / ref))


def rank(s, v):
    return 100 * bisect.bisect_left(s, v) / len(s)


def analyze(logs, now=None):
    """Everything the pad's B and D lines need, as a dict."""
    now = now or time.time()
    lt = time.localtime(now)
    midnight = int(time.mktime((lt.tm_year, lt.tm_mon, lt.tm_mday, 0, 0, 0, 0, 0, -1)) // 60)
    start, now_m = midnight - (DAYS - 1) * 1440, int(now // 60)
    n = now_m - start + 1
    zero = [0] * 6
    rows = [logs.min.get(start + k, zero) for k in range(n)]

    def prefix(f):
        out, s = [0], 0
        for v in f:
            s += v
            out.append(s)
        return out

    tok = prefix(r[TOK] for r in rows)
    chars = prefix(r[CHARS] for r in rows)
    acts = prefix(r[PROMPTS] + r[TOOLS] + r[HANDS] for r in rows)
    active_l = [1 if (r[CALLS] or r[PROMPTS] or r[HANDS]) else 0 for r in rows]
    active = prefix(active_l)
    human = [bool(r[PROMPTS] or r[HANDS]) for r in rows]

    def win(p, i, w):
        return p[i + 1] - p[max(0, i - w + 1)]

    last_h, last_human = -10 ** 9, []
    for i in range(n):
        if human[i]:
            last_h = i
        last_human.append(last_h)

    burn_s, type_s, act_s, qual = [], [], [], []
    for i in range(n):
        if win(active, i, 15) >= 3 and i - last_human[i] <= AUTOPILOT_AFTER:
            qual.append(i)
            burn_s.append(win(tok, i, 15))
            type_s.append(win(chars, i, 30))
            act_s.append(win(acts, i, 15))
    calibrated = len(qual) >= 120
    for s in (burn_s, type_s, act_s):
        s.sort()

    def p_burn(v):
        return rank(burn_s, v) if calibrated else soft(v, 3e6)

    def raw(i):
        if calibrated:
            return 0.4 * rank(burn_s, win(tok, i, 15)) + 0.35 * rank(type_s, win(chars, i, 30)) + 0.25 * rank(act_s, win(acts, i, 15))
        return 0.4 * soft(win(tok, i, 15), 3e6) + 0.35 * soft(win(chars, i, 30), 400) + 0.25 * soft(win(acts, i, 15), 20)

    raws = sorted(raw(i) for i in qual) if calibrated else []

    state, last_a, streak_start, engaged, score = 0, -10 ** 9, 0, 0, 0
    for i in range(n):
        if active_l[i]:
            if i - last_a >= 10:
                streak_start = i
            last_a = i
        idle = i - last_a
        if idle >= 30:
            state = 0
        elif idle >= 5:
            state = 1
        else:
            score = (rank(raws, raw(i)) if calibrated else raw(i)) if win(active, i, 5) else 0
            streak, prev = i - streak_start, state
            if i - last_human[i] > AUTOPILOT_AFTER:
                state = 6
            elif (score >= 93 and streak >= 20) or (prev == 5 and score >= 88):
                state = 5
            elif (score >= 75 and streak >= 20) or (prev in (4, 5) and score >= 62):
                state = 4
            else:
                state = 3 if score >= 30 else 2
        if start + i >= midnight and 2 <= state <= 5:
            engaged += 1

    i, part = n - 1, (now % 60) / 60
    rate = lambda p, w: win(p, i, w) * 60 / ((w - 1) + part)
    idle = i - last_a
    flame = 0.12 + 0.88 * (p_burn(rate(tok, 5) / 60 * 15) / 100) ** 1.3 if win(tok, i, 3) > 0 else 0.04 if idle < 30 else 0

    today0 = midnight - start
    hours = [0] * 24
    for k in range(today0, n):
        if rows[k][TOK]:
            hours[time.localtime((start + k) * 60).tm_hour] += rows[k][TOK]
    peak = max((win(tok, k, 15) * 4 for k in range(today0, n)), default=0)
    usual = burn_s[len(burn_s) // 2] * 4 if burn_s else 0
    return {
        "state": state, "intensity": round(score) if idle < 5 else 0, "flame": flame,
        "rate": round(rate(tok, 10)), "streak": (i - streak_start) if (active_l[i] or idle < 10) else 0,
        "hours": hours, "engaged": engaged, "peak": peak, "usual": usual,
        "sessions": sum(1 for m in logs.sessions.values() if m >= midnight),
    }


if __name__ == "__main__":                  # self-check: python metrics.py prints today's numbers
    assert rank([1, 2, 3, 4], 3) == 50 and round(soft(400, 400)) == 50
    logs = Logs()
    t = time.time()
    logs.poll()
    a = analyze(logs)
    print(f"{len(logs.files)} logs, {len(logs.min)} minutes, read+analyzed in {time.time() - t:.1f}s")
    print({k: v for k, v in a.items() if k != "hours"}, NAMES[a["state"]])
