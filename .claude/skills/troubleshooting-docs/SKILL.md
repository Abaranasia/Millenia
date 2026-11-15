---
name: troubleshooting-docs
description: "Trigger: a non-trivial JUCE/DSP/build bug just got solved after real investigation. Capture it as a categorized troubleshooting doc under troubleshooting/ so the next session doesn't re-debug it."
license: Apache-2.0
metadata:
  author: "Abaranasia"
  version: "1.0"
---

## Activation Contract
Apply right after a bug is confirmed fixed ("that worked", "it's fixed", "working now") **and** it took more than one obvious attempt, or the root cause is non-obvious enough that future-you would burn time rediscovering it. Skip single-attempt typos/syntax fixes.

## Hard Rules
- One problem = one file, in `troubleshooting/<category>/<sanitized-symptom>-<YYYYMMDD>.md`. Category comes from `problem_type` via the mapping in `schema.yaml`.
- YAML frontmatter is required and must validate against `schema.yaml` (enum values must match exactly — don't invent new ones without updating the schema first).
- `root_cause` documents the *actual* mechanism, not the symptom (e.g. `wrong_api`, not "build failed").
- Before writing, `grep -r` the target category (then all of `troubleshooting/` if nothing matches) for the exact symptom string — cross-reference instead of duplicating if something similar exists.
- Include a minimal before/after code snippet in the Solution section — prose-only fixes aren't searchable later.

## Decision Gates
| Situation | Do this |
|---|---|
| Similar doc found in grep | Add a bidirectional `## Related Issues` link in both files instead of writing a near-duplicate |
| 3+ docs share the same root cause | Add a short entry to `troubleshooting/patterns/common-solutions.md` (create it if missing) |
| Bug is foundational and would bite any future DSP/plugin work (not project-specific) | Ask the user whether it belongs in a project-wide "critical patterns" doc instead of a one-off troubleshooting entry |

## Execution Steps
1. Classify the problem against `schema.yaml`'s enums (`problem_type`, `component`, `root_cause`, `resolution_type`, `severity`). If nothing fits cleanly, ask before inventing a value.
2. Search existing docs for the same symptom (see Hard Rules).
3. Write the file: frontmatter + Problem / Symptoms / What Didn't Work / Solution (with code) / Why This Works / Prevention / Related Issues.
4. If cross-references apply, update the other doc too.

## Output Contract
Before reporting the doc as done, confirm: frontmatter validates against `schema.yaml`, the file lives under the correct category directory, and the solution section has a concrete code diff — not just a description.

## References
- `schema.yaml` — required frontmatter fields, enums, and category mapping
- `../../../troubleshooting/api-usage/juce-dsp-reverb-api-mismatch.md` — worked example (imported reference case, not a Millenia incident)
