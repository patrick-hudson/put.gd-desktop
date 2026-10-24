# cap.sh RUN: desktop capture through the resident, then its operation's final state
base=$HOME/putgd-lab/i523; run=$base/runs/$1
root=$base/x-$(cat "$run/tag")/squashfs-root
c() { XDG_RUNTIME_DIR=/run/user/$(id -u) LD_LIBRARY_PATH="$root/usr/lib:$root/usr/lib/putgd" "$root/usr/bin/putgd-cli" "$@"; }
id=$(c action.invoke '{"action":"desktop"}' | python3 -c 'import json,sys; r=json.load(sys.stdin); print(r["result"].get("operationId") or r["result"].get("operation",{}).get("id","") or json.dumps(r))')
echo "op $id"
sleep 6
c operation.inspect "{\"id\":\"$id\"}" 2>/dev/null | python3 -c 'import json,sys; r=json.load(sys.stdin); o=r["result"].get("operation",r["result"]); print(json.dumps({k:o.get(k) for k in ("state","phase","error")}))' || c operations.list '{}' | head -c 600
ls -la "$run/archive" 2>/dev/null | tail -n +2; find "$run/archive" -type f | head
