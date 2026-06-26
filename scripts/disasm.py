#!/usr/bin/env python3
"""Extract native code from crash_diag output and disassemble with objdump."""
import subprocess, sys, re

# Read crash_diag output
result = subprocess.run(['./scripts/crash_diag'], capture_output=True, text=True, timeout=30)
output = result.stdout + result.stderr

# Find native code section
match = re.search(r'=== Native code \((\d+) bytes\) ===\n((?:[\da-fA-F]{2}.*\n?)+)', output)
if not match:
    print("Could not find native code section")
    sys.exit(1)

hex_bytes = match.group(2).replace('\n', ' ').split()
code = bytes(int(b, 16) for b in hex_bytes if b)

# Write binary
with open('/tmp/native_code.bin', 'wb') as f:
    f.write(code)

# Disassemble with objdump
result2 = subprocess.run(
    ['objdump', '-D', '-b', 'binary', '-m', 'i386:x86-64', '-M', 'intel', '/tmp/native_code.bin'],
    capture_output=True, text=True
)
print(result2.stdout)
