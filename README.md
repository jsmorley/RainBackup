# RainBackup

RainBackup is a Windows x64 utility for backing up Rainmeter skins, settings, layouts, custom plugins, and additional files you select.

Download the installer ZIP from [GitHub Releases](https://github.com/jsmorley/RainBackup/releases), extract it, and run `RainBackup-Setup.exe`. See the [user manual](manual/RainBackup-Manual.md) for installation and operation.

## Backup

- **Scan** looks for the installed Rainmeter and running portable copies. To add a portable copy that is closed, use **Add folder** and select its installation folder. RainBackup remembers the sources and checkbox selections.
- Expand the tree to individual files. **Add folder** and **Add file** can include anything you consider part of your setup, such as your plugin SDK. Select a source to use **Change path**, **Remove folder**, or **Remove file**. Uncheck individual items inside a source to exclude them from backups.
- Use **New** beside the scheme name to save a name, destination, and optional schedule. Each scheme has its own sources and exclusions. Select one on the main window, use **Edit** to change its settings, and click **Back up now** to create a timestamped ZIP.

## Restore

- The Restore dropdown lists ZIPs for the selected backup scheme. **Open ZIP** can browse for other archives. Check individual items or click **Select all**.
- **Delete ZIP** permanently deletes the selected ZIP after showing its full path and asking for confirmation. With no ZIP selected, it opens a file picker.
- **Original location** uses the paths stored in the archive. **New location** puts the selected files under one folder you choose. Review the resulting paths before restoring.
- Verify the preview. RainBackup requires an installed or detected portable Rainmeter. It closes running Rainmeter instances before writing and relaunches them afterward. A relocated restore updates the restored Rainmeter.ini when both settings and skins are mapped. New files inherit destination permissions, and replacement uses Windows’ access-control preservation. Restores apply file by file. A failure reports how many files were restored before it stopped.

Interactive backups and restores show the current path and percent complete. **Cancel** discards an unfinished backup ZIP. During restore, files already written stay in place, and Rainmeter restarts if RainBackup closed it.

## Scheduling

Use **Edit** to choose manual only, daily, or Monday. Automatic backups keep a configurable number of copies per scheme and run under the signed-in account. RainBackup imports previous schedules as backup schemes.

Open **More options → Log** for dated backup and restore results. **Empty log** clears the history after confirmation.

## Command line

Run `RainBackup.exe "Scheme name" Backup` to back up a saved scheme, or `RainBackup.exe "Scheme name" Restore` to restore its newest ZIP to the original locations. Both commands run silently. See the [command-line manual](manual/RainBackup-Manual.md#command-line) for exit codes and restore behavior.

## Build

Configure with `cmake -S . -B build -A x64`, then run `cmake --build build --config Release` and `ctest --test-dir build -C Release --output-on-failure`. To build the installer, create the `dist` folder and compile `installer/RainBackup.nsi` with NSIS.

RainBackup uses zlib and MiniZip, vendored from the local Rainmeter source tree. The Rainmeter application icon forms the base of the RainBackup icon. Third-party licenses are included with the installer.
