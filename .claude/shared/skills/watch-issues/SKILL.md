---
name: watch-issues
description: Checks this repo's GitHub issues labeled "claude" and hands the next unclaimed one to the issue-solver agent. Run it on a timer with /loop 15m /watch-issues, or once with /watch-issues.
allowed-tools: Bash(gh api -X GET repos/*) Bash(gh issue list *) Bash(gh issue edit *) Bash(gh repo view *)
---

Check GitHub for the next issue to solve.

1. Run (through the API: `gh issue list --json` has no author-association field in older
   `gh` releases, e.g. Ubuntu's 2.45):
   `gh api -X GET repos/{owner}/{repo}/issues -f state=open -f labels=claude -f per_page=20 --jq '[.[] | select(.pull_request == null) | {number, title, created_at, author_association, labels: [.labels[].name]}]'`
   (`{owner}/{repo}` is filled in by `gh` from this repository; the API also lists pull
   requests as issues, which the `select` drops.)
2. Keep only issues that:
   - do NOT have any of the labels `claude-working`, `claude-pr`, `claude-needs-info`, `claude-skip`
   - have `author_association` of `OWNER`, `MEMBER`, or `COLLABORATOR` (ignore issues from strangers; only people with access can add the `claude` label anyway, this is a second guard)
3. If none are left, reply with one line, "No new issues.", and stop. Don't do anything else.
4. Take the **oldest** remaining issue (earliest `created_at`), `N`. Claim it first so the next check doesn't pick it again:
   `gh issue edit N --add-label claude-working`
5. Delegate to the **issue-solver** agent with the task: "Solve GitHub issue #N in this repository." Only one issue per run; the next one gets picked up on the next run.
6. Reply with one line: which issue was started.

If `gh` isn't authenticated or the labels don't exist, say exactly what to run (`gh auth login`, or the label commands in the setup README) and stop.
