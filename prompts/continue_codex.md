# Continue Building

Build iteration {{build_iter}}/{{total_build_iters}}, review cycle {{review_cycle}}.

## Critical rules

1. **You MUST make file changes.** Do not say "everything is done." Find the next task and implement it.
2. **Follow the plan.** Pick the next unfinished item.
3. **Fix reviewer issues first.** If feedback mentions problems, fix them before anything else.
4. **Run verification after changes.**

## Architecture plan

{{kiro_plan}}

## Reviewer feedback (FIX THESE FIRST)

{{previous_feedback}}

## Repository snapshot

{{repo_snapshot}}

## Project brief

{{brief}}

## What to do

1. Read the plan and feedback above.
2. Identify what is NOT yet done.
3. Implement it NOW.
4. Run tests.
5. Report what you changed.

If the plan tasks are all done, look for:
- Performance improvements
- Missing error handling
- Security hardening
- Edge case fixes
- Missing test coverage

End with JSON:

```json
{
  "state": "in_progress | complete | blocked",
  "summary": "What you changed",
  "files_changed": ["path/to/file"],
  "verification": "Commands and results",
  "next_suggested_task": "Next task"
}
```
