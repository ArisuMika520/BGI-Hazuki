# BGI-Hazuki ♡

<div align="center">
  <img src="static/bg.png" width="400" alt="BGI-Hazuki Background" />
</div>

> A Windows toolkit for unpacking and patching resources used by BGI / Ethornell / BURIKO ARC20 games.

> Oze Hazuki is Cute!

**[中文文档 →](README.zh-CN.md)**

---

> ⚠️ **Disclaimer & Warning**  
> This project is provided for technical research, reverse-engineering study, and personal archival/modding use only.  
> All extracted game assets, scripts, images, audio, and related materials remain the intellectual property of their original developers and publishers.  
> Do not use this tool for commercial distribution, large-scale piracy, or any activity that infringes copyright. The authors assume no responsibility for misuse.

---

## 1. What Is Included

The project currently ships with five end-user binaries:

- `BGI_Hazuki_GUI.exe` - graphical front-end for dragging in `.arc` archives.
- `BGI_Hazuki_Core.dll` - native core library required by the GUI.
- `BGI_Hazuki_ImageTool.exe` - standalone PNG <-> CBG converter.
- `BGI_Hazuki_TextTool.exe` - standalone DSC text extractor / applier.
- `BGI_Hazuki_AssetTool.exe` - standalone asset probe / unpack helper.

There is also one optional legacy utility:

- `BGI_Unpacker.exe` - simple drag-and-drop Python-based ARC extractor kept for compatibility.

If you only want the GUI, the minimum runtime pair is:

- `BGI_Hazuki_GUI.exe`
- `BGI_Hazuki_Core.dll`

Both files must stay in the same folder.

---

## 2. Requirements

For normal use, you only need:

1. Windows
2. A game using BGI / Ethornell Engine


No extra runtime package is required beyond what is already present on a standard Windows install.

---

## 3. Where to Put the Files

The recommended setup is to place `BGI_Hazuki_GUI.exe` and `BGI_Hazuki_Core.dll` directly in the game root, next to the `.arc` files you want to inspect.

Example:

```text
HoukagoCinderella2\
├── data01000.arc
├── system.arc
├── sysprg.arc
├── ...
├── BGI_Hazuki_GUI.exe
└── BGI_Hazuki_Core.dll
```

Then double-click `BGI_Hazuki_GUI.exe`.

---

## 4. GUI Quick Start

1. Launch `BGI_Hazuki_GUI.exe`.
2. Drag one or more `.arc` files onto the window, or click **Select Files**.
3. Wait until the status changes to finished.
4. Click **Open Output Directory** to jump to the generated files.

By default, output goes to:

```text
unpack\<arc_name>\
```

inside the current working directory.

The GUI automatically performs these follow-up conversions after ARC extraction:

- `CBG image` -> `PNG`
- `DSC / compiled script` -> `.hazuki.txt`
- `bw  audio` -> `OGG`

Optional command-line mode is also available:

```powershell
BGI_Hazuki_GUI.exe system.arc sysprg.arc --auto-close
```

That is useful for scripted batch runs.

---

## 5. Output Rules

The output folder intentionally keeps only files that are useful for later editing or re-import work.

- Converted CBG originals are deleted after successful PNG export.
- Original DSC / compiled script files are kept, because text apply needs the original binary script as a template.
- Original `bw  ` audio files are kept, because repacking requires the original container header.

In other words:

- `PNG` is enough to rebuild `CBG` later.
- `.hazuki.txt` alone is not enough to rebuild a script without the original script file.
- `OGG` alone is not enough to rebuild a BGI audio file without the original `bw  ` source.

---

## 6. Standalone Tools

### `BGI_Hazuki_ImageTool.exe`

Drag in one or more PNG files, CBG files, or whole folders.

- `PNG` -> `CBG`
- `CBG` -> `PNG`

### `BGI_Hazuki_TextTool.exe`

Usage:

```text
BGI_Hazuki_TextTool extract <script> [output.hazuki.txt] [--decode-cp 932] [--encode-cp 932]
BGI_Hazuki_TextTool apply <script.hazuki.txt> [output_script] [--encode-cp 932]
```

Supports drag-and-drop auto mode:

- dropping a DSC script extracts text
- dropping a `.hazuki.txt` project applies it back

### `BGI_Hazuki_AssetTool.exe`

Usage:

```text
BGI_Hazuki_AssetTool probe <file-or-dir> [...]
BGI_Hazuki_AssetTool unpack <file-or-dir> [...] [--decode-cp 932] [--encode-cp 932]
```

Useful for already extracted loose files. It auto-detects:

- CBG image
- DSC script / raw compiled script
- BGI audio (`bw  `)

### `BGI_Unpacker.exe`

This is the older ARC-only extractor. It does not provide the newer GUI workflow or automatic post-processing, but it is still kept as a small compatibility tool.

---

## 7. Directory Overview

```text
BGI_Hazuki/
├── bin/                 Final native binaries
├── BGI_Unpacker/        Legacy Python ARC unpacker and built exe
├── include/             Public headers for the native core
├── resources/           Icon resource script
├── src/                 C++ source files
├── static/              Background and icon source assets
├── tools/               Build helper scripts
├── build.bat            Full build entry
├── clean.bat            Cleanup script
├── README.md            English README
└── README.zh-CN.md      Chinese README
```

---

## 8. Building from Source

Requirements:

- Visual Studio 2022 with Desktop development with C++
- Windows SDK
- Python environment with `PyInstaller` if you also want to build `BGI_Unpacker.exe`

Build everything:

```powershell
.\build.bat
```

Clean intermediate output while keeping final binaries:

```powershell
.\clean.bat
```

Current build output:

- `bin\BGI_Hazuki_GUI.exe`
- `bin\BGI_Hazuki_Core.dll`
- `bin\BGI_Hazuki_ImageTool.exe`
- `bin\BGI_Hazuki_TextTool.exe`
- `bin\BGI_Hazuki_AssetTool.exe`
- `BGI_Unpacker\dist\BGI_Unpacker.exe`

---

## 9. FAQ

**Q: The GUI exe closes immediately.**  
A: `BGI_Hazuki_Core.dll` is missing or not placed next to the exe.

**Q: Why are some original files still left in the unpack result?**  
A: Script and audio repack workflows still depend on their original binary sources.

**Q: Can the GUI repack ARC files back yet?**  
A: No. The current GUI is unpack-focused. Rebuild and edit workflows are handled by the standalone tools.

**Q: Which codepage should I use for scripts?**  
A: Default is `932` (Shift-JIS), which is correct for typical BGI titles unless your target game proves otherwise.

---

Enjoy your patching! ✿ 