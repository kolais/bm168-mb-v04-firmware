#!/bin/sh
# The update's HTTP server on the host: the page, an upload that is taken,
# one that is refused, one that is too large, and nothing more after a taken
# one. The installer is a stand-in that says what app-slot would.
cd "$(dirname "$0")" || exit 1
T=$(mktemp -d); P=18473; fails=0; n=0
trap 'kill $pid 2> /dev/null; rm -rf "$T"' EXIT
cc -std=gnu11 -Wall -Wextra -Werror -DUPDATE_MAIN -o "$T/update" update.c || exit 1
cat > "$T/install" <<'SH'
#!/bin/sh
cp "$1" "$(dirname "$1")/seen"
if grep -q GOOD "$1"; then echo 'app: version 7 kept in slot a: it runs from the next start'; exit 0; fi
echo 'app: refused: the signature is not ours'; exit 1
SH
chmod 755 "$T/install"
printf '#!/bin/sh\nprintf "the logs of this session\\n" > "$1"\n' > "$T/logs"
chmod 755 "$T/logs"
"$T/update" $P "$T/bundle" "$T/install" "$T/status" 300000 $((P + 1)) 192.168.43.1 "$T/logs" &
pid=$!
sleep 0.5
ok() { n=$((n + 1)); if [ "$1" = 0 ]; then :; else fails=$((fails + 1)); echo "FAIL $2"; fi; }
U=http://127.0.0.1:$P
ok "$(grep -c '^waiting$' "$T/status" | grep -qx 1; echo $?)" 'status: waiting at the start'
curl -s $U/ | grep -q 'Software update'; ok $? 'the page'
[ "$(curl -s -o /dev/null -w '%{http_code}' $U/other)" = 404 ]; ok $? 'another path'
curl -s $U/ | grep -q 'href=/logs'; ok $? 'the page offers the logs'
[ "$(curl -s $U/logs)" = 'the logs of this session' ]; ok $? 'the logs: the file the command made'
curl -s -D - -o /dev/null $U/logs | grep -qi 'Content-Disposition: attachment; filename="autotop-logs.tar"'; ok $? 'the logs come as a file to save'
[ ! -e "$T/bundle.logs" ]; ok $? "the logs' file is removed afterwards"
curl -s -H 'Host: captive.apple.com' $U/logs | grep -q '<TITLE>Success</TITLE>'; ok $? 'by a name, even /logs is only the yes'
curl -s -H 'Host: captive.apple.com' $U/hotspot-detect.html | grep -q '<TITLE>Success</TITLE>'; ok $? "the phone's network check gets its yes"
curl -s -H 'Host: www.appleiphonecell.com' $U/ | grep -q '<TITLE>Success</TITLE>'; ok $? 'a check by another name and path gets the yes too, never the update page'
curl -s -H 'Host: 192.168.43.1' $U/ | grep -q 'Software update'; ok $? "the update page is at the unit's address"
[ "$(dig +short +tries=1 +time=2 @127.0.0.1 -p $((P + 1)) captive.apple.com A)" = 192.168.43.1 ]; ok $? 'every name is the unit: an Apple one'
[ "$(dig +short +tries=1 +time=2 @127.0.0.1 -p $((P + 1)) anything.example A)" = 192.168.43.1 ]; ok $? 'every name is the unit: any other'
dig +tries=1 +time=2 @127.0.0.1 -p $((P + 1)) captive.apple.com AAAA > "$T/aaaa"
grep -q 'status: NOERROR' "$T/aaaa" && grep -q 'ANSWER: 0' "$T/aaaa"; ok $? 'another kind of question: an empty answer, no error'
head -c 100000 /dev/urandom > "$T/bad"; curl -s --data-binary @"$T/bad" $U/upload > "$T/r1"
grep -q 'refused: the signature is not ours' "$T/r1" && grep -q 'Nothing was changed' "$T/r1"; ok $? 'a refused bundle: the reason reaches the phone'
grep -q '^refused app: refused: the signature is not ours$' "$T/status"; ok $? 'status after a refusal'
cmp -s "$T/bad" "$T/seen"; ok $? 'the installer saw the bytes that were sent'
[ ! -e "$T/bundle" ]; ok $? 'the received file is removed afterwards'
head -c 400000 /dev/urandom > "$T/big"
[ "$(curl -s -o /dev/null -w '%{http_code}' --data-binary @"$T/big" $U/upload)" = 413 ]; ok $? 'too large: refused unread'
{ head -c 150000 /dev/urandom; echo GOOD; } > "$T/good"; curl -s --data-binary @"$T/good" $U/upload > "$T/r2"
grep -q 'version 7 kept in slot a' "$T/r2" && grep -q 'The unit restarts now' "$T/r2"; ok $? 'a good bundle: taken'
grep -q '^done app: version 7 kept in slot a' "$T/status"; ok $? 'status after a taken bundle'
cmp -s "$T/good" "$T/seen"; ok $? 'the installer saw the good bundle whole'
[ "$(curl -s -o /dev/null -w '%{http_code}' --data-binary @"$T/good" $U/upload)" = 409 ]; ok $? 'nothing more is taken after it'
[ "$(curl -s -o /dev/null -w '%{http_code}' -X POST $U/upload)" = 409 ]; ok $? 'still nothing'
echo "$n checks, $fails failed"
[ $fails = 0 ]
