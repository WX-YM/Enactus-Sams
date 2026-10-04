#!/usr/bin/env bash
# Prints a fresh set of the backend's secrets as KEY=value lines, ready to be
# appended to the environment file the service reads (see README).
#
# Each is 32 CSPRNG bytes as unpadded base64url, the only form the server
# accepts. Run once per deployment and keep the output out of git. Rotating
# TOKEN_SIGNING_KEY or SESSION_PEPPER signs everybody out; rotating
# PREHASH_PEPPER or PREHASH_SALT_KEY without anvil's pepper rotation makes
# every stored password unusable, so never regenerate those two in place.
set -euo pipefail

key() { head -c 32 /dev/urandom | base64 | tr '+/' '-_' | tr -d '=\n'; }

for name in TOKEN_SIGNING_KEY SESSION_PEPPER PREHASH_PEPPER PREHASH_SALT_KEY CODE_PEPPER \
            ADDRESS_INDEX_KEY ANALYTICS_VISITOR_PEPPER PII_SEAL_KEY PII_INDEX_KEY; do
    printf '%s=%s\n' "$name" "$(key)"
done
printf 'PREHASH_PEPPER_ID=p1\n'
