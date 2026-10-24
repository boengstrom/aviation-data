#!/bin/bash
# Run this ON the Hostinger VPS, as root, from the folder containing
# airline_service (binary), data/ and airline_service.cpp:
#     sudo ./deploy-vps.sh
# It installs the service to /opt/aviation and starts it on port 8080.
set -e
mkdir -p /opt/aviation
cp -r airline_service airline_service.cpp data /opt/aviation/
cp aviation.service /etc/systemd/system/
systemctl daemon-reload
systemctl enable --now aviation
sleep 1
systemctl --no-pager status aviation | head -5
echo
echo "Service is up:  http://<this-VPS-IP>:8080"
echo "Next: point your subdomain (e.g. aviation.engstrom.net) A record at"
echo "this VPS's IP in hPanel, then optionally add nginx + Let's Encrypt"
echo "for https:// on port 443 (see DEPLOY.md)."
