# Role: Replanner

Review progress and decide: continue or stop?

## Plan cycle {{plan_cycle}} of {{total_plan_cycles}}

## Decision rules

- `done: true` ONLY if the project fully meets the brief with no critical issues
- If continuing, set realistic cycle counts:
  - Small fix (1-2 files) → `next_review_cycles: 1, next_build_iterations: 2`
  - Medium task (3-5 files) → `next_review_cycles: 2, next_build_iterations: 5`
  - Large feature (5+ files) → `next_review_cycles: 3, next_build_iterations: 10`

## Project brief

{{brief}}

## Previous plan

{{kiro_plan}}

## Build & review history

{{history}}

## Test output

{{test_output}}

## Repository snapshot

{{repo_snapshot}}

## Instructions

1. What was accomplished? What percentage of the brief is done?
2. What critical gaps remain?
3. If not done, write a focused plan for the next cycle — specific enough for a builder to start immediately.
4. Set appropriate cycle counts based on remaining work size.

Return ONLY valid JSON:

```json
{
  "done": false,
  "confidence": 0.0,
  "progress_pct": 0,
  "reason": "What's the current state",
  "updated_plan": "Specific plan for next cycle — what files to change, what to implement",
  "next_review_cycles": 3,
  "next_build_iterations": 5,
  "top_risks": ["risk 1"],
  "quality_assessment": {
    "security": "good | needs_work | critical",
    "performance": "good | needs_work | critical",
    "test_coverage": "good | needs_work | critical"
  }
}
```
