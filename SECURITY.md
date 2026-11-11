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

## Antivirus warnings

Windows Defender or SmartScreen can warn about a new release, for example as `Trojan:Script/Sabsik.EN.A!ml`. The
`!ml` at the end means a machine-learning guess about a file few people have downloaded yet, not a match with known
malware. New, unsigned programs that unpack and build code, as this one does with your game dump, get these guesses
often, and they usually stop once the file has been seen for a while.

How to check a release yourself instead of trusting this page:

1. **Compare the checksum** with `SHA256SUMS.txt` (above). A match means the file is exactly what the GitHub Actions
   workflow built from this source: open the workflow run under **Actions** to see every build step.
2. **Look the checksum up on VirusTotal**: paste it into the search box at <https://www.virustotal.com>. Nothing is
   uploaded, and you see what some 60 antivirus engines say about that exact file.
3. **Read what is inside**: the zip holds the game's launcher (`Metroid Prime 4 Beyond.exe`), `SDL2.dll`, `zlib1.dll`,
   license texts, and a `recompiler` folder with the GCC compiler, Python and the Python scripts that turn your own
   game dump into a PC program. The scripts are plain text. There is no installer, no service and nothing that starts
   with Windows.

For 2026.11.15 (`MetroidPrime4Recomp-PC-v2026.11.15.zip`, SHA-256
`bbf9342bc369382aef981be3743c826ed45c17bb6a1b0a148b764495fe8b7278`), VirusTotal showed 1 of 55 engines flagging it on
2026-11-10: one machine-learning engine, at moderate confidence. Microsoft's own engine there reported nothing.

If Defender blocks a download, don't turn protection off. Report the file to Microsoft as a false positive at
<https://www.microsoft.com/wdsi/filesubmission> ("Software developer" or "Home customer", "Incorrectly detected"),
which is what clears it for everyone, and please tell us in an issue which file and which detection name.

## Your data

Logs (`Metroid Prime 4 Beyond.log`, `setup.log`, traces) contain your Windows user name in file paths. Check them
before attaching them to a public issue. Never post console keys, title keys or game files.

## Supported versions

Only the newest release gets fixes. The Play screen installs new versions by itself; if yours is older, update first.
