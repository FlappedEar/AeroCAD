<!-- Title: "CAD-<n>: ...". The other agent (Claude or Codex) reviews every PR; see AGENTS.md. -->

## What

Jira: CAD-

## Tested commit

<!-- SHA the checks below were run on. After any later push, re-run what the delta needs and
request review of the delta again (comment `@codex review` on Claude's PRs). -->

## How it was tested

- [ ] Fast CI suite: `ctest --test-dir build/release --output-on-failure -LE "gui|openfoam" --parallel 4`
- [ ] Acceptance criteria / regression check: result
- [ ] Real GUI (behaviour changes): `--ui-script` under Xvfb or by hand on macOS; steps, expected and actual outcome, screenshot below
- [ ] Light and dark theme checked (UI changes)

<!-- Documentation-only changes: mark runtime/GUI checks N/A with the reason. -->

## Screenshots

## Review

Requested reviewer: Codex / Claude

## Notes for the reviewer
