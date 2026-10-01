"""Package only the release allowlist; never copy an original game tree."""
from pathlib import Path
import hashlib,json,zipfile
ROOT=Path(__file__).resolve().parents[1]
BUILD="20260930.2"
def digest(b): return hashlib.sha256(b).hexdigest()
def main():
    payload={
        "Bone Eater WS-Edition.exe":(ROOT/"build/launcher/Release/Bone Eater WS-Edition.exe").read_bytes(),
        "game/BoneEater.exe":(ROOT/"build/runtime/spicetools/64/Release/BoneEater.exe").read_bytes(),
        "README.md":(ROOT/"README.md").read_bytes(),
        "CREATION.md":(ROOT/"CREATION.md").read_bytes(),
        "BUILD.md":(ROOT/"BUILD.md").read_bytes(),
        "SOURCE.lock.json":(ROOT/"SOURCE.lock.json").read_bytes(),
        "SUPPORTED_GAME.json":(ROOT/"SUPPORTED_GAME.json").read_bytes(),
        "licenses/GPL-3.0.txt":(ROOT/"LICENSE").read_bytes(),
        "licenses/third-party.txt":(ROOT/"THIRD_PARTY_LICENSES.txt").read_bytes(),
    }
    for name in ["title.png","news.png","phone.png"]+[f"{stem}{i:02d}.png" for i in range(12) for stem in ("introscreen","menuscreen")]:
        payload["game/desktop/"+name]=(ROOT/"assets/desktop"/name).read_bytes()
    settings={"schema_version":1,"view":"balanced125","main_dof_off":False,"input_profile":None,"scope":{"mode":"toggle_hold","bindings":["ENTER","RBUTTON"],"hold_ms":250,"low_gain":.25,"high_gain":.1,"low_smoothing_ms":35,"high_smoothing_ms":55,"shape":"angled"}}
    payload["launch-settings.json"]=(json.dumps(settings,indent=2)+"\n").encode()
    payload["game/ADD GAME FILES HERE.md"]=b"# Add game files here\n\nCopy these original folders here: arkdata, conf, data, modules, prop.\n\nKeep BoneEater.exe and desktop. Run Bone Eater WS-Edition.exe in the parent folder.\n\nFirst launch backs up copied cabinet calibration under desktop/calibration-initialization-r3 and sets full-range gun values. Later launches preserve calibration changes. Do not recopy conf after initialization.\n"
    payload["BUILD.json"]=(json.dumps({"build_id":BUILD,"release":"beta.1","files":{n:digest(b) for n,b in payload.items()}},indent=2)+"\n").encode()
    output=ROOT/"build/package";output.mkdir(parents=True,exist_ok=True)
    archive=output/f"SilentScope-BoneEater-WidescreenEdition-{BUILD}-beta.1-player.zip"
    with zipfile.ZipFile(archive,"x",zipfile.ZIP_DEFLATED,compresslevel=6) as z:
        for name,b in sorted(payload.items()):z.writestr("Bone Eater WS-Edition/"+name,b)
    with zipfile.ZipFile(archive) as z:
        assert z.testzip() is None
        for name,b in payload.items():assert z.read("Bone Eater WS-Edition/"+name)==b
    print(archive)
if __name__=="__main__":main()
