#!/bin/bash

current=`wget -qO- 'checkip.amazonaws.com' |grep -o '[0-9\.]*'`
zoneFile="/etc/bind/random_dlz/default"
[ -f "${zoneFile}" ] || exit 1
record=`cat "${zoneFile}" |grep "^ns1[[:space:]]\+" |grep -o '[0-9]\{1,3\}\.[0-9]\{1,3\}\.[0-9]\{1,3\}\.[0-9]\{1,3\}'`
[ "$current" == "$record" ] && exit 0

SERIAL=`cat "${zoneFile}" |grep "[[:space:]]\+SOA[[:space:]]\+" |head -n1 |grep -o '[0-9]\+' |tail -n5 |head -n1` || SERIAL=`date +%y%m%d0000`
SERIAL=$((SERIAL+1))

sed -i "/[[:space:]]\+SOA[[:space:]]\+/c\@        300  SOA    ns1.%ZONE% hostmaster.%ZONE% ${SERIAL} 300 60 86400 300" "/etc/bind/random_dlz/default"

sed -i "s/^ns1[[:space:]]\+[0-9]\+[[:space:]]\+A[[:space:]]\+.*/ns1      300  A     ${current}/" "/etc/bind/random_dlz/default"
sed -i "s/^ns2[[:space:]]\+[0-9]\+[[:space:]]\+A[[:space:]]\+.*/ns2      300  A     ${current}/" "/etc/bind/random_dlz/default"
sed -i "s/^ns3[[:space:]]\+[0-9]\+[[:space:]]\+A[[:space:]]\+.*/ns3      300  A     ${current}/" "/etc/bind/random_dlz/default"
sed -i "s/^ns4[[:space:]]\+[0-9]\+[[:space:]]\+A[[:space:]]\+.*/ns4      300  A     ${current}/" "/etc/bind/random_dlz/default"

