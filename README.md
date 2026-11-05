# DiscusSlicer

DiscusSlicer is a fork of OrcaSlicer for a farm of Bambu Lab printers that are run by [Spoolside](https://spoolside.shelbyklein.com).
It exists because the closed-source Bambu network plug-in (`libbambu_networking`) crashes OrcaSlicer: a use-after-free in its
MQTT client (`mqtt::async_client::remove_token` / `mqtt::token::on_success` on `MQTTAsync_receiveThread`) during connect and reconnect.

## What differs from upstream

Everything is behind one CMake option, `DISCUS_SLICER` (ON by default; `-DDISCUS_SLICER=OFF` builds stock OrcaSlicer),
which defines the `DISCUS_SLICER` macro. Search for `DISCUS_SLICER` to find every change.

- **The Bambu network plug-in is never loaded.** `BBLNetworkPlugin::initialize()` and `get_source_module()` return before
  `dlopen`, so neither `libbambu_networking` nor `BambuSource` is ever opened. `installed_networking` is forced off in
  `AppConfig`, and the plug-in download, install and update prompts and the Preferences plug-in settings are disabled.
  OrcaSlicer then runs exactly as it does for users who never installed the plug-in.
- **No Device tab for Bambu printers.** `PresetBundle::use_bbl_device_tab()` is false, so Bambu printers get the web view
  that other printers use, pointed at Spoolside's printers page. No camera, MQTT, cloud login or direct printer sending.
- **Print / Send adds the plate, and its models, to Spoolside.** The Spoolside library is a curated set of finished
  prints, and Spoolside decides which printer runs them. For Bambu printers the print button reads "Add to library"
  (tooltip: "Add to Spoolside library"); Print all, Send and Send all are hidden, so only the current plate can be added.
  The dialog (`src/slic3r/GUI/SpoolsideUpload.cpp`, `Plater::send_to_spoolside`) asks for:
  - the **library name** (default: the project name, plus the plate name if the plate has one; the first object's name
    for an untitled project) and the **library** (PlayCase or Personal, default from Preferences);
  - for **each object on the plate**: *Update existing asset* (a list of Spoolside's assets from `GET /api/assets`,
    phones left out, preselected when the asset's name or its source file name matches the object's name), *Add as new
    asset* (name, Case/Faceplate/Sleeve/Part, Gen 3/4), or *Don't upload*. Objects with no matching asset default to
    *Don't upload*.

  Then, in order: each chosen object goes up as a binary STL to `POST /api/assets` (`x-asset` header: URL-encoded JSON).
  An update re-sends the asset's own fields and `source`, so Spoolside replaces that asset's STL and keeps its name,
  status and notes. A new asset gets the source `discusslicer/<name>.stl`, so adding the same name again updates it
  instead of making a duplicate (a source deleted in Spoolside is refused, as for the Dropbox import). Next the plate's
  `.gcode.3mf` (the same file as "Export plate sliced file") goes to `POST /api/library`, named `<name>.gcode.3mf`;
  a name that already exists in that library updates the entry. Finally `PATCH /api/library/:id {plate, assetIds}`
  links the plate to those assets (with their new STL hashes). If no object is uploaded, the entry's existing links
  are left alone. A confirmation lists the entry and each asset added or updated, with an "Open in Spoolside" button.

  **STL coordinates.** The STL is the object as designed, not as placed: only model parts are merged (no modifiers,
  negative volumes or support blockers), and the plate position, rotation and scale of the instance are left out.
  Each part loaded from a file goes back to where it sat in that file (`ModelVolume::source.mesh_offset`, the centre
  Orca removed on import, which 3MF projects keep), with its own scale and rotation inside the object. So an updated
  asset keeps the origin of its original STL, and Spoolside assemblies, which place parts by STL coordinates, stay
  aligned. Parts not loaded from a file (shapes, text) keep their place in the object.

  **Dropbox import caveat.** Assets imported from Dropbox have their Dropbox path as source (e.g.
  `Cases/12/iPhone 12 Phone Case.stl`). Spoolside's `scripts/import-assets.mjs` is run by hand, not on a schedule, but
  each run re-posts every Dropbox STL with its path as source and so replaces the STL of that asset, including one
  updated from DiscusSlicer. The dialog says so under such an asset. To avoid it, either save the new STL into Dropbox
  as well, or Spoolside's import would need to skip assets whose STL was last replaced by something other than the
  import (for example by recording the hash it last imported and skipping when the current hash differs).
  Server URL, default library and API key are under Preferences > Online > Spoolside. The key is kept in
  the macOS Keychain (service `DiscusSlicer/Spoolside`). Keys are made by a script inside Spoolside's container on the Beelink:
  ```sh
  ssh beelink 'cd /srv/projects/spoolside/deploy && docker compose -f compose.yaml -f tunnel-compose.yaml exec -T app node server/apikey.mjs create discusslicer'
  ```
  Use `revoke <name>` or `list` in place of `create <name>` to revoke or list keys. DiscusSlicer turns off curl's verbose
  output, so request headers (and the key) never reach stderr or the logs.
- **No OrcaSlicer prompts.** No "new version of OrcaSlicer" prompt at launch, no network plug-in page in the setup guide,
  and no `orcaslicer://` URL scheme (those links keep going to stock OrcaSlicer).
- **Separate app.** Bundle id `com.shelbyklein.DiscusSlicer`, bundle name DiscusSlicer, and data folder
  `~/Library/Application Support/DiscusSlicer`, so it never reads or changes stock OrcaSlicer's config.
  `SLIC3R_APP_KEY` stays `OrcaSlicer` on purpose: 3MF metadata, G-code headers and translations depend on it.
- **Black and grey instead of Orca teal.** Orca's accent family (#009688, hover #26A69A, dark #00675B, tints #BFE1DE /
  #E5F0EE, ...) is remapped in one table, `gDiscusAccent` in `src/slic3r/GUI/Widgets/StateColor.cpp`: near-black #1A1A1A
  (hover #333333, pressed #000000, tints #DEDEDE / #EDEDED) in light mode, mid grey #8A8A90 (hover #9E9EA4, tints #4A4A50 /
  #3C3C42) in dark mode so white button text stays readable. Every `StateColor` and `darkModeColorFor()` goes through it;
  ImGui overlays use `ImGuiWrapper::COL_ORCA` (now mode-dependent), SVG icons are recoloured on load (`BitmapCache`,
  `GLTexture`), and the few direct colour literals in visible UI are wrapped in `DISCUS_ACCENT()`. The web pages in
  `resources/web` are recoloured by `tools/discus_recolor_web.py` (re-run it after rebasing). Bambu device/monitor pages,
  which DiscusSlicer never shows, keep their literals.
- **Own logo.** Fish app icon (`resources/DiscusSlicer.icns`), splash and About banner (`resources/images/DiscusSlicer_*.png`),
  and the web homepage logo (`resources/web/image/logo.png`).
- **One-time preset import.** On first launch it offers to copy the user presets of `~/Library/Application Support/OrcaSlicer/user`
  into its own `user/default` folder (every account folder is merged, newest file wins). OrcaSlicer's files are only read.

## Building (macOS, Apple Silicon)

```sh
brew install cmake ninja automake autoconf libtool texinfo gettext
export SDKROOT=$(xcrun --sdk macosx --show-sdk-path)   # GMP/MPFR configure fails without it
./build_release_macos.sh -d -a arm64 -x                # dependencies, once
./build_release_macos.sh -s -a arm64 -x                # the app: build/arm64/OrcaSlicer/OrcaSlicer.app
ditto build/arm64/OrcaSlicer/OrcaSlicer.app /Applications/DiscusSlicer.app
codesign --force --deep --sign - /Applications/DiscusSlicer.app
```

## Rebasing onto a new OrcaSlicer release

The `discus` branch is upstream's release tag plus a few small commits.

```sh
git fetch upstream --tags
git rebase --onto vX.Y.Z v2.4.2 discus   # replace v2.4.2 with the tag the branch is currently based on
```

Conflicts are usually in the guarded blocks of `GUI_App.cpp`, `Plater.cpp`, `Preferences.cpp` and `BBLNetworkPlugin.cpp`.
After rebasing, check that nothing new calls `dlopen`/`LoadLibrary` for the plug-in (`grep -rn "dlopen\|initialize_network_module" src`)
and that new BBL print or send entry points in `Plater.cpp` route to `send_to_spoolside`. Then build and confirm with
`vmmap <pid> | grep -i bambu` that no plug-in library is mapped.

---

<div align="center">

<picture>
  <img alt="OrcaSlicer logo" src="resources/images/OrcaSlicer.png" width="15%" height="15%">
</picture>

<a href="https://trendshift.io/repositories/15552" target="_blank"><img src="https://trendshift.io/api/badge/repositories/15552" alt="OrcaSlicer%2FOrcaSlicer | Trendshift" style="width: 250px; height: 55px;" width="250" height="55"/></a>

[![GitHub Repo stars](https://img.shields.io/github/stars/OrcaSlicer/OrcaSlicer)](https://github.com/OrcaSlicer/OrcaSlicer/stargazers) [![Build all](https://github.com/OrcaSlicer/OrcaSlicer/actions/workflows/build_all.yml/badge.svg?branch=main)](https://github.com/OrcaSlicer/OrcaSlicer/actions/workflows/build_all.yml)

OrcaSlicer: an open source Next-Gen Slicing Software for Precision 3D Prints.  
Optimize your prints with ultra-fast slicing, intelligent support generation, and seamless printer compatibility—engineered for perfection.
<h3>

# Official links and community

#### Official Website:

<a href="https://www.orcaslicer.com/" style="font-size:2em;">OrcaSlicer.com</a>

#### Github Repository:

<a href="https://github.com/OrcaSlicer/OrcaSlicer"><img src="https://img.shields.io/badge/OrcaSlicer-181717?style=flat&logo=github&logoColor=white" width="200" alt="GitHub Logo"/> </a>

#### Follow us:

<a href="https://twitter.com/real_OrcaSlicer"><img src="https://img.shields.io/badge/real__OrcaSlicer-000000?style=flat&logo=x&logoColor=white" width="200" alt="X Logo"/> </a>  
<a href="https://www.youtube.com/@OfficialOrcaSlicer"><img src="https://img.shields.io/badge/OfficialOrcaSlicer-FF0000?style=flat&logo=youtube&logoColor=white" width="200" alt="YouTube Logo"/> </a>

#### Join our Discord community:

<a href="https://discord.gg/P4VE9UY9gJ"><img src="https://img.shields.io/badge/-Discord-5865F2?style=flat&logo=discord&logoColor=fff" width="200" alt="discord logo"/> </a>

<table border="2" style="border-color: #ffa500; background-color:rgb(232, 220, 180); color: #856404;">
<tr>
<td>
<strong>⚠️ CAUTION:</strong><br>
Several clickbait and malicious websites, such as <b>orca-slicer[.]com</b> and <b>orcaslicer[.]net</b>, are pretending to be the official OrcaSlicer site. These sites may redirect you to dangerous downloads or contain misleading information.<br>
<b>Our only official website is <a href="https://www.orcaslicer.com/">www.orcaslicer.com</a>.</b><br><br>
If you come across any of these in search results, please <b>report them</b> as unsafe or phishing to help keep the community secure with:<br>
 - <a href="https://safebrowsing.google.com/safebrowsing/report_phish/">Google Safe Browsing</a><br>
 - <a href="https://www.microsoft.com/en-us/wdsi/support/report-unsafe-site">Microsoft Security Intelligence</a><br>
 - <a href="https://ipthreat.net/tools/reportphishing">IPThreat</a>
</td>
</tr>
</table>

</div>

# Main features

- **[Advanced Calibration Tools](https://www.orcaslicer.com/wiki/calibration_guide)**  
  Comprehensive suite: temperature towers, flow rate, retraction & more for optimal performance.
- **[Precise Wall](https://www.orcaslicer.com/wiki/quality_settings_precision#precise-wall) and [Seam Control](https://www.orcaslicer.com/wiki/quality_settings_seam)**  
  Adjust outer wall spacing and apply scarf seams to enhance print accuracy.
- **[Sandwich Mode](https://www.orcaslicer.com/wiki/quality_settings_wall_and_surfaces#innerouterinner) and [Polyholes](https://www.orcaslicer.com/wiki/quality_settings_precision#polyholes) Support**  
  Use varied infill [patterns](https://www.orcaslicer.com/wiki/strength_settings_patterns) and accurate hole shapes for improved clarity.
- **[Overhang](https://www.orcaslicer.com/wiki/quality_settings_overhangs) and [Support Optimization](https://www.orcaslicer.com/wiki#support-settings)**  
  Modify geometry for printable overhangs with precise support placement.
- **[Granular Controls and Customization](https://www.orcaslicer.com/wiki#process-settings)**  
  Fine-tune print speed, layer height, pressure, and temperature with precision.
- **Network Printer Support**  
  Seamless integration with Klipper, PrusaLink, and OctoPrint for remote control.
- **[Mouse Ear Brims](https://www.orcaslicer.com/wiki/others_settings_brim) & [Adaptive Bed Mesh](https://www.orcaslicer.com/wiki/printer_basic_information_adaptive_bed_mesh)**  
  Automatic brims and adaptive mesh calibration ensure consistent adhesion.
- **User-Friendly Interface**  
  Intuitive drag-and-drop design with pre-made profiles for popular printers.
- **[Open-Source](https://github.com/OrcaSlicer/OrcaSlicer) & [Community Driven](https://discord.gg/P4VE9UY9gJ)**  
  Regular updates fueled by continuous community contributions.
- **Wide Printer Compatibility**  
  Supports a broad range of printers: Bambu Lab, Prusa, Creality, Voron, and more.
- Additional features can be found in the [change notes](https://github.com/OrcaSlicer/OrcaSlicer/releases/).

# Wiki

The [wiki](https://www.orcaslicer.com/wiki) aims to provide a detailed explanation of the slicer settings, including how to maximize their use and how to calibrate and set up your printer.

- **[Access the wiki here](https://www.orcaslicer.com/wiki)**
- **[Contribute to the wiki](https://www.orcaslicer.com/wiki/how_to_wiki)**

# Download

## Stable Release

📥 **[Download the Latest Stable Release](https://github.com/OrcaSlicer/OrcaSlicer/releases/latest)**  
Visit our GitHub Releases page for the latest stable version of OrcaSlicer, recommended for most users.

## Nightly Builds

🌙 **[Download the Latest Nightly Build](https://github.com/OrcaSlicer/OrcaSlicer/releases/tag/nightly-builds)**  
Explore the latest developments in OrcaSlicer with our nightly builds. Feedback on these versions is highly appreciated.

# How to install

## Windows

Download the **Windows Installer exe**  for your preferred version from the [releases page](https://github.com/OrcaSlicer/OrcaSlicer/releases).

- *For convenience there is also a portable build available.*
    <details>
    <summary>Troubleshooting</summary>

  - *If you have troubles to run the build, you might need to install following runtimes:*
  - [MicrosoftEdgeWebView2RuntimeInstallerX64](https://github.com/OrcaSlicer/OrcaSlicer/releases/download/v1.0.10-sf2/MicrosoftEdgeWebView2RuntimeInstallerX64.exe)
    - [Details of this runtime](https://aka.ms/webview2)
    - [Alternative Download Link Hosted by Microsoft](https://go.microsoft.com/fwlink/p/?LinkId=2124703)
  - [vcredist2019_x64](https://github.com/OrcaSlicer/OrcaSlicer/releases/download/v1.0.10-sf2/vcredist2019_x64.exe)
    - [Alternative Download Link Hosted by Microsoft](https://aka.ms/vs/17/release/vc_redist.x64.exe)
    - This file may already be available on your computer if you've installed visual studio.  Check the following location: `%VCINSTALLDIR%Redist\MSVC\v142`
    </details>

Windows Package Manager

```shell
winget install --id=SoftFever.OrcaSlicer -e
```

## Mac

1. Download the DMG for your computer: `arm64` version for Apple Silicon and `x86_64` for Intel CPU.
2. Drag OrcaSlicer.app to Application folder.
3. *If you want to run a build from a PR, you also need to follow the instructions below:*

    <details>
    <summary>Quarantine</summary>

    - Option 1 (You only need to do this once. After that the app can be opened normally.):
      - Step 1: Hold _cmd_ and right click the app, from the context menu choose **Open**.
      - Step 2: A warning window will pop up, click _Open_

    - Option 2:
      Execute this command in terminal:

      ```shell
      xattr -dr com.apple.quarantine /Applications/OrcaSlicer.app
      ```

    - Option 3:
        - Step 1: open the app, a warning window will pop up  
            ![mac_cant_open](./SoftFever_doc/mac_cant_open.png)
        - Step 2: in `System Settings` -> `Privacy & Security`, click `Open Anyway`:  
            ![mac_security_setting](./SoftFever_doc/mac_security_setting.png)
    </details>

## Linux

### Flathub (Recommended)

OrcaSlicer is available through FlatHub:

<a href='https://flathub.org/apps/com.orcaslicer.OrcaSlicer'><img width='240' alt='Download on Flathub' src='https://dl.flathub.org/assets/badges/flathub-badge-en.png'/></a>

Install from the command line:

```shell
flatpak install flathub com.orcaslicer.OrcaSlicer
flatpak run com.orcaslicer.OrcaSlicer
```

It can also be installed through graphical software managers (KDE Discover, GNOME Software, etc.) when Flathub is enabled. Search for **OrcaSlicer** in your software center.

### AppImage

AppImages are published for both **x86_64** and **aarch64** (ARM64). Pick the file matching your CPU — the ARM64 build has `aarch64` in its name (e.g. `OrcaSlicer_Linux_AppImage_Ubuntu2404_aarch64_*.AppImage`).

 1. Download App image from the [releases page](https://github.com/OrcaSlicer/OrcaSlicer/releases).
 2. Double click the downloaded file to run it.

 3. If you run into trouble executing it, try this command in the terminal:
    `chmod +x /path_to_appimage/OrcaSlicer_Linux.AppImage`

# How to Compile

All updated build instructions for Windows, macOS, and Linux are now available on the official [OrcaSlicer Wiki - How to build](https://www.orcaslicer.com/wiki/how_to_build) page.

Please refer to the wiki to ensure you're following the latest and most accurate steps for your platform.

# Klipper Note

If you're running Klipper, it's recommended to add the following configuration to your `printer.cfg` file.

```gcode
# Enable object exclusion
[exclude_object]

# Enable arcs support
[gcode_arcs]
resolution: 0.1
```

# Supports

**OrcaSlicer** is an open-source project and I'm deeply grateful to all my sponsors and backers.  
Their generous support enables me to purchase filaments and other essential 3D printing materials for the project.  
Thank you! :)

## Sponsors

<table>
<tr>
<td>
<a href="https://qidi3d.com/" style="display:inline-block; border-radius:8px; background:#fff;">
  <img src="SoftFever_doc\sponsor_logos\QIDI.png" alt="QIDI" width="100" height="100">
</a>
</td>
<td>
<a href="https://bigtree-tech.com/" style="display:inline-block; border-radius:8px; background:#222;">
    <img src="SoftFever_doc\sponsor_logos\BigTreeTech.png" alt="BIGTREE TECH" width="100" height="100">
</a>
</td>
</tr>
</table>

## Backers:

**Ko-fi supporters** ☕: [Backers list](https://github.com/user-attachments/files/16147016/Supporters_638561417699952499.csv)

## Support me

<a href="https://github.com/sponsors/SoftFever"><img src="https://img.shields.io/badge/GitHub%20Sponsors-30363D?style=flat&logo=GitHub-Sponsors&logoColor=EA4AAA" height="50"></a>  
<a href="https://ko-fi.com/G2G5IP3CP"><img src="https://img.shields.io/badge/Support_me_on_Ko--fi-FF5E5B?style=flat&logo=ko-fi&logoColor=white" height="50"></a>  
<a href="https://paypal.me/softfever3d"><img src="https://img.shields.io/badge/PayPal-003087?style=flat&logo=paypal&logoColor=fff" height="50"></a>

## Some Background

Open-source slicing has always been built on a tradition of collaboration and attribution. [Slic3r](https://github.com/Slic3r/Slic3r), created by Alessandro Ranellucci and the RepRap community, laid the foundation. [PrusaSlicer](https://github.com/prusa3d/PrusaSlicer) by Prusa Research built on Slic3r and acknowledged that heritage. [Bambu Studio](https://github.com/bambulab/BambuStudio) in turn forked from PrusaSlicer, and [SuperSlicer](https://github.com/supermerill/SuperSlicer) by @supermerill extended PrusaSlicer with community-driven enhancements. Each project carried the work of its predecessors forward, crediting those who came before.

OrcaSlicer began in that same spirit, drawing from BambuStudio, PrusaSlicer, and ideas inspired by CuraSlicer and SuperSlicer. But it has since grown far beyond its origins. Through relentless innovation — introducing advanced calibration tools, precise wall and seam control, tree supports, adaptive slicing, and hundreds of other features — OrcaSlicer has become the most widely used and actively developed open-source slicer in the 3D printing community. Many of its innovations have been adopted by other slicers, making it a driving force for the entire industry.

The OrcaSlicer logo was designed by community member [Justin Levine](https://github.com/jal-co).

# License

- **OrcaSlicer** is licensed under the GNU Affero General Public License, version 3.
- The **GNU Affero General Public License**, version 3 ensures that if you use any part of this software in any way (even behind a web server), your software must be released under the same license.
- OrcaSlicer includes a **pressure advance calibration pattern test** adapted from Andrew Ellis' generator, which is licensed under GNU General Public License, version 3. Ellis' generator is itself adapted from a generator developed by Sineos for Marlin, which is licensed under GNU General Public License, version 3.
- The **Bambu networking plugin** is based on non-free libraries from BambuLab. It is optional to the OrcaSlicer and provides extended functionalities for Bambulab printer users.
