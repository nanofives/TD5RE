#!/usr/bin/env bash
# deploy_release.sh - publish the TD5RE release to the LAN file server on the Pi.
# Run from the dev machine (Git Bash). One-shot: manifest -> upload -> nginx -> firewall -> verify.
#
# Upload strategy (no rsync required):
#   * first publish        -> tar the whole release over SSH and extract on the Pi
#   * subsequent publishes -> diff local vs remote manifest (jq) and tar ONLY changed files
set -euo pipefail

PI_HOST="${PI_HOST:-mariano@mariano-server.local}"
SSH_KEY="${SSH_KEY:-$HOME/.ssh/id_ed25519_rpi}"
PI_DIR="${PI_DIR:-/home/mariano/DockerApps}"
REMOTE_DIR="${REMOTE_DIR:-/home/mariano/td5re-release}"
FILES_PORT="${TD5RE_FILES_PORT:-8088}"
LAN_CIDR="${LAN_CIDR:-192.168.0.0/16}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DOCKERAPPS="${DOCKERAPPS:-$HOME/Desktop/Proyectos/DockerApps}"
SSH_OPTS="-o ConnectTimeout=15 -o BatchMode=yes -o StrictHostKeyChecking=accept-new"
SSH_CMD="ssh -i $SSH_KEY $SSH_OPTS"
META_FILES=(td5re_release.exe td5re_release.ini td5re_update.ps1 update.bat pending_to_test.csv manifest.json)

# Pre-flight: confirm the Pi answers over SSH BEFORE the expensive manifest hash
# (SHA-256 over ~600 MB of re/assets). Exit 2 (distinct from a real failure) so
# callers — including /end Step 7e — can treat "Pi off" as a clean skip, not an error.
echo "[+] Pre-flight: checking $PI_HOST is reachable over SSH ..."
if ! $SSH_CMD "$PI_HOST" true 2>/dev/null; then
    echo "[!] $PI_HOST not reachable (SSH key: $SSH_KEY). Nothing was published."
    echo "    Re-run 'bash scripts/deploy_release.sh' when the Pi is online."
    exit 2
fi

echo "[+] (1/6) Regenerating manifest.json ..."
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w "$ROOT/scripts/make_release_manifest.ps1")"

echo "[+] (2/6) Ensuring remote release dir ..."
$SSH_CMD "$PI_HOST" "mkdir -p '$REMOTE_DIR/re/assets'"

echo "[+] (3/6) Uploading release files ..."
if command -v rsync >/dev/null 2>&1; then
    rsync -az --info=progress2 -e "$SSH_CMD" \
        "${META_FILES[@]/#/$ROOT/}" "$PI_HOST:$REMOTE_DIR/"
    rsync -az --delete --info=progress2 -e "$SSH_CMD" \
        "$ROOT/re/assets/" "$PI_HOST:$REMOTE_DIR/re/assets/"
else
    tmp_remote="$(mktemp)"
    trap 'rm -f "$tmp_remote"' EXIT
    $SSH_CMD "$PI_HOST" "cat '$REMOTE_DIR/manifest.json' 2>/dev/null" > "$tmp_remote" || true
    if [ -s "$tmp_remote" ] && command -v jq >/dev/null 2>&1; then
        echo "    incremental: diffing against the server manifest ..."
        # tr -d '\r': the Windows jq build emits CRLF; strip CR so paths stat cleanly.
        mapfile -t CHANGED < <(jq -r -n \
            --slurpfile a "$ROOT/manifest.json" \
            --slurpfile b "$tmp_remote" '
              ($b[0].files | map({key: .path, value: .sha256}) | from_entries) as $bmap
              | $a[0].files[] | select($bmap[.path] != .sha256) | .path' | tr -d '\r')
        if [ "${#CHANGED[@]}" -eq 0 ]; then
            echo "    nothing changed; refreshing manifest only."
            tar -czf - -C "$ROOT" manifest.json | $SSH_CMD "$PI_HOST" "tar -xzf - -C '$REMOTE_DIR'"
        else
            echo "    ${#CHANGED[@]} changed file(s); uploading just those."
            printf '%s\n' "${CHANGED[@]}" manifest.json \
                | tar -czf - -C "$ROOT" -T - \
                | $SSH_CMD "$PI_HOST" "tar -xzf - -C '$REMOTE_DIR'"
        fi
    else
        echo "    first publish: full upload via tar-over-ssh ..."
        tar -czf - -C "$ROOT" "${META_FILES[@]}" re/assets \
            | $SSH_CMD "$PI_HOST" "tar -xzf - -C '$REMOTE_DIR'"
    fi
fi

echo "[+] (4/6) Syncing docker-compose.yml and starting the nginx host ..."
scp -i "$SSH_KEY" $SSH_OPTS "$DOCKERAPPS/docker-compose.yml" "$PI_HOST:$PI_DIR/docker-compose.yml"
$SSH_CMD "$PI_HOST" "cd '$PI_DIR' && docker compose up -d td5re-files"

echo "[+] (5/6) Opening LAN firewall port $FILES_PORT ..."
if $SSH_CMD "$PI_HOST" "sudo -n ufw allow from $LAN_CIDR to any port $FILES_PORT proto tcp comment 'td5re files LAN'" 2>/dev/null; then
    echo "    ufw rule added."
else
    echo "    [!] Could not add the ufw rule non-interactively. Run this once on the Pi:"
    echo "        sudo ufw allow from $LAN_CIDR to any port $FILES_PORT proto tcp comment 'td5re files LAN'"
fi

echo "[+] (6/6) Verifying ..."
$SSH_CMD "$PI_HOST" "curl -fsS http://localhost:$FILES_PORT/manifest.json >/dev/null && echo '    server OK - manifest reachable on the Pi'"

echo ""
echo "[+] Publish complete. On each game machine, run update.bat (or td5re_update.ps1)."
echo "    Files are served at: http://mariano-server.local:$FILES_PORT/"
