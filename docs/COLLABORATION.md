# Working together: Claude and Codex

AeroCAD is built by two AI agents for one owner:

- **Claude** (Anthropic, Claude Code) — works in parallel threads, implements features and fixes.
- **Codex / ChatGPT** (OpenAI) — audits the application, implements fixes, and reviews pull requests.
- **Arek** (`@arekkozuch`) — the owner. He sets goals and judges the result in the app on his Mac.
  He does not route work between the agents and does not approve tool prompts.

The agents cannot talk to each other directly. **This repository is the only shared workspace.**
Everything one agent wants the other to know is written here: in an issue, a pull request, a
review comment, or a file in `docs/`. Nothing important lives only in a chat.

## 1. GitHub Issues are the single task queue

Every piece of work is a GitHub issue in `flappedear/aerocad`: audit findings, bugs, UX problems,
missing features. If it is not an issue, it is not in the queue.

- One problem per issue. Split audit reports into separate issues.
- Use the templates in `.github/ISSUE_TEMPLATE/` (bug, UX, audit finding, task).
- Write steps to reproduce in the real application, the expected result and the actual result.
  Attach a screenshot or a `--ui-script` that shows the problem when possible.
- Search open issues before filing to avoid duplicates. Comment on an existing issue instead.
- Jira (`CAD-*`) holds the long-term plan; GitHub Issues hold the day-to-day queue. Reference a
  Jira key in the issue when one exists.

## 2. Labels

| Label | Meaning |
|---|---|
| `audit` | Finding from a code or product audit |
| `bug` | Something is broken or wrong |
| `ux` | Works, but is hard, slow or unpleasant to use |
| `from-codex` | Filed by Codex / ChatGPT |
| `from-claude` | Filed by Claude |
| `needs-review` | Work is done and waits for the other agent's check |
| `verified` | The other agent checked the fix in the real GUI and confirms it |
| `blocked` | Cannot proceed; the comment says on what |

Every issue gets an author label (`from-codex` or `from-claude`) and at least one type label
(`audit`, `bug`, `ux`, `enhancement`, `documentation`). `scripts/setup-labels.sh` creates the labels.

## 3. Claiming work

- Before starting, **claim** the issue: comment `Claimed by Claude` or `Claimed by Codex`
  (and assign yourself if you can). Do not start an issue someone else has claimed.
- A claim with no linked pull request or progress comment for 24 hours may be taken over;
  say so in a comment.
- By default: Codex files audit findings; either agent may fix any issue. Arek's priorities
  (comments from `@arekkozuch`) come first.

## 4. Pull requests

- One issue (or a small group of related issues) per pull request. Put `Fixes #<n>` in the
  description so the issue closes on merge.
- Use the pull request template. Say what you tested and how (unit tests, `--ui-script`,
  screenshots).
- **Every pull request is reviewed by the other agent.** Claude's PRs are reviewed by Codex;
  Codex's PRs are reviewed by Claude. Add the `needs-review` label when the PR is ready.
- The author addresses every review comment: fix it, or reply with a reason why not. Then
  resolve the thread.
- The author merges their own PR when CI is green and all review threads are resolved.
  Arek does not need to click anything.
- Follow [CONTRIBUTING.md](../CONTRIBUTING.md): English, SPDX headers, command layer, tests,
  warning-free build.

## 5. Findings go into issues, not chats

If you find a problem while reviewing or working on something else — in either agent's code —
file an issue (or a review comment on the PR that introduced it). Do not leave it in a chat
or a summary for Arek: he will not relay it.

## 6. Definition of done

An issue is done only when all of these are true:

1. The change is merged into `main` with green CI (Linux + macOS).
2. Unit tests cover the new behaviour (`ctest -LE gui`).
3. **It was tested in the real GUI**: the application was started (on Linux under Xvfb is fine)
   and the workflow was driven with real mouse events via `--ui-script`
   (see [building.md](building.md#tests)), or by hand on macOS. A screenshot of the result is
   attached to the PR.
4. The other agent checked it and added the `verified` label (or wrote `Verified` in a comment
   with what was checked).
5. User-visible changes are reflected in `docs/` (user guide or feature doc).

## 7. Quality bar

AeroCAD must feel like a professional CAD tool (Autodesk Fusion is the baseline):

- Basics first: navigation, selection, undo, New/Open fully resetting state, saving, clear errors.
- Minimal steps, obvious workflow. Real 3D scans are never planar or perfect; tools must work on
  curved, imperfect surfaces and produce clean edges.
- Check light and dark themes.

## 8. Where things are

- Build and tests: [building.md](building.md)
- Architecture: [architecture.md](architecture.md)
- UI conventions: [ui.md](ui.md)
- User guide: [user-guide.md](user-guide.md)
