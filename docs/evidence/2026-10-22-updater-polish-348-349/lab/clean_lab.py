"""#348/#349 lab clean-up on d69 (after #293 phase 3 clean_lab.py): list (dry) or delete every object the signer lab scope
tracks plus this run receipts, invalidate /lab/*, and remove the local lab workspace and publisher state. Before this run
published v0.1.1-nightly.9002 the lab scope had no publisher state, so everything it lists is this run."""
import json, shutil, sys
from pathlib import Path
SRC, CONFIG, ACTION = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3]
TAGS = sys.argv[4:]
sys.path.insert(0, str(SRC / "tools/releases"))
import pkgrepo  # noqa: E402
config = pkgrepo.Config.load(CONFIG)
state_path = config.state / "published" / "lab-nightly.json"
state = json.loads(state_path.read_text())
keys = sorted(set(state["objects"]) | set(state["pending"])) + [f"lab/receipts/{t}.json" for t in TAGS]
for key in keys:
    assert key.startswith("lab/"), key
if ACTION == "list":
    print("\n".join(keys)); print(len(keys), "keys"); sys.exit()
s3, cloudfront = pkgrepo.Signer(config).clients()
for key in keys:
    s3.delete_object(Bucket=config.bucket, Key=key)
print("deleted", len(keys), "lab objects")
answer = cloudfront.create_invalidation(DistributionId=config.distribution, InvalidationBatch={
    "Paths": {"Quantity": 1, "Items": ["/lab/*"]}, "CallerReference": "putgd-348-349-lab-clean"})
print("invalidation", answer["Invalidation"]["Id"])
shutil.rmtree(config.state / "workspaces" / "lab")
state_path.unlink()
print("lab workspace and publisher state removed")
