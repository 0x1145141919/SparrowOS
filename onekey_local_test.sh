#!/usr/bin/env bash
set -euo pipefail

# Step 0: update repo
git pull

# Step 1: build kernel resources
make kernel_resources

# Step 2: install & test on this machine
../installandtestonthismechain.sh

# Step 3: reboot sequence (try multiple commands)
set +e
reboot_cmds=(
  "systemctl reboot"
  "reboot"
  "shutdown -r now"
)

for cmd in "${reboot_cmds[@]}"; do
  eval "$cmd" && exit 0
  sleep 1
done

echo "All reboot commands failed." >&2
exit 1
