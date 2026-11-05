# Changelog

## 1.0.0rc5 2026-11-05

- Generate the minimal PowerShell script at build time with the version and settings embedded
- Ship the minimal PowerShell script as a versioned release asset

## 1.0.0rc4 2026-11-05

- Ship the installer only; the portable zip archive is no longer provided
- Register the scheduled task only from the installed copy

## 1.0.0rc3 2026-11-02

- Support 16 Audient products
- Support several Audient devices that are connected at the same time
- General behaviour improvements

## 1.0.0rc2 2026-10-11

- Add an installer
- Bring the already running window to the front when the program is started a second time
- Run the repair task as LocalSystem so that it works for every account, and make it work correctly when several mixer instances hold the device
- Fix the log being wiped entirely once it grows large
- Other minor fixes

## 1.0.0rc1 2026-10-09

- Initial
