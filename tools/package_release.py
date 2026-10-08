"""Package only the release allowlist; never copy an original game tree."""
from pathlib import Path
import hashlib,json,zipfile
ROOT=Path(__file__).resolve().parents[1]
BUILD="20261007.3"
def digest(b): return hashlib.sha256(b).hexdigest()
def main():
    payload={
        "Bone Eater WS-Edition.exe":(ROOT/"build/launcher/Release/Bone Eater WS-Edition.exe").read_bytes(),
        "game/BoneEater.exe":(ROOT/"build/runtime/spicetools/64/Release/BoneEater.exe").read_bytes(),
        "docs/README.md":(ROOT/"README.md").read_bytes(),
        "docs/CREATION.md":(ROOT/"CREATION.md").read_bytes(),
        "docs/BUILD.md":(ROOT/"BUILD.md").read_bytes(),
        "docs/RELEASE_NOTES.md":(ROOT/"RELEASE_NOTES.md").read_bytes(),
        "docs/SOURCE.lock.json":(ROOT/"SOURCE.lock.json").read_bytes(),
        "docs/SUPPORTED_GAME.json":(ROOT/"SUPPORTED_GAME.json").read_bytes(),
        "docs/licenses/GPL-3.0.txt":(ROOT/"LICENSE").read_bytes(),
        "docs/licenses/third-party.txt":(ROOT/"THIRD_PARTY_LICENSES.txt").read_bytes(),
    }
    for name in ["title.png","news.png","phone.png"]+[f"{stem}{i:02d}.png" for i in range(12) for stem in ("introscreen","menuscreen")]:
        payload["game/desktop/"+name]=(ROOT/"assets/desktop"/name).read_bytes()
    settings={"schema_version":1,"view":"balanced125","main_dof_off":False,"force_1080p":True,"input_profile":None,"scope":{"mode":"toggle_hold","bindings":["ENTER","RBUTTON"],"hold_ms":250,"hold_release":"exit","low_gain":.25,"high_gain":.1,"low_smoothing_ms":35,"high_smoothing_ms":55,"shape":"angled"}}
    payload["launch-settings.json"]=(json.dumps(settings,indent=2)+"\n").encode()
    payload["game/ADD GAME FILES HERE.md"]=b"# Add game files here\n\nExtract the package to a short path such as C:\\BE. Read ../docs/README.md for setup and build information.\n\nCopy these original folders here: arkdata, data, modules, prop. conf is optional: existing settings are copied into ../user once.\n\nKeep BoneEater.exe and desktop. Run Bone Eater WS-Edition.exe in the parent folder. Do not run the original game first.\n\nThe launcher creates a separate user folder for writable NVRAM, cabinet marker, controls and main logs. Supplied conf and prop are not modified. Keep user when upgrading; do not copy another player's user folder.\n\nDefault force_1080p temporarily uses a 1920x1080 primary desktop with the working display signal retained through GPU scaling, restoring the captured configuration after game exit.\n"
    payload["docs/BUILD.json"]=(json.dumps({"build_id":BUILD,"release":"beta.1","files":{n:digest(b) for n,b in payload.items()}},indent=2)+"\n").encode()
    output=ROOT/"build/package";output.mkdir(parents=True,exist_ok=True)
    archive=output/"BoneEater-WSEdition.zip"
    with zipfile.ZipFile(archive,"x",zipfile.ZIP_DEFLATED,compresslevel=6) as z:
        for name,b in sorted(payload.items()):z.writestr(name,b)
    with zipfile.ZipFile(archive) as z:
        assert z.testzip() is None
        assert {name.split('/')[0] for name in z.namelist()} == {"game", "docs", "launch-settings.json", "Bone Eater WS-Edition.exe"}
        for name,b in payload.items():assert z.read(name)==b
    print(archive)
if __name__=="__main__":main()
