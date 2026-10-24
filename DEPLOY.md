# Deploying to a real URL with Hostinger

The site is a compiled C++ program that listens on port 8080, so it needs a
server that can run a binary: a **Hostinger VPS (KVM)**. Hostinger *shared*
hosting can't run it (no custom programs, no port 8080). If the account only
has shared/email hosting, add the cheapest KVM VPS — everything else is
already prepared.

## What you get

A public URL like **https://aviation.engstrom.net** (any subdomain of the
domain already on the account works) that anyone — including Kleinman — can
open any time, no office-hours scheduling needed.

## Steps

1. **VPS**: in hPanel, make sure a KVM VPS is active (Ubuntu 24.04 is fine).
   Note its public IP address.
2. **DNS**: in hPanel → Domains → engstrom.net → DNS, add an **A record**:
   host `aviation`, points to the VPS IP. (DNS usually propagates in minutes.)
3. **Upload** this project to the VPS (scp the deploy tarball, unpack it).
4. **Install**: `sudo ./deploy-vps.sh` — copies everything to /opt/aviation,
   registers the systemd service, starts it on port 8080.
   Quick check: `http://<VPS-IP>:8080` should show the site.
5. **HTTPS (optional but recommended)**: install nginx + certbot on the VPS,
   reverse-proxy `aviation.engstrom.net` → `localhost:8080`, and let certbot
   issue a free Let's Encrypt certificate. Then the real URL is
   `https://aviation.engstrom.net`.

## Alternative: Docker

`docker build -t aviation . && docker run -d -p 8080:8080 aviation`
The included Dockerfile compiles from source, so the same image also works
on Render/Railway if plans change.

## Notes

- The student ID is compiled into the binary — after any ID change, rebuild
  before redeploying (`start.sh` rebuilds automatically when the source is
  newer than the binary).
- The service restarts itself on failure and on VPS reboot (systemd).
