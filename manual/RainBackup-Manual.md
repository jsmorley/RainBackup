# RainBackup User Manual

RainBackup is a Windows x64 utility for backing up and restoring Rainmeter skins, settings, layouts, custom plugins, and any additional files or folders you choose. A single ZIP can hold files from more than one Rainmeter setup.

## Install or upgrade RainBackup

1. Run **RainBackup-Setup.exe**. The default destination is **C:\Program Files\RainBackup**. Use **Browse** to choose a different folder.
2. Choose whether to **Create Start menu shortcut** for the installing account. Setup checks this option for a new installation and clears it when it detects an existing one. Leave it cleared during an upgrade if you’ve moved your shortcut and want to keep using it.
3. Review the installation path and shortcut choice, then confirm. On the final screen, leave **Launch RainBackup** checked to open the program when you click **Finish**.

The plain-text manual is installed as **RainBackup-Manual.txt** in the same folder as **RainBackup.exe**.

Setup closes idle RainBackup windows before replacing the program files. If a backup or restore is running, wait for it to finish, then click **Retry**.

## Create or edit a backup scheme

Click **New** beside the scheme name to enter a name, choose a destination, and set an optional schedule. Each scheme has its own sources and checkboxes. Use the name picker to switch schemes and **Edit** to change the selected scheme’s settings.

RainBackup imports existing schedules as separate backups with copies of the previous source selection. Existing ZIPs stay in place.

## Choose what to back up

1. Open **Backup** and click **Scan**. RainBackup looks for the installed copy and any running portable copies it can identify. It reads Rainmeter’s configured skins path and shows the skins separately from **Settings & Plugins**.
2. For a portable copy that isn’t running, click **Add folder** and select the installation folder containing **Rainmeter.exe**, **Rainmeter.dll**, and **Rainmeter.ini**. RainBackup adds its skins, settings, and plugins. Repeat for each portable copy.
3. Expand the tree to see individual folders and files. Check or uncheck a folder to apply the choice to everything under it. A dash in a checkbox means only some of its contents are selected. Use **Select all** or **Clear selection** to change the entire tree.
4. Use **Add folder** or **Add file** for anything else you want to include. For example, add **C:\rainmeter-plugin-sdk** if you want the SDK in the same archive.
5. To replace a configured path, select the source and click **Change path**. To take an entire source off the list, select its top-level folder or file and click **Remove folder** or **Remove file**. The Yes/No prompt shows exactly which path will be removed from future backups. Your files and existing ZIPs stay in place. To skip a nested folder or file, uncheck it in the tree.

RainBackup remembers your sources and checkbox choices. Review **Scan issues** if the scan reports unavailable paths or skipped links. Use **Scan** before a manual backup if you’ve added or removed files since the tree was loaded.

Selection changes save automatically. Click **Save** beside **Clear selection** to save the current selection again and confirm it in the status line.

## Command line

```powershell
$exe = 'C:\Program Files\RainBackup\RainBackup.exe'
$run = Start-Process -FilePath $exe -ArgumentList '"My scheme" Backup' -Wait -PassThru
$run.ExitCode
```

Use `Backup` or `Restore` after the name of a saved scheme. Quote names with spaces. Both commands run without a window, prompts, or console output. The exit code is `0` for success, `1` for an operation failure, or `2` for invalid arguments. Operation details appear under **More options → Open log**.

- **Backup** uses the scheme's saved sources, exclusions, destination, and retention settings.
- **Restore** restores all entries from the scheme's newest ZIP in its current destination to their original locations, overwriting existing files without prompting. RainBackup closes and restarts Rainmeter as needed.

## Create a backup ZIP

1. Select a backup scheme. Use **Edit** to view or change its destination.
2. Click **Back up now** and review the confirmation. RainBackup names the ZIP using the scheme name and local time, for example **RainBackup-Skins-2026-11-14-18-00-00.zip**, and shows the full path when it finishes.
3. Watch the progress bar and current path. Click **Cancel** to discard the unfinished ZIP. If the operation is canceled or fails before the new archive is saved, any existing ZIP at that path stays in place.

If the skins are in OneDrive, Windows may download selected online-only files as RainBackup reads them. If a file can’t be read, check OneDrive’s connection or use **Always keep on this device** in File Explorer, then try again.

## Restore a backup

1. Install Rainmeter on the destination PC before restoring. For a portable copy that RainBackup hasn’t detected, use **Backup** → **Add folder** and select its installation folder.
2. Select a backup scheme and open **Restore**. Choose one of its ZIPs from the dropdown. **Open ZIP** starts in the folder of the last ZIP you chose through that button. If that folder is unavailable or you haven't chosen a ZIP yet, the dialog opens at the **RainBackup** group of current scheme destinations when the group is available.
3. Expand the archive tree and check the items to restore. Click **Select all** to restore the entire backup.
4. Choose a destination. The list shows the backup type, original location, and restore destination for each source.
5. Click **Restore selected**. Review the destinations and the number of existing files that will be overwritten, then click **Yes**. RainBackup closes running Rainmeter instances before writing and reopens those instances afterward.

### Choose a restore destination

- **Original location** uses the exact paths stored in the backup. Use it when the same folders are still correct.
- **New location** puts the selected files under one folder you choose. RainBackup keeps separate backup sources in separate subfolders.

When a relocated restore includes **Rainmeter.ini** and a skins destination is mapped for the same setup, RainBackup updates **SkinPath** in the restored INI. New files inherit permissions from their destination folder. For overwrites, RainBackup attempts to preserve the existing file’s owner and access controls and stops if Windows prevents that.

**Cancel** stops the restore before the next file is committed. Files already restored stay in place, and RainBackup reports how many it completed. Rainmeter restarts if RainBackup closed it.

## Schedule backups and keep a set number of copies

1. Select a backup scheme and click **Edit**.
2. Set **Schedule** to **Manual only**, **Daily**, or **Weekly on Monday**. For automatic backups, enter a local time such as **18:00** and set **Copies to keep** to a number from 1 to 999.
3. Click **Save scheme** and confirm.

Windows Task Scheduler runs automatic backups under your signed-in account and catches up with one current backup after missed runs. Retention applies to both automatic and on-demand runs of that scheme. Manual-only schemes keep all copies.

Switch to **Manual only** to stop automatic runs. In **Edit**, choose **Remove scheme** to remove the scheme and its schedule. If matching ZIPs are found in its current destination, check **Remove all .zip files as well** to delete them permanently. The box starts unchecked. You can open any ZIPs you keep with **Open ZIP**.

## View or empty the operation log

Open **More options → Open log** to see manual backups, scheduled backups, and restores. Each entry shows when the operation ran and whether it succeeded, failed, or was canceled. Click **Empty log** and confirm to clear the history. Backup ZIPs and settings stay in place.

When RainBackup starts, the bottom-left status area shows the date and result of the last scheduled backup in the log.

The log file is stored at `%LOCALAPPDATA%\RainBackup\operations.log`. Paste that path into File Explorer’s address bar to open it directly.

To turn button tooltips on or off, open **More options → Settings → Show button tooltips**.

Use **More options → Settings → Dark Mode** to switch between light and dark. RainBackup follows the Windows app theme until you choose a mode.

Choose **12-hour time** or **24-hour time** under **More options → Settings**. This applies to the schedule editor, the Restore dropdown for a backup scheme, and the log view. ZIP filenames use 24-hour time.

## Delete an old backup

On **Restore**, select a ZIP in the dropdown and click **Delete ZIP**. If no ZIP is selected, the button opens a file picker. Verify the full path in the Yes/No prompt. Clicking **Yes** permanently deletes that ZIP.
