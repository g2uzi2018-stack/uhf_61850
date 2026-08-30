# Repository instructions

Read `docs/luna-implementation-brief.md` first, then use the other design documents only for the detail needed by the current increment.

- Implement one small, complete behavior at a time. Keep the tree buildable and tests green.
- Add or update tests in the same commit as the behavior.
- Commit and push each completed increment to `main`; never force-push or rewrite published history.
- Do not re-open decisions recorded as confirmed in the brief unless evidence shows they are impossible or unsafe.
- Prefer straightforward C++17 and small interfaces. Do not add containers, databases, Node.js on target, code generation frameworks, or extra services without a demonstrated need.
- Develop protocol behavior against host simulators first. Do not stop `/data` services or touch frpc/4G before the packaging/cutover increment.
- Never commit credentials, private keys, real device logs, target configuration backups, or generated initial passwords.
- A compile alone is not completion: run the narrow tests for the increment and report the command and result.
- If blocked by missing hardware, leave the latest complete commit green and continue only with work that the simulator can prove.
