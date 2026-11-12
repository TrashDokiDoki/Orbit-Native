import hashlib
import io
import pathlib
import shutil
import sys
import urllib.request
import zipfile


WHEELS = {
    "x64": (
        "https://files.pythonhosted.org/packages/fc/ca/83398cfcd557360a3d7b2d732aee1c5f6999f68618d1645f38d53e14c9ff/"
        "vosk-0.3.45-py3-none-manylinux_2_12_x86_64.manylinux2010_x86_64.whl",
        "25e025093c4399d7278f543568ed8cc5460ac3a4bf48c23673ace1e25d26619f",
    ),
    "arm64": (
        "https://files.pythonhosted.org/packages/a4/23/3130a69fa0bf4f5566a52e415c18cd854bf561547bb6505666a6eb1bb625/"
        "vosk-0.3.45-py3-none-manylinux2014_aarch64.whl",
        "54efb47dd890e544e9e20f0316413acec7f8680d04ec095c6140ab4e70262704",
    ),
}
MODELS = {
    "ru": ("vosk-model-small-ru-0.22", "961d5ff98a17f4aa6de69864d0aa71fa5bac682301d2b5d17a3f24c5c99a46d4"),
    "en": ("vosk-model-small-en-us-0.15", "30f26242c4eb449f948e42cb302dd7a686cb29a3423a8367f99ff41780942498"),
}


def download(url, digest):
    with urllib.request.urlopen(url, timeout=60) as response:
        content = response.read(60 * 1024 * 1024)
    if hashlib.sha256(content).hexdigest() != digest:
        raise ValueError(f"Unexpected SHA-256 for {url}")
    return content


def main(architecture, target):
    target.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(io.BytesIO(download(*WHEELS[architecture]))) as wheel:
        (target / "libvosk.so").write_bytes(wheel.read("vosk/libvosk.so"))

    (target / "Vosk-LICENSE").write_bytes(download(
        "https://raw.githubusercontent.com/alphacep/vosk-api/v0.3.45/COPYING",
        "bb618af833c817ab01c1564bd3ec75ba4b4f9cf30910a4a2168e30a674ee8cc6",
    ))

    for language, (name, digest) in MODELS.items():
        content = download(f"https://alphacephei.com/vosk/models/{name}.zip", digest)
        with zipfile.ZipFile(io.BytesIO(content)) as model:
            for entry in model.infolist():
                relative = pathlib.PurePosixPath(entry.filename)
                if entry.is_dir() or not relative.parts or relative.parts[0] != name:
                    continue
                parts = relative.parts[1:]
                if not parts or ".." in parts:
                    raise ValueError(f"Unexpected model entry {entry.filename}")
                destination = target.joinpath(language, *parts)
                destination.parent.mkdir(parents=True, exist_ok=True)
                with model.open(entry) as source, destination.open("wb") as file:
                    shutil.copyfileobj(source, file)


if __name__ == "__main__":
    main(sys.argv[1], pathlib.Path(sys.argv[2]))
