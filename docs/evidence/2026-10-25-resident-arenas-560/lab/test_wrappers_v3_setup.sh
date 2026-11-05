#!/bin/bash
# Meta-test for test_wrappers_v3.sh setup guards (#579 Cubic 4372411656): with mktemp failing on its first call
# (work directory) or on its second (stand-in directory), and with a bad LABDIR, test_wrappers_v3.sh must exit 2,
# leave no new /tmp entries, and leave this caller's own sentinel untouched.  usage: bash test_wrappers_v3_setup.sh LABDIR
set -uo pipefail
lab=$(realpath -e -- "$1") || { echo "bad LABDIR" >&2; exit 2; }
C=$(mktemp -d "/tmp/i560 setup meta-test.XXXX") || { echo "cannot create meta-test directory" >&2; exit 2; }
printf 'caller-owned sentinel\n' > "$C/caller sentinel" || { rm -rf -- "$C"; exit 2; }
mkdir "$C/bin" || { rm -rf -- "$C"; exit 2; }
real=$(command -v mktemp)
cat > "$C/bin/mktemp" <<SH
#!/bin/bash
n=\$(( \$(cat "$C/calls" 2>/dev/null || echo 0) + 1 )); echo \$n > "$C/calls"
[[ \$n -ge \${FAIL_AT:-1} ]] && exit 1
exec "$real" "\$@"
SH
chmod +x "$C/bin/mktemp"
fail=0; n=0
check(){ n=$((n+1)); if [[ $2 == "$3" ]]; then echo "PASS $1 ($3)"; else echo "FAIL $1: got $2 want $3"; fail=1; fi; }
listing(){ ls -1a /tmp | grep -E '^i560 (wrapper test v3|stand-in owner)' | sort; }
sum(){ sha256sum < "$C/caller sentinel"; }
before_list=$(listing); before_sum=$(sum)
for at in 1 2; do
 rm -f "$C/calls"
 FAIL_AT=$at PATH="$C/bin:$PATH" bash "$lab/test_wrappers_v3.sh" "$lab" > "$C/out-$at" 2>&1; rc=$?
 check "mktemp fails at call $at: exit 2, $at call(s), no new /tmp entries" "$rc/$(cat "$C/calls")/$([[ $(listing) == "$before_list" ]] && echo none-left || echo left-behind)" "2/$at/none-left"
done
bash "$lab/test_wrappers_v3.sh" "$C/no such lab" > /dev/null 2>&1; check "bad LABDIR exits 2 before mktemp" "$?/$([[ $(listing) == "$before_list" ]] && echo none-left || echo left-behind)" "2/none-left"
check "caller sentinel unchanged" "$([[ $(sum) == "$before_sum" ]] && echo same || echo changed)" "same"
rm -rf -- "$C"; echo "$n checks, failed=$fail"; exit $fail
