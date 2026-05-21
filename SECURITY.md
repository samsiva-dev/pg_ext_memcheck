# Security Policy

## Supported versions

| Version | Supported |
| ------- | --------- |
| latest (main) | ✅ |
| older tags | ❌ |

Only the latest release on `main` receives security fixes.
Backporting to older tags is not planned unless the issue is critical.

## PostgreSQL version compatibility

This extension is tested against **PostgreSQL 15, 16, and 17**.
Vulnerabilities specific to older PG versions (≤14) are out of scope.

## Reporting a vulnerability

**Please do not open a public GitHub issue for security vulnerabilities.**

Report privately via one of these channels:

- **GitHub private advisory** (preferred):  
  [Security → Report a vulnerability](../../security/advisories/new)  
  GitHub keeps it confidential until a patch is ready.

- **Email**: `samsr.devmail@gmail.com`  
  Use the subject line: `[SECURITY] pg_ext_memcheck - <short description>`

### What to include

- PostgreSQL version and OS
- Extension version or commit hash
- Description of the vulnerability and its impact
- Steps to reproduce (SQL, C code, or GDB output if applicable)
- Any suggested fix or patch (optional but appreciated)

## Response timeline

| Step | Target |
| ---- | ------ |
| Acknowledgement | Within 48 hours |
| Initial assessment | Within 7 days |
| Patch release | Within 30 days (critical), 90 days (non-critical) |

## Scope

Issues in scope:

- Memory safety bugs in the extension C code (buffer overflows, use-after-free, etc.)
- Privilege escalation via the extension hooks
- Incorrect shared memory boundary enforcement
- DSM lifecycle mismanagement leading to data corruption

Out of scope:

- Bugs in PostgreSQL core itself (report to [pgsql-bugs](https://www.postgresql.org/support/submitbug/))
- Issues in the user's PostgreSQL configuration unrelated to this extension
- Denial-of-service via intentionally malformed SQL from a superuser

## Disclosure policy

We follow **coordinated disclosure**. Once a fix is released, a public GitHub Security Advisory will be published with full details and credit to the reporter.
