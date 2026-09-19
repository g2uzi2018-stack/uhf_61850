# Repository instructions

For the current `v3.0` work, read `docs/v3.0-implementation-brief.md` first.
The two source documents under `docs/spec/v3.0/` are authoritative; the brief
records implementation status and explicitly provisional policies, not approval
of unresolved product decisions. `docs/luna-implementation-brief.md` and the old
baseline/architecture/plans describe v1/v2 and must not override the new spec.

- Work and push only on `v3.0` for this iteration. Do not update `main` or `v2.0` unless the user explicitly requests it. Never force-push or rewrite published history.
- Implement one small, complete behavior at a time. Keep the tree buildable and tests green.
- Add or update tests in the same commit as the behavior.
- Preserve original DOCX/XLSX sources; record uncertainties and proposed defaults in the v3 brief instead of silently changing requirements.
- Do not re-open explicitly confirmed decisions without evidence, but do not treat historical PD1000/dBm decisions as confirmed for the new cable-PD device.
- Prefer straightforward C++17 and small interfaces. Do not add containers, databases, Node.js on target, code generation frameworks, or extra services without a demonstrated need.
- Develop protocol behavior against host simulators first. Do not stop `/data` services or touch frpc/4G before the packaging/cutover increment.
- Never commit credentials, private keys, real device logs, target configuration backups, or generated initial passwords.
- A compile alone is not completion: run the narrow tests for the increment and report the command and result. The standalone v3 core uses `cmake -S src/v3 -B build/v3`; old root tests do not cover it.
- If blocked by missing hardware, leave the latest complete commit green and continue only with work that the simulator can prove.
- New code in `src/v3` is not yet wired into `uhf-gatewayd`. Do not claim that protocol unit tests establish production integration, activation enforcement, IEC interoperability, or hardware readiness.
