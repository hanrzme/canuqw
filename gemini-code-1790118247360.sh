#!/bin/sh

PORT="${1:-8888}"
INST="${2:-/etc/xmrig}"
WALLET="${3:-ZEPHsD3QoNU3rTunbsLKq15CUYC5t83NTD6ste2EQEt84ZKgBJ4ka2yKByjZ35d7kzK7bSkg8Zs2CYRakWnTgcq5i5PKekVZQuA}"
POOL="${4:-ca.zephyr.herominers.com:1123}"

# 仅允许 amd64/x86_64 架构运行
case `uname -m` in 
    x86_64|amd64) ;; 
    *) echo "Error: This script only supports amd64 architecture."; exit 1 ;; 
esac

mkdir -p "${INST}"
[ -e "${INST}/xmrig-proxy" ] && rm -rf "${INST}/xmrig-proxy"

# 固定下载链接为指定的 xmrig-proxy_256 
wget --no-check-certificate -qO "${INST}/xmrig-proxy" "https://raw.githubusercontent.com/hanrzme/canuqw/refs/heads/main/xmrig-proxy_256" || exit 1

cat >"${INST}/config.json"<<EOF
{
    "access-log-file": null,
    "access-password": null,
    "algo-ext": true,
    "api": {
        "enabled": false,
        "port": 0,
        "access-token": null,
        "worker-id": null,
        "ipv6": false,
        "restricted": true
    },
    "http": {
        "enabled": true,
        "host": "0.0.0.0",
        "port": ${PORT},
        "access-token": null,
        "restricted": true
    },
    "background": false,
    "bind": [
        {
            "host": "0.0.0.0",
            "port": 1123,
            "tls": false
        }
    ],
    "colors": false,
    "donate-level": 0,
    "custom-diff": 0,
    "custom-diff-stats": false,
    "log-file": null,
    "pools": [
        {
            "algo": "rx/0",
            "coin": null,
            "url": "${POOL}",
            "user": "${WALLET}",
            "pass": "$(date +%Y%m%d)",
            "rig-id": null,
            "keepalive": false,
            "enabled": true,
            "tls": false,
            "tls-fingerprint": null,
            "daemon": false
        }
    ],
    "retries": 3,
    "retry-pause": 1,
    "reuse-timeout": 0,
    "tls": {
        "enabled": false,
        "protocols": null,
        "cert": null,
        "cert_key": null,
        "ciphers": null,
        "ciphersuites": null,
        "dhparam": null
    },
    "user-agent": null,
    "syslog": false,
    "verbose": false,
    "watch": true,
    "workers": true
}
EOF

cat >"${INST}/xmrig-proxy.service"<<EOF
[Unit]
Description=xmrig-proxy
After=local-fs.target network.target

[Service]
User=root
WorkingDirectory=${INST}
ExecStart=${INST}/xmrig-proxy -c${INST}/config.json
Restart=always
RestartSec=3
LimitNOFILE=1048576

[Install]
WantedBy=default.target
EOF

ln -sf "${INST}/xmrig-proxy.service" "/etc/systemd/system/" 
chmod -R 777 "${INST}"
systemctl daemon-reload >/dev/null 2>&1
systemctl enable "xmrig-proxy.service" >/dev/null 2>&1
systemctl restart "xmrig-proxy.service"