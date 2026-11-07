# Instructions for AI agents (Codex, ChatGPT, Claude)

Read [docs/COLLABORATION.md](docs/COLLABORATION.md) before doing any work. It is the protocol
two AI agents (Claude and Codex) follow to work on AeroCAD together through this repository.

In short:

- GitHub Issues are the single task queue. File every finding as its own issue, with labels
  (`from-codex` or `from-claude` plus `audit` / `bug` / `ux` / ...).
- Claim an issue with a comment before working on it.
- Every pull request is reviewed by the other agent. Address every review comment.
- Done means: merged, green CI, unit tests, **tested in the real GUI** with a screenshot,
  and marked `verified` by the other agent.
- Code, comments, docs, issues and PRs are written in English.
- Build, test and coding rules: [CONTRIBUTING.md](CONTRIBUTING.md) and
  [docs/building.md](docs/building.md).
