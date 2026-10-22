"""#344 lab clean-up on d69: delete every object the signer lab scope tracks for lab nightly (only 9102 of #344 at this point) and its receipt, invalidate /lab/*, remove the lab workspace and lab-nightly publisher state."""
import json, shutil, sys
from pathlib import Path
SRC, CONFIG = Path(sys.argv[1]), Path(sys.argv[2])
sys.path.insert(0, str(SRC / "tools/releases"))
import pkgrepo
config = pkgrepo.Config.load(CONFIG)
s3, cloudfront = pkgrepo.aws_clients(config)
state_path = config.state / "published" / "lab-nightly.json"
state = json.loads(state_path.read_text())
keys = sorted(set(state["objects"]) | set(state["pending"])) + ["lab/receipts/v0.1.1-nightly.9102.json"]
for key in keys:
    assert key.startswith("lab/"), key
    s3.delete_object(Bucket=config.bucket, Key=key)
print("deleted", len(keys))
answer = cloudfront.create_invalidation(DistributionId=config.distribution, InvalidationBatch={"Paths": {"Quantity": 1, "Items": ["/lab/*"]}, "CallerReference": "putgd-344-lab-clean"})
print("invalidation", answer["Invalidation"]["Id"])
shutil.rmtree(config.state / "workspaces" / "lab", ignore_errors=True)
state_path.unlink()
print("lab workspace and lab-nightly publisher state removed")
