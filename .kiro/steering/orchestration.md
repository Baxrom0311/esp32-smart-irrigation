# Orchestration steering

## Architecture

Three separate Kiro agents in a nested loop:
- **ai-planner** (Opus): Creates plans, replans, auto-discovery. Read-only.
- **ai-builder** (Opus/Sonnet): Makes code changes, runs verification. Write access.
- **ai-reviewer** (Opus/Haiku): Reviews builder output. Read-only. Separate from planner.

## Loop structure

```
Plan → [Build × N → Test → Review] × M → Replan → repeat or done
```

## Reliability features

- Checkpoint/Resume: `run_state.json` saved after each build
- Error retry: exponential backoff with jitter (max 3 attempts)
- Budget tracking: estimated cost per operation, hard limit stops loop
- Metrics: duration, files changed, review pass rate → `metrics.json`

## Planner expectations

- Create actionable implementation plans with clear milestones
- During replan: assess progress honestly, adjust cycle counts
- Return structured JSON when requested
- Do not edit files — read-only mode

## Builder expectations

- Make concrete edits every iteration
- Follow the plan and reviewer feedback (feedback first)
- Run verification after meaningful changes
- End with JSON report: state, files_changed, summary

## Reviewer expectations

- Separate agent from planner (can use cheaper/faster model)
- Focus on correctness, security, reliability, tests
- Return structured JSON with verdict, defects, builder_prompt
- Do not edit files — read-only mode

## Safety

- Do not expose secrets
- Do not run destructive commands outside the repository
- Budget limit enforced — loop stops when exhausted
- Retry only on timeout/crash, not on logic errors
