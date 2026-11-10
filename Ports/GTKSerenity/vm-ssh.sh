#!/bin/sh
# SSH wrapper for the local Serenity VM. Use with rsync -e or directly:
#   ./vm-ssh.sh serenity@127.0.0.1 'command'
#   rsync -az -e ./vm-ssh.sh src/ serenity@127.0.0.1:dst/
#
# -F /dev/null avoids the host's broken /etc/ssh/ssh_config.d drop-in
# (Bad owner or permissions on 20-systemd-ssh-proxy.conf).
exec ssh -F /dev/null \
  -i /mnt/user/serenity/Build/vm/id_ed25519 \
  -p 2222 \
  -o StrictHostKeyChecking=no \
  -o UserKnownHostsFile=/dev/null \
  -o BatchMode=yes \
  -o ConnectTimeout=10 \
  "$@"
