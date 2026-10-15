# Agent Instructions

This project uses **bd** (beads) for issue tracking. Run `bd prime` for full workflow context.

## Project: embpet

ESP32-C3 USB Telegram bridge with GPIO. ESP-IDF v5.5.2, plain C.

- **Spec:** `README.md`
- **Architecture and verification strategy:** `docs/PLAN.md`
- **Work items:** beads; the entry point is `docs/PLAN.md` §3.

### Build & Test

```bash
scripts/idf.sh idf.py build                   # run one command inside ESP-IDF (self-contained root ~/code/esp-idf_5.5.2)
scripts/hw.sh idf.py -p /dev/cu.usbmodem11101 flash    # board port; check `ls /dev/cu.usbmodem*`; holds the board lock (see 5)
scripts/test_host.sh                          # host unit tests (Unity+cJSON, build-host/, CTest); non-zero on failure
scripts/hw.sh python tools/embpet_cli.py sys info    # one CLI command over serial
```

Agents: always go through `scripts/idf.sh`. Never source `scripts/export.sh` in a tool call, because Claude Code never auto-allows sourcing, so the call blocks on a permission prompt (or is auto-denied in unmanned mode). Interactive shells can still use `. scripts/export.sh`.

Never install ESP-IDF into `~/.espressif` or `~/esp`. A new version gets its own root via `scripts/bootstrap.sh <root> -v <tag>`.

### Project workflow

1. **Pick work:** run `bd ready` and choose a task, not an epic. Prefer tasks labelled `critical-path`. Claim it with `bd update <id> --claim`. Don't start beads labelled `backlog`.
2. **Brief:** `bd show <id>` (Context / Scope / Acceptance Criteria / Refs, plus risk notes) and the README sections it cites. Stay in scope. If the spec is wrong or incomplete, ask the user instead of inventing behaviour, and record the outcome with `bd update <id> --append-notes`.
3. **Verify, then close:** check every acceptance criterion and record the evidence (command output, measurements) in the bead notes. A separate verifier or reviewer pass approves the work; never self-approve. Close with `bd close <id> --suggest-next`. File new work with `--deps discovered-from:<id>`.
4. **Definition of done:**
   - `idf.py build` is clean, and project components compile with `-Werror -Wall -Wextra`.
   - Host tests are green.
   - README and skills are updated when a command, event or behaviour changed.
   - No TODOs, stubs, `#if 0`, skipped tests or placeholder handlers remain.
5. **Hardware (label `hw`):** only **one** agent at a time may use `/dev/cu.usbmodem*`; flash, monitor and HIL all count. Run every hardware command through `scripts/hw.sh <cmd>`, which holds `/tmp/embpet-hw.lock` via `flock` (`brew install flock`) and runs the command inside ESP-IDF. Don't call `flock /tmp/…` directly; the `/tmp` path argument makes the call prompt. Close `idf.py monitor` before using `tools/embpet_cli.py` or pytest. Open the port with DTR=RTS=False; the tools already do this.
6. **Agent routing:**

   | Work | Agent |
   |---|---|
   | Most implementation | `executor` (sonnet) |
   | Beads labelled `model-opus` | `executor` with `model=opus` |
   | Test-heavy work | `test-engineer` |
   | End of an epic | `verifier` |
   | Before committing | `code-reviewer` |
   | Docs | `writer` |
   | ESP-IDF API questions | Read `$IDF_PATH` sources first, then `document-specialist` |
7. **Human items** (label `human`, `bd human list`): ask the user early so they don't block the critical path.
8. **Git:** this repository **opts in to the Team-maintainer profile** (see the Beads block below). Agents commit and push without asking:
   - Commit when a task is verified and closed: one focused commit per task. The message is `<area>: <summary>` plus a `Bead: <id>` line, and ends with the agent's Co-Authored-By trailer.
   - Before each commit:
     - Stage explicit paths, never `git add -A`.
     - Run `git diff --cached` and check that it contains no secrets (see 9).
     - Confirm the build and host tests are green.
   - Push to `origin` after each commit (`git pull --rebase` first if the push is rejected).
   - **Never** force-push, rewrite published history, or run `git reset --hard`; these still need the user. Don't commit broken builds or work in progress to `main`.
   - An explicit "don't commit" or "don't push" from the user always wins.
9. **Secrets:** `WIFI_PASS` and `TG_TOKEN` are secrets. They exist only in the git-ignored `.env.local`, alongside `WIFI_SSID`. Never print the secrets or copy them into code, skills, beads, logs or commits. The SSID is not a secret: it may appear in `wifi status`, logs and notes, but it is still configured from `.env.local`, not hard-coded. Humans load the file with `set -a; . ./.env.local; set +a`; agents use the in-process helpers (`scripts/hw.sh python tools/wifi_from_env.py`), because sourcing always prompts.

> **Architecture in one line:** Issues live in a local Dolt database
> (`.beads/dolt/`); cross-machine sync uses `bd dolt push/pull` (a
> git-compatible protocol), stored under `refs/dolt/data` on your git
> remote — separate from `refs/heads/*` where your code lives.
> `.beads/issues.jsonl` is a passive export, not the wire protocol.
>
> See [SYNC_CONCEPTS.md](https://github.com/gastownhall/beads/blob/main/docs/SYNC_CONCEPTS.md)
> for the one-screen overview and anti-patterns (don't treat JSONL as the
> source of truth; don't `bd import` during normal operation; don't
> reach for third-party Dolt hosting before trying the default).

## Quick Reference

```bash
bd ready              # Find available work
bd show <id>          # View issue details
bd update <id> --claim  # Claim work atomically
bd close <id>         # Complete work
bd dolt push          # Push beads data to remote
```

## Non-Interactive Shell Commands

**ALWAYS use non-interactive flags** with file operations to avoid hanging on confirmation prompts.

Shell commands like `cp`, `mv`, and `rm` may be aliased to include `-i` (interactive) mode on some systems, causing the agent to hang indefinitely waiting for y/n input.

**Use these forms instead:**
```bash
# Force overwrite without prompting
cp -f source dest           # NOT: cp source dest
mv -f source dest           # NOT: mv source dest
rm -f file                  # NOT: rm file

# For recursive operations
rm -rf directory            # NOT: rm -r directory
cp -rf source dest          # NOT: cp -r source dest
```

**Other commands that may prompt:**
- `scp` - use `-o BatchMode=yes` for non-interactive
- `ssh` - use `-o BatchMode=yes` to fail instead of prompting
- `apt-get` - use `-y` flag
- `brew` - use `HOMEBREW_NO_AUTO_UPDATE=1` env var

<!-- BEGIN BEADS INTEGRATION v:1 profile:minimal hash:6cd5cc61 -->
## Beads Issue Tracker

This project uses **bd (beads)** for issue tracking. Run `bd prime` to see full workflow context and commands.

### Quick Reference

```bash
bd ready              # Find available work
bd show <id>          # View issue details
bd update <id> --claim  # Claim work
bd close <id>         # Complete work
```

### Rules

- Use `bd` for ALL task tracking — do NOT use TodoWrite, TaskCreate, or markdown TODO lists
- Run `bd prime` for detailed command reference and session close protocol
- Use `bd remember` for persistent knowledge — do NOT use MEMORY.md files

**Architecture in one line:** issues live in a local Dolt DB; sync uses `refs/dolt/data` on your git remote; `.beads/issues.jsonl` is a passive export. See https://github.com/gastownhall/beads/blob/main/docs/SYNC_CONCEPTS.md for details and anti-patterns.

## Agent Context Profiles

The managed Beads block is task-tracking guidance, not permission to override repository, user, or orchestrator instructions.

- **Conservative (default)**: Use `bd` for task tracking. Do not run git commits, git pushes, or Dolt remote sync unless explicitly asked. At handoff, report changed files, validation, and suggested next commands.
- **Minimal**: Keep tool instruction files as pointers to `bd prime`; use the same conservative git policy unless active instructions say otherwise.
- **Team-maintainer**: Only when the repository explicitly opts in, agents may close beads, run quality gates, commit, and push as part of session close. A current "do not commit" or "do not push" instruction still wins.

## Session Completion

This protocol applies when ending a Beads implementation workflow. It is subordinate to explicit user, repository, and orchestrator instructions.

1. **File issues for remaining work** - Create beads for anything that needs follow-up
2. **Run quality gates** (if code changed) - Tests, linters, builds
3. **Update issue status** - Close finished work, update in-progress items
4. **Handle git/sync by active profile**:
   ```bash
   # Conservative/minimal/default: report status and proposed commands; wait for approval.
   git status

   # Team-maintainer opt-in only, unless current instructions forbid it:
   git pull --rebase
   git push
   git status
   ```
5. **Hand off** - Summarize changes, validation, issue status, and any blocked sync/commit/push step

**Critical rules:**
- Explicit user or orchestrator instructions override this Beads block.
- Do not commit or push without clear authority from the active profile or the current user request.
- If a required sync or push is blocked, stop and report the exact command and error.
<!-- END BEADS INTEGRATION -->

<!-- BEGIN BEADS CODEX SETUP: generated by bd setup codex -->
## Beads Issue Tracker

Use Beads (`bd`) for durable task tracking in repositories that include it. Use the `beads` skill at `.agents/skills/beads/SKILL.md` (project install) or `~/.agents/skills/beads/SKILL.md` (global install) for Beads workflow guidance, then use the `bd` CLI for issue operations.

### Quick Reference

```bash
bd ready                # Find available work
bd show <id>            # View issue details
bd update <id> --claim  # Claim work
bd close <id>           # Complete work
bd prime                # Refresh Beads context
```

### Rules

- Use `bd` for all task tracking; do not create markdown TODO lists.
- Run `bd prime` when Beads context is missing or stale. Codex 0.129.0+ can load Beads context automatically through native hooks; use `/hooks` to inspect or toggle them.
- Keep persistent project memory in Beads via `bd remember`; do not create ad hoc memory files.

**Architecture in one line:** issues live in a local Dolt DB; sync uses `refs/dolt/data` on your git remote; `.beads/issues.jsonl` is a passive export. See https://github.com/gastownhall/beads/blob/main/docs/SYNC_CONCEPTS.md for details and anti-patterns.
<!-- END BEADS CODEX SETUP -->
