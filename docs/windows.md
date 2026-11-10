# Windows and MSVC


From PowerShell at the repository root:

```powershell
.\tools\setup_windows.ps1
```

Setup creates `.venv`, installs `requirements.txt`, installs the pinned vcpkg
manifest (OpenSSL, SDL2 and PThreads4W), and updates `v64recomp.sln`.
It uses Visual Studio's CMake and vcpkg when available. Set `VCPKG_ROOT` or pass
`-VcpkgRoot` for another checkout; `-Python C:\path\to\python.exe` overrides Python.

Open **v64recomp.sln** at the project root. It contains:

- **Recomp**: shared recompiler tools, runtime, public headers and backends.
- **Games**: one project per registered game, with its own C11 sources,
  headers, Debug/Release entry points, profile and generation helpers.


**Source Files** and **Header Files** show repository code. Game projects also
show translated C and generated headers under **Generated Code** once generated;
reload the project to refresh these after a first build. CMake compiles the
same `main.c` entry point for Debug and Release. A project's **Build**,
**Rebuild** and **Clean** commands use `tools/build_msvc_project.ps1`.

The solution generator enables Recomp and every game whose profile's ROM file
exists, in both Debug and Release. After importing another ROM, refresh the
solution to enable that game for **Build Solution** and F5:

```powershell
.\.venv\Scripts\python.exe tools/import_rom.py --profile games/sm64-us/profile.toml --rom 'C:\ROMs\Super Mario 64.n64'
.\.venv\Scripts\python.exe tools/msvc_solution.py
```

All games remain listed; games without an imported ROM remain excluded from
solution builds. `--game <id>` places that game first and explicitly enables it.
Set the desired game as the startup project, choose **Debug|x64** or
**Release|x64**, and press **F5** to build and launch it. Reload the solution
when Visual Studio reports that the generated files have changed. Some existing
Debug entry points require diagnostic arguments; see the game's README.

**Build > Configuration Manager** shows which games are enabled. You can also
right-click a game and choose **Build**, or build it directly from PowerShell:

```powershell
.\tools\build_msvc_project.ps1 -Game goldeneye-us -Configuration Debug
```

Choose **Release|x64** or **Debug|x64**, or build from Developer PowerShell:

```powershell
msbuild .\v64recomp.sln /m:4 /p:Configuration=Release /p:Platform=x64
msbuild .\v64recomp.sln /m:4 /p:Configuration=Debug /p:Platform=x64
```

Each project/configuration has an isolated build tree under
`build/msvc/<game-or-recomp>/<release-or-debug>/`. For example:

```powershell
.\build\msvc\sm64-us\release\bin\Release\sm64-us.exe --frames 300 --mute
```

`-Configuration Debug -Build` selects Debug in setup. Running
`.\.venv\Scripts\python.exe tools/msvc_solution.py --game sm64-us` refreshes the
solution/project file lists and default game without reinstalling dependencies.
CMake remains the authority for source selection, compiler flags and linking.
The existing CMake presets also remain available for direct command-line builds.

## VS Code

After running setup and importing the game's ROM, open the repository folder in
VS Code and install the recommended **C/C++** extension (`ms-vscode.cpptools`).
In **Run and Debug**, select **Debug: <game> (Windows)** or
**Release: <game> (Windows)**, then press **F5** to build and debug, or
**Ctrl+F5** to build and run without debugging.
Use **Ctrl+Shift+B** to select a **Build Debug/Release: <game>** task.

The **Configure Debug/Release: <game>** and **Build Debug/Release: <game>** tasks
select the Windows MSVC helper automatically. It locates Visual Studio's CMake
and vcpkg, so a Developer PowerShell session is unnecessary. Configure tasks use
`tools/build_msvc_project.ps1 -Action Configure` without compiling. Windows
launches use the Visual Studio debugger (`cppvsdbg`) and the same
`build/msvc/<game>/<mode>/bin/Debug|Release/<game>.exe` outputs as the solution.
Build errors appear in the Problems panel through the MSVC problem matcher.

The **(Linux)** launch configurations use GDB and the existing Ninja presets.
Some Debug entry points require diagnostic arguments; edit the launch entry's
`args` according to the game's README. Generated Windows CPU sources are under
`build/msvc/<game>/debug/generated/<game>/` for source breakpoints.

ROMs stay private in ignored `local/`. Builds explicitly disable ROM embedding;
setup never downloads ROMs. Exclude `local/` when distributing source or binaries.
Downloads, generated CPU sources, libraries, saves and captures stay outside the
checked project files. Keep the deployed DLLs beside the executable and retain
PDBs for native crash investigation.
