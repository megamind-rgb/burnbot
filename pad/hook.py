"""Burnbot Pad: the one hook script Claude Code runs, on macOS and Windows alike.

Claude Code starts it with what it knows as JSON on stdin. The first argument says which job:

  status   the status line: keeps the plan's usage for the pad's Usage page (status.json, rates.json).
           Prints nothing, so the status line stays empty.
  event    UserPromptSubmit, Stop, SessionEnd, Notification: notes the session (sessions/<id>.json):
           its title, project, transcript, whether Claude is working, and the session's message socket,
           so the pad can talk to it.
  ask      PermissionRequest: while the bridge keeps pad-alive fresh, hands the request to the pad and
           waits up to 45 s for Approve or Deny. Otherwise it prints nothing and Claude asks as usual.

It never fails loudly: whatever goes wrong, Claude Code carries on as if it were not there.
Only the Python standard library, so it starts fast.
"""
import json, os, secrets, sys, time

DATA = os.path.join(os.path.expanduser("~"), ".claude", "burnbot-pad")
WAIT = 45                                   # seconds the pad is given to answer a permission prompt


def alive():
    try:
        return time.time() - os.path.getmtime(os.path.join(DATA, "pad-alive")) <= 5
    except OSError:
        return False


def write(path, text):
    tmp = path + f".{os.getpid()}.tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        f.write(text)
    os.replace(tmp, path)


def status(raw):
    write(os.path.join(DATA, "status.json"), raw)
    if '"rate_limits"' in raw:
        write(os.path.join(DATA, "rates.json"), raw)


def event(raw):
    d = json.loads(raw)
    sid = d.get("session_id")
    if not sid:
        return
    name = d.get("hook_event_name", "")
    folder = os.path.join(DATA, "sessions")
    os.makedirs(folder, exist_ok=True)
    path = os.path.join(folder, sid + ".json")
    try:
        old = json.load(open(path, encoding="utf-8"))
    except Exception:
        old = {}
    if name == "SessionEnd":
        try:
            os.remove(path)
        except OSError:
            pass
        return
    state = {"UserPromptSubmit": "working", "Stop": "idle", "Notification": old.get("state", "idle")}.get(name, "idle")
    cwd = d.get("cwd", "") or old.get("cwd", "")
    project = os.path.basename(cwd.split("/.claude/worktrees/")[0].split("\\.claude\\worktrees\\")[0].rstrip("/\\"))
    old.update({
        "session_id": sid, "state": state, "updated": time.time(), "cwd": cwd, "project": project,
        "transcript": d.get("transcript_path", old.get("transcript", "")),
        "msg_socket": os.environ.get("CLAUDE_CODE_MESSAGING_SOCKET", old.get("msg_socket", "")),
        "msg_token": os.environ.get("CLAUDE_CODE_MESSAGING_TOKEN", old.get("msg_token", "")),
    })
    if name == "UserPromptSubmit" and not old.get("title"):
        old["title"] = " ".join(str(d.get("prompt", "")).split())[:60]
    write(path, json.dumps(old))


def ask(raw):
    if not alive():
        return
    if '"AskUserQuestion"' in raw or '"ExitPlanMode"' in raw:
        return
    asks, answers = os.path.join(DATA, "asks"), os.path.join(DATA, "answers")
    os.makedirs(asks, exist_ok=True)
    os.makedirs(answers, exist_ok=True)
    rid = secrets.token_hex(12)
    mine, reply = os.path.join(asks, rid + ".json"), os.path.join(answers, rid + ".json")
    try:
        write(mine, raw)
        end = time.time() + WAIT
        while time.time() < end:
            if os.path.exists(reply):
                time.sleep(0.05)
                answer = open(reply, encoding="utf-8").read()
                if answer.strip():
                    sys.stdout.write(answer)
                return
            if not alive():
                return
            time.sleep(0.2)
    finally:
        for p in (mine, reply):
            try:
                os.remove(p)
            except OSError:
                pass


def main():
    job = sys.argv[1] if len(sys.argv) > 1 else "event"
    try:
        raw = sys.stdin.buffer.read().decode("utf-8", "replace")
    except Exception:
        return
    if not os.path.isdir(DATA):             # not installed (or uninstalled): do nothing
        return
    try:
        {"status": status, "event": event, "ask": ask}.get(job, event)(raw)
    except Exception:
        pass


if __name__ == "__main__":
    main()
    sys.exit(0)
