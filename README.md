# RainBackup

RainBackup is a Windows x64 utility for backing up Rainmeter skins, settings, layouts, custom plugins, and additional files you select.

Download the installer ZIP from [GitHub Releases](https://github.com/jsmorley/RainBackup/releases), extract it, and run `RainBackup-Setup.exe`. See the [user manual](manual/RainBackup-Manual.md) for installation and operation.

## Backup

- **Scan** looks for the installed Rainmeter and running portable copies. **Scan folder** searches a folder you choose for portable installations. RainBackup remembers the sources and checkbox selections.
- Expand the tree to individual files. **Add folder** and **Add file** can include anything you consider part of your setup, such as your plugin SDK. Select a source to use **Change path**, **Remove folder**, or **Remove file**. Uncheck individual items inside a source to exclude them from backups.
- Enter a destination and filename in **Backup ZIP**, or use **Browse**. **Back up now** previews the selected items and ZIP path before writing. Review scan notices before backing up. Selected OneDrive files may download on demand. If a selected file cannot be read, an existing ZIP stays intact.

## Restore

- **Open ZIP** opens a backup. Use the dropdown to switch between ZIPs in the same folder. Check individual items or click **Select all**.
- **Delete backup** permanently deletes the selected ZIP after showing its full path and asking for confirmation. With no ZIP selected, it opens a file picker.
- **Original location** uses the paths stored in the archive. **New location** maps the roots beneath a folder you select. **Use this PC’s folders** maps matching Rainmeter roots. **Change destination** sets the path for a selected root, including portable setups on a different computer.
- Verify the preview. RainBackup requires an installed or detected portable Rainmeter. It closes running Rainmeter instances before writing and relaunches them afterward. A relocated restore updates the restored Rainmeter.ini when both settings and skins are mapped. New files inherit destination permissions, and replacement uses Windows’ access-control preservation. Restores apply file by file. A failure reports how many files were restored before it stopped.

Interactive backups and restores show the current path and percent complete. **Cancel** discards an unfinished backup ZIP. During restore, files already written stay in place, and Rainmeter restarts if RainBackup closed it.

## Scheduling

**Schedule** sets a daily or Monday backup, a local time, a destination, and how many managed copies to keep. Windows Task Scheduler runs it under the signed-in account. A missed backup runs when Windows and the account become available. Several missed intervals produce one current snapshot. The oldest managed ZIPs are removed after a successful backup. Other ZIPs are left alone. The schedule and latest result are stored in the RainBackup folder under Local AppData.

Open **Log…** on the main window for dated backup and restore results. **Empty log** clears the history after confirmation.

## Build

Configure with `cmake -S . -B build -A x64`, then run `cmake --build build --config Release` and `ctest --test-dir build -C Release --output-on-failure`. To build the installer, create the `dist` folder and compile `installer/RainBackup.nsi` with NSIS.

RainBackup uses zlib and MiniZip, vendored from the local Rainmeter source tree. The Rainmeter application icon forms the base of the RainBackup icon. Third-party licenses are included with the installer.
