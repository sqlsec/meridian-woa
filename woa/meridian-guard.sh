#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 meridian-woa contributors
# Run once inside Linux (Omarchy):  sudo bash /boot/meridian-guard.sh
# Installs a small systemd service that puts the "Meridian Obsidian" UEFI entry
# back at the front of BootOrder on every boot and every shutdown/reboot, so
# Limine/efibootmgr updates in Omarchy cannot take over the default again.
# Undo:  sudo bash /boot/meridian-guard.sh --remove
set -euo pipefail

BIN=/usr/local/bin/meridian-bootorder
UNIT=/etc/systemd/system/meridian-bootorder.service

if [[ $EUID -ne 0 ]]; then
    echo "Run with sudo: sudo bash $0" >&2
    exit 1
fi

if [[ ${1:-} == --remove ]]; then
    systemctl disable --now meridian-bootorder.service 2>/dev/null || true
    rm -f "$UNIT" "$BIN"
    systemctl daemon-reload
    echo "Removed the meridian-bootorder service (BootOrder left as it is)."
    exit 0
fi

if ! command -v efibootmgr >/dev/null 2>&1; then
    echo "Installing efibootmgr..."
    pacman -S --needed --noconfirm efibootmgr
fi

install -Dm755 /dev/stdin "$BIN" <<'EOF'
#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 meridian-woa contributors
# Keep the "Meridian Obsidian" entry first in the UEFI BootOrder.
set -eu
num=$(efibootmgr | sed -n 's/^Boot\([0-9A-Fa-f]\{4\}\)\*\{0,1\}[[:space:]]\{1,\}Meridian Obsidian.*/\1/p' | head -n1)
[ -n "$num" ] || exit 0
order=$(efibootmgr | sed -n 's/^BootOrder: //p')
case ",$order" in ",$num"*) exit 0 ;; esac
rest=$(printf '%s\n' "$order" | tr ',' '\n' | grep -vix "$num" | paste -sd, -)
efibootmgr -q -o "$num${rest:+,$rest}"
echo "meridian-bootorder: BootOrder -> $num${rest:+,$rest}"
EOF

cat >"$UNIT" <<'EOF'
[Unit]
Description=Keep Meridian Obsidian first in the UEFI BootOrder
After=local-fs.target
Before=shutdown.target
DefaultDependencies=no
Conflicts=shutdown.target

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/usr/local/bin/meridian-bootorder
ExecStop=/usr/local/bin/meridian-bootorder

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable --now meridian-bootorder.service
"$BIN" || true
echo
efibootmgr | sed -n '1,4p'
echo
echo "Done: Meridian Obsidian is put back first in BootOrder at every boot and shutdown/reboot."
