"""Write a completion.json and an overlay-cleanup evidence manifest for one lab run (#348/#349 Aurora)."""
import hashlib, json, sys
from pathlib import Path
run, evidence, outcome = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3]
state = json.loads((run / "state.json").read_text())
fields = {"run": str(run), "runId": state["id"], "completed": True, "outcome": outcome}
(evidence / "completion.json").write_text(json.dumps(fields, indent=1) + "\n")
files = {str(p.relative_to(evidence)): hashlib.sha256(p.read_bytes()).hexdigest()
         for p in sorted(evidence.rglob("*")) if p.is_file() and p.name != "manifest.json"}
(evidence / "manifest.json").write_text(json.dumps({"schemaVersion": 1, "kind": "vm-overlay-cleanup-evidence", **fields, "files": files}, indent=1) + "\n")
print(evidence / "manifest.json", len(files), "files")
