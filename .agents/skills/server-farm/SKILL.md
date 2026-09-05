---
name: server-farm
description: Provision and manage a CloudLab server farm for running astra-sim experiments remotely
---

# Server Farm Provisioning & Management

## Server List

Ask the user for the file containing the list of servers (one `user@host` per line). Read that file to get the target hosts.

SSH is passwordless via key-based auth. Always use `-o StrictHostKeyChecking=no -o ConnectTimeout=10`.

## Provisioning Steps (in order)

### 1. System update and reboot

```bash
ssh <server> "sudo apt update && sudo apt upgrade -y && sudo reboot"
```

Run on all servers in parallel. After issuing reboot, poll every 60s with `ssh <server> uptime` until all respond.

### 2. Install packages

```bash
ssh <server> '
    sudo apt install -y docker.io docker-buildx htop build-essential python3-pip libhwloc-dev libscotch-dev
    pkg-config libjemalloc-dev cmake g++ libprotobuf-dev protobuf-compiler libscotch-dev libhwloc-dev libjemalloc-dev
    libboost-dev rsync criu && sudo apt autoremove -y
'
```

### 3. User and permissions setup

```bash
ssh <server> "sudo usermod -aG docker <user> && sudo chown -R <user>:<group> /workspace"
```

- Add the user to the `docker` group (requires new session to take effect; use `sudo docker` until then).
- `/workspace` is the shared working directory.

## Operational Notes

- All commands that touch Docker must use `sudo` until the user logs out and back in (group membership).
- Run commands on all servers in parallel for speed; use background tasks and collect results.
- After reboot, poll with SSH connectivity checks every 60s until all nodes respond.
