# Privacy and security

Do not commit runtime configuration, stream keys, private domains, client IP logs, user media, environment files or local test state. Build and test outputs are ignored. Examples use loopback or reserved documentation addresses.

Run `python scripts/privacy_check.py` before pushing. It checks tracked source for credential patterns, private IP literals, personal machine paths and unexpected URL domains. Findings report file/line/category without printing matched values. This is an additional safeguard, not a substitute for reviewing changes.

Public issue reports should use generated fixtures and redacted diagnostics. Do not attach the diagnostic state file: it intentionally contains the local publishing URL.
