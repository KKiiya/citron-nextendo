#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Package a Windows beta without a portable user directory or account data."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import zipfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--binary", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--public-client-id", required=True)
    parser.add_argument("--homebrew-dir", type=Path)
    args = parser.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9._-]{1,64}", args.version):
        parser.error("Invalid version label")
    if not re.fullmatch(r"[A-Za-z0-9_-]{1,128}", args.public_client_id):
        parser.error("Expected a public desktop OAuth client identifier")
    repo = Path(__file__).resolve().parents[1]
    runtime = args.runtime.resolve()
    if not (runtime / "citron.exe").is_file():
        runtime = runtime / "bin"
    if not (runtime / "citron.exe").is_file():
        parser.error("Runtime must contain citron.exe, directly or under bin")
    app = args.output.resolve() / ("Citron Beta" + args.version)
    if app.exists():
        parser.error("Output application directory already exists; preserve it or choose another output")
    app.mkdir(parents=True)
    # Allow runtime files explicitly; never copy profiles/configuration from the input.
    for file in runtime.iterdir():
        if file.is_file() and (file.suffix.lower() == ".dll" or file.name in
                               {"citron.exe", "citron-updater.exe", "qt.conf", "LICENSE"}):
            shutil.copy2(file, app / file.name)
    for directory in ("platforms", "imageformats", "iconengines", "styles", "tls",
                      "translations", "networkinformation", "multimedia", "LICENSES"):
        if (runtime / directory).is_dir():
            shutil.copytree(runtime / directory, app / directory)
    if args.binary:
        shutil.copy2(args.binary, app / "citron.exe")
    shutil.copy2(repo / "LICENSE", app / "LICENSE")
    if not (app / "LICENSES").exists():
        shutil.copytree(repo / "LICENSES", app / "LICENSES")
    if args.homebrew_dir:
        # The source manifest is the authority for each optional cached executable.
        manifest = (repo / "src/citron/homebrew_setup.inc").read_text(encoding="utf-8")
        for line in manifest.splitlines():
            if not line.strip().startswith('{"https://'):
                continue
            fields = re.findall(r'"([^"\n]+)"', line)
            file = args.homebrew_dir / fields[3]
            if hashlib.sha256(file.read_bytes()).hexdigest() != fields[5]:
                raise ValueError("Official homebrew hash mismatch: " + file.name)
            target = app / "homebrew/unpacked" / file.name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(file, target)
    (app / "nextendo-public-client-id.txt").write_text(args.public_client_id + "\n", encoding="ascii")
    for name, arguments in (("INICIAR CITRON BETA", ""), ("ABRIR HOME", "--home-menu"),
                            ("ABRIR ASISTENTE", "--setup-wizard")):
        (app / (name + ".cmd")).write_text(
            '@echo off\ncd /d "%~dp0"\nstart "" "%~dp0citron.exe" ' + arguments + ' %*\n',
            encoding="ascii")
    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=repo, text=True).strip()
    source = app / "source"
    source.mkdir()
    subprocess.run(["git", "archive", "--format=zip", "HEAD", "-o",
                    str(source / ("Citron-beta" + args.version + "-source.zip"))], cwd=repo, check=True)
    digest = hashlib.sha256((app / "citron.exe").read_bytes()).hexdigest()
    (app / "BETA_VERSION.txt").write_text(
        f"Beta {args.version}\nSource revision: {revision}\nExecutable SHA256: {digest}\n"
        "Windows default storage: %APPDATA%\\citron\nNo portable user directory is bundled.\n",
        encoding="utf-8")
    (app / "LEEME.md").write_text(
        "# Citron beta de pruebas\n\nWindows usa `%APPDATA%\\citron` cuando no hay una carpeta "
        "portable `user` junto al ejecutable. Este paquete no incluye `user`.\n\n"
        "Abre la beta desde una carpeta nueva. Si ya tienes Citron configurado en Roaming, "
        "conservará esos datos y no abrirá automáticamente el asistente. Para abrirlo "
        "manualmente usa Tools > Setup Wizard o ABRIR ASISTENTE.cmd.\n\n"
        "No borres una carpeta `user` antigua que contenga tus datos. Esta beta no migra ni "
        "mezcla perfiles portables automáticamente.\n\n"
        "Las herramientas homebrew se preparan en la SD virtual activa desde Tools > "
        "Prepare Homebrew Tools o al finalizar el asistente.\n\n"
        "Es una beta: el fallo al volver de Splatoon 3 a HOME y la edición de foto del "
        "perfil siguen pendientes. Consulta docs/beta54-3-acceptance.md en el código fuente.\n",
        encoding="utf-8")
    assert not (app / "user").exists()
    hashes = {p.relative_to(app).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
              for p in app.rglob("*") if p.is_file()}
    (app / "SHA256SUMS.json").write_text(json.dumps(hashes, indent=2), encoding="utf-8")
    archive = args.output.resolve() / ("Citron-Beta" + args.version + "-Windows-x64.zip")
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as package:
        for file in app.rglob("*"):
            if file.is_file():
                package.write(file, file.relative_to(args.output.resolve()))
    with zipfile.ZipFile(archive) as package:
        assert package.testzip() is None
        assert not any("/user/" in name for name in package.namelist())
    sha = hashlib.sha256(archive.read_bytes()).hexdigest()
    archive.with_suffix(archive.suffix + ".sha256").write_text(sha + "  " + archive.name + "\n", encoding="ascii")
    print(json.dumps({"archive": str(archive), "sha256": sha, "revision": revision}))


if __name__ == "__main__":
    main()
