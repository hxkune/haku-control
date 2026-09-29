# Code signing

Releases of haku control are signed through [SignPath.io](https://signpath.io), with a certificate issued by the
[SignPath Foundation](https://signpath.org) for open-source projects. Windows then shows *SignPath Foundation* as
the publisher, and a changed or rebuilt copy is easy to tell apart from the official one.

Only builds made by GitHub Actions from this repository's source are signed (`.github/workflows/build.yml`):

1. the program's executables (`haku-control.exe`, `haku-control-open.exe`, `haku-control-hotspot.exe`),
2. the installer (`haku-control-setup.exe`), built from the signed executables.

Signing runs only for release tags (`v*`) and every release is approved by hand in SignPath. Without the
`SIGNPATH_API_TOKEN` secret the workflow builds unsigned, as before.

## Setting up (maintainer)

1. Apply at <https://signpath.org/apply> (text below), enable two-factor authentication on GitHub and SignPath.
2. In SignPath, once the project exists:
   - link the predefined trusted build system **GitHub.com** to the project and install the SignPath GitHub App on
     this repository;
   - create a signing policy (for example `release-signing`, manual approval);
   - create two artifact configurations with the slugs `program` and `installer`:

   `program`
   ```xml
   <?xml version="1.0" encoding="utf-8"?>
   <artifact-configuration xmlns="http://signpath.io/artifact-configuration/v1">
     <parameters>
       <parameter name="version" required="true" />
     </parameters>
     <zip-file>
       <pe-file path="haku-control.exe" product-name="haku control" product-version="${version}"><authenticode-sign /></pe-file>
       <pe-file path="haku-control-open.exe" product-name="haku control" product-version="${version}"><authenticode-sign /></pe-file>
       <pe-file path="haku-control-hotspot.exe" product-name="haku control" product-version="${version}"><authenticode-sign /></pe-file>
     </zip-file>
   </artifact-configuration>
   ```

   `installer`
   ```xml
   <?xml version="1.0" encoding="utf-8"?>
   <artifact-configuration xmlns="http://signpath.io/artifact-configuration/v1">
     <parameters>
       <parameter name="version" required="true" />
     </parameters>
     <zip-file>
       <pe-file path="haku-control-setup.exe" product-name="haku control" product-version="${version}"><authenticode-sign /></pe-file>
     </zip-file>
   </artifact-configuration>
   ```
3. In the GitHub repository (*Settings → Secrets and variables → Actions*):
   - secret `SIGNPATH_API_TOKEN` (a SignPath user with the *submitter* role),
   - variables `SIGNPATH_ORGANIZATION_ID`, `SIGNPATH_PROJECT_SLUG`, `SIGNPATH_SIGNING_POLICY_SLUG`.

The next `v*` tag then waits for the approval in SignPath and publishes signed files.

## Application text

- **Project name:** haku control
- **Repository:** https://github.com/hxkune/haku-control
- **License:** GPL-3.0-only
- **Description:** Windows tray app that drives PC and room RGB lighting: MSI Mystic Light boards and ENE DRAM
  (through the PawnIO driver), Nanoleaf, Philips Hue, WLED, OpenRGB, LIFX, Govee, Yeelight, WiZ and AiDot lights over
  the local network. Written in C with a WebView2 settings window; built by GitHub Actions.
- **Released artifacts:** `haku-control-setup.exe` (installer) and a zip with the same files, on GitHub Releases.
- **Third-party components:** Microsoft WebView2 SDK loader (BSD-3-Clause), PawnIO SMBus module (LGPL-2.1,
  unmodified, from namazso/PawnIO.Modules). No other bundled code.
