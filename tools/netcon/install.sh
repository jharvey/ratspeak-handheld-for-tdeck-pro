#!/bin/sh
# Install the remote console relay as a service on the relay machine (the build VM).
#   scp tools/netcon/{relay.py,rscon.py,install.sh} debian-build:rscon/ && ssh debian-build sh rscon/install.sh
# Makes ~/rscon/key once (20 letters and digits: the same key goes on every unit).
set -e
D="$HOME/rscon"
mkdir -p "$D/logs"
if [ ! -s "$D/key" ]; then
    python3 -c "import secrets,string; print(''.join(secrets.choice(string.ascii_lowercase+string.digits) for _ in range(20)))" > "$D/key"
    chmod 600 "$D/key"
fi
sudo tee /etc/systemd/system/rscon-relay.service >/dev/null <<EOF
[Unit]
Description=T-Deck Pro remote console relay
After=network-online.target
Wants=network-online.target

[Service]
User=$(id -un)
ExecStart=/usr/bin/python3 -u $D/relay.py --dir $D --port 4404
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target
EOF
sudo systemctl daemon-reload
sudo systemctl enable --now rscon-relay
sudo systemctl restart rscon-relay
sleep 1
systemctl --no-pager status rscon-relay | head -5
