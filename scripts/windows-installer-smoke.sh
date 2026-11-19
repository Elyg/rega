#!/usr/bin/env bash
# CI only, on a Windows runner, from the folder holding the release's setup.exe and zip:
# checks the zip's rega.exe starts, then installs silently into a temporary folder, checks
# what the installer put there and in the registry, uninstalls and checks it's all gone.
set -euo pipefail
# Git Bash would turn /S and /D=… into paths.
export MSYS2_ARG_CONV_EXCL='*'

setup=$(ls rega-*-windows-x64-setup.exe)
zip=$(ls rega-*-windows-x64.zip)

# rega.exe writes --version to the console it attaches to, not to a pipe, so only its exit
# code tells: a missing DLL or a crash on start fails here.
tmp=$(mktemp -d)
python -m zipfile -e "$zip" "$tmp"
timeout 30 "$tmp/rega/rega.exe" --version || { echo "rega.exe --version failed ($?)"; exit 1; }
echo "rega.exe from $zip starts"

dir="$(cygpath -w "$(mktemp -d)")\\rega"
unix=$(cygpath -u "$dir")
key='HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall\rega'
progid='HKCU\Software\Classes\rega.file'

# /D= must come last and unquoted (the temp folder has no spaces).
# shellcheck disable=SC2086
"./$setup" /S /D=$dir
[ -f "$unix/rega.exe" ] || { echo "Not installed into $dir"; exit 1; }
[ -f "$unix/licenses/LICENSE" ] || { echo "licenses/ missing from $dir"; ls -la "$unix"; exit 1; }
# (Git Bash on the runners has no /dev/stderr to tee into.)
entry=$(reg query "$key" /v InstallLocation)
echo "$entry"
grep -qF "$dir" <<< "$entry" || { echo "Installed apps entry doesn't point at $dir"; exit 1; }
reg query "$progid\\shell\\open\\command" >/dev/null || { echo "No \"Open with rega\" ($progid)"; exit 1; }
timeout 30 "$unix/rega.exe" --version || { echo "Installed rega.exe --version failed ($?)"; exit 1; }
echo "Installed into $dir"

# The uninstaller runs a copy of itself from %TEMP% and returns at once: wait for the folder.
"$unix/Uninstall.exe" /S
for _ in $(seq 60); do
    [ -d "$unix" ] || break
    sleep 0.5
done
[ ! -d "$unix" ] || { echo "Uninstall left $dir behind"; ls -la "$unix"; exit 1; }
for k in "$key" "$progid"; do
    if reg query "$k" >/dev/null 2>&1; then
        echo "Uninstall left $k behind"
        exit 1
    fi
done
echo "Uninstalled"
