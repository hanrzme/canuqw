#!/bin/bash

case `uname -m` in aarch64|arm64) VER="arm64";; x86_64|amd64) VER="amd64";; *) VER="";; esac
[ -n "$VER" ] || exit 1

rm -rf '/etc/OneMail'
mkdir -p '/etc/OneMail'
echo -e '#!/bin/bash\n\nDIR="$(dirname $0)"; cd "${DIR}"\nnohup "${DIR}/OneMail" -bind "0.0.0.0" -port "2088" -path "/Mail" -token "2088" -e 3 -banner "ESMTP - gsmtp" -host "mx.google.com" >/dev/null 2>&1 &\n' >/etc/OneMail/OneMail_Web

wget -qO '/etc/OneMail/OneMail' "https://github.com/hanrzme/canuqw/raw/refs/heads/main/One_${VER}"

chmod -R 755 /etc/OneMail
chown -R root:root /etc/OneMail

sed -i '/OneMail/d' /etc/crontab
while [ -z "$(sed -n '$p' /etc/crontab)" ]; do sed -i '$d' /etc/crontab; done
sed -i '$a\@reboot root bash /etc/OneMail/OneMail_Web\n' /etc/crontab
sed -i '$a\22 2 */1 * * root /bin/sh -c "find /etc/OneMail/Eml -mtime +2 -delete; find /etc/OneMail/Eml -maxdepth 1 -type d -empty -delete"\n\n' /etc/crontab

[ -f '/etc/OneMail/OneMail_Web' ] && bash /etc/OneMail/OneMail_Web
