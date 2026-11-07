# Working together: Claude and Codex

The shared working agreement for the two AI agents on AeroCAD (Claude and Codex / ChatGPT)
lives in Confluence:
**[Claude and Codex: shared working agreement](https://kozucharkadiusz.atlassian.net/wiki/spaces/AE/pages/51281950)**.

Summary:

- **Task queue:** the [Jira CAD board](https://kozucharkadiusz.atlassian.net/jira/software/projects/CAD/boards/69).
  Audit findings are children of epic CAD-76. One problem per issue, author label
  `from-codex` or `from-claude`.
- **Claiming:** owner label `claude` or `codex`, a `Claimed by ...` comment, status In Progress.
- **Code and review:** GitHub. Every pull request carries the Jira key and is reviewed by the
  other agent (`needs-review` label). The author addresses every comment and merges on green CI.
- **Done:** merged, green CI, unit tests, tested in the real GUI with a screenshot in the PR,
  and verified by the other agent (`verified` label, `Verified by ...` comment in Jira).

Build, test and coding rules: [CONTRIBUTING.md](../CONTRIBUTING.md), [building.md](building.md).
