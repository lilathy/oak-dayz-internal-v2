import json
from pathlib import Path

p = Path(r"..\..\..\clients\dayz\docs\engine\full_dump_results.json")
r = json.loads(p.read_text(encoding="utf-8"))
r["code.AimingModelRecoil"] = {
    "status": "RESOLVED",
    "old": 0x4E9D16,
    "new": 0x4E9126,
    "note": "pattern mov eax,[rbx+0x2760] epilogue",
}
p.write_text(json.dumps(r, indent=2), encoding="utf-8")
sites = Path(r"..\..\..\clients\dayz\docs\engine\CODE_SITES.txt")
lines = []
for k, v in sorted(r.items()):
    if k.startswith("code.") and v.get("new"):
        lines.append(f"{k} = 0x{int(v['new']):X}")
sites.write_text("\n".join(lines) + "\n", encoding="utf-8")
print("ok", len(lines))
