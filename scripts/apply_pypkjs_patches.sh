#!/bin/bash
# Apply the local pypkjs fixes that make the Pebble emulator's JavaScript
# runtime able to fetch from a FreshRSS instance on the local network.
#
# The emulator's phone-simulator XHR is broken with the modern urllib3
# bundled in pebble-tool (two API drifts), and its sandbox blocks RFC1918
# hosts — so an app pointing at a LAN FreshRSS server can never load its
# feed tree. This patches the installed pebble-tool (user-level, dev-only).
#
# Run once after (re)installing pebble-tool:
#   scripts/apply_pypkjs_patches.sh
set -e

SR=/home/shured/.local/share/uv/tools/pebble-tool/lib/python3.13/site-packages/pypkjs/javascript/safe_requests.py
[ -f "$SR" ] || { echo "safe_requests.py not found at $SR — adjust the path."; exit 1; }

cp "$SR" "$SR.bak"

# 1. urllib3 2.x calls _new_pool(..., request_context=...)
python3 - "$SR" <<'EOF'
import sys
p = sys.argv[1]
src = open(p).read()
old = "    def _new_pool(self, scheme, host, port):"
new = "    def _new_pool(self, scheme, host, port, request_context=None):"
assert old in src, "signature line not found (already patched?)"
open(p, 'w').write(src.replace(old, new))
print("patched _new_pool signature")
EOF

# 2. urllib3 2.x removed the 'strict' connection kwarg
python3 - "$SR" <<'EOF'
import sys
p = sys.argv[1]
src = open(p).read()
old = """            for kw in requests.packages.urllib3.poolmanager.SSL_KEYWORDS:
                kwargs.pop(kw, None)

        return pool_cls(host, port, **kwargs)"""
new = """            for kw in requests.packages.urllib3.poolmanager.SSL_KEYWORDS:
                kwargs.pop(kw, None)
        # urllib3 2.x removed the 'strict' connection kwarg
        kwargs.pop('strict', None)

        return pool_cls(host, port, **kwargs)"""
assert old in src, "strict block not found (already patched?)"
open(p, 'w').write(src.replace(old, new))
print("patched strict kwarg")
EOF

# 3. Allow the FreshRSS test LAN (192.168.x.x) through the sandbox
python3 - "$SR" <<'EOF'
import sys
p = sys.argv[1]
src = open(p).read()
old = '    IPNetwork("192.168.0.0/16"),  # RFC 1918\n'
assert old in src, "192.168 block not found (already removed?)"
open(p, 'w').write(src.replace(old, ''))
print("allowed 192.168.0.0/16 through the sandbox")
EOF

echo "pypkjs patched. Restart any running emulator before capturing screenshots."
