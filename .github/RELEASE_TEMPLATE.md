### Download

- [Installer](https://github.com/${REPO}/releases/download/${VERSION}/audient-device-closer_${VERSION}_setup.exe) - audient-device-closer_${VERSION}_setup.exe
- [Minimal PowerShell script](https://github.com/${REPO}/releases/download/${VERSION}/audient-device-closer_${VERSION}_minimal.ps1) - audient-device-closer_${VERSION}_minimal.ps1

### Run the minimal script

After downloading the script, run it from PowerShell:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\audient-device-closer_${VERSION}_minimal.ps1
```

Or run it without downloading it:

```powershell
# [WARN] This method is handy for a quick test, but it is not meant for everyday use because of safety concerns.
powershell -NoProfile -Command "irm https://github.com/${REPO}/releases/download/${VERSION}/audient-device-closer_${VERSION}_minimal.ps1 | iex"
```

### Changes
