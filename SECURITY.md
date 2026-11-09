# Security

## Reporting a problem

If you find a security problem (for example a save, mod, cheat or update file that can make the game or the launcher
run code, write outside its folder or download something unexpected), **don't open a public issue**. Use
**Security > Report a vulnerability** on this repository's GitHub page instead, so it can be fixed before it is
public. Include the version, what you did and what happened. Expect an answer within a few days.

Crashes, graphics problems and setup errors are not security problems: use the
[bug report form](../../issues/new?template=bug_report.yml) for those.

## Getting the game safely

- Download only from this repository's **[Releases](../../releases)** page. Releases are built and published by the
  GitHub Actions workflow from the source in this repository, never uploaded by hand.
- Each release has a `SHA256SUMS.txt` file. To check a download, run this in PowerShell in the download folder and
  compare the result with the line for that file:

  ```powershell
  Get-FileHash .\MetroidPrime4Recomp-PC-vX.Y.Z.zip -Algorithm SHA256
  ```

  If it doesn't match, delete the file and download it again from the Releases page.
- The Play screen's in-place updates come from the same releases and check every file before replacing anything.
- Copies from other sites, re-uploads and "repacks" are not from this project. Never run one that asks for your
  console keys, passwords or payment details: this project never needs them.

## Your data

Logs (`Metroid Prime 4 Beyond.log`, `setup.log`, traces) contain your Windows user name in file paths. Check them
before attaching them to a public issue. Never post console keys, title keys or game files.

## Supported versions

Only the newest release gets fixes. The Play screen installs new versions by itself; if yours is older, update first.
