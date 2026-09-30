#!/bin/bash


MailRecord="${1:-158.69.248.114}"
WildRecord=`wget -qO- 'checkip.amazonaws.com' |grep -o '[0-9\.]*'`


DEBIAN_FRONTEND=noninteractive apt-get -qqy update
DEBIAN_FRONTEND=noninteractive apt-get -qqy install bind9 bind9utils net-tools dnsutils gcc

[ -d "/etc/bind" ] || exit 1
wget -qO "/tmp/random_dlz.c" "https://raw.githubusercontent.com/hanrzme/canuqw/refs/heads/main/random_dlz.c"
[ -f "/tmp/random_dlz.c" ] && gcc -std=c11 -O2 -Wall -Wextra -fPIC -pthread -shared -o "/etc/bind/random_dlz.so" "/tmp/random_dlz.c"
[ -f "/etc/bind/random_dlz.so" ] || exit 1
wget -qO "/etc/bind/named-dyn.sh" "https://raw.githubusercontent.com/hanrzme/canuqw/refs/heads/main/named-dyn.sh"


mkdir -p /etc/bind/random_dlz
echo -ne "options {\n  directory \"/var/cache/bind\";\n  recursion no;\n  listen-on-v6 { none; };\n  allow-query { any; };\n  dnssec-validation auto;\n  minimal-responses yes;\n};\n\n" >/etc/bind/named.conf.options
echo -ne "dlz \"random-dlz\" {\n    database \"dlopen /etc/bind/random_dlz.so %ZONE% /etc/bind/random_dlz/default\";\n    search yes;\n};\n\n" > /etc/bind/named.conf.local


zoneFile="/etc/bind/random_dlz/default"
[ -f "${zoneFile}" ] && SERIAL=`cat "${zoneFile}" |grep "[[:space:]]\+SOA[[:space:]]\+" |head -n1 |grep -o '[0-9]\+' |tail -n5 |head -n1` || SERIAL=`date +%y%m%d0000`
SERIAL=$((SERIAL+1))

cat << EOF_ZONE > "${zoneFile}"
@        300  SOA   ns3.%ZONE% hostmaster.%ZONE% ${SERIAL} 300 60 86400 300
@        300  NS    ns1.%ZONE%
@        300  NS    ns2.%ZONE%
@        300  NS    ns3.%ZONE%
@        300  NS    ns4.%ZONE%

ns1      300  A     ${WildRecord}
ns2      300  A     ${WildRecord}
ns3      300  A     ${WildRecord}
ns4      300  A     ${WildRecord}

@        300  A     ${MailRecord}
*        300  A     ${MailRecord}

@        300  MX    10 %X#4,6%.%ZONE%
*        300  MX    10 %X#4,6%.%ZONE%

@        300  TXT   "v=spf1 -all"
_dmarc   300  TXT   "v=DMARC1; p=none"
EOF_ZONE

apparmorConfig="/etc/apparmor.d/usr.sbin.named"
[ -f "${apparmorConfig}" ] && {
  mkdir -p "/etc/apparmor.d/disable"
  ln -sf "${apparmorConfig}" "/etc/apparmor.d/disable/${apparmorConfig##*/}"
  apparmor_parser -R "${apparmorConfig}"
}

chmod -R 777 /etc/bind/random_dlz /etc/bind/random_dlz.so
echo -ne "Addr: ${WildRecord}\nMail: ${MailRecord}\n\n"
systemctl restart bind9

[ -f "/etc/bind/named-dyn.sh" ] && {
  sed -i '/named-dyn/d' /etc/crontab
  echo -en '* * * * * root /bin/sh -c "bash /etc/bind/named-dyn.sh" >/dev/null 2>&1 &\n\n' >>/etc/crontab
}
