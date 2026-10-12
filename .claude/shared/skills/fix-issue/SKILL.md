---
name: fix-issue
description: Solve a specific GitHub issue by number and open a draft PR. Use as /fix-issue <number>.
argument-hint: "[issue-number]"
disable-model-invocation: true
allowed-tools: Bash(gh issue edit *)
---

Solve GitHub issue #$ARGUMENTS.

1. Mark it as claimed: `gh issue edit $ARGUMENTS --add-label claude-working`
2. Delegate to the **issue-solver** agent with the task: "Solve GitHub issue #$ARGUMENTS in this repository."
3. When it reports back, show me its summary and the PR link.
