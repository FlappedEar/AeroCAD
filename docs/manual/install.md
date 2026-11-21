# Install and run

AeroCAD is built from source. There is no installer or prebuilt download yet. Supported systems are
**macOS on Apple Silicon** (the main platform) and **Linux** (Ubuntu 24.04, for development).
Windows is not supported at the moment.

## macOS (Apple Silicon)

You need macOS 13 or newer, the Xcode Command Line Tools (`xcode-select --install`) and
[Homebrew](https://brew.sh).

```sh
# 1. Tools and libraries
brew install cmake ninja qt opencascade nlohmann-json pugixml catch2

# 2. Get the code
git clone https://github.com/FlappedEar/AeroCAD.git
cd AeroCAD

# 3. Build
cmake --preset release -DCMAKE_PREFIX_PATH="$(brew --prefix qt)"
cmake --build --preset release

# 4. Run
open build/release/bin/AeroCAD.app
```

To update later: `git pull`, then repeat step 3 (it only rebuilds what changed).

To open a scan straight away, pass it as an argument:

```sh
build/release/bin/AeroCAD.app/Contents/MacOS/AeroCAD ~/Downloads/bumper.stl
```

More detail and troubleshooting: [macOS](../macos.md).

## Linux (Ubuntu 24.04)

```sh
sudo apt install ninja-build qt6-base-dev qt6-declarative-dev qt6-quick3d-dev qt6-shadertools-dev \
  libxkbcommon-dev qml6-module-qtquick qml6-module-quick3d qml6-module-qtquick-controls \
  qml6-module-qtquick-layouts qml6-module-qtquick-window qml6-module-qtquick-dialogs \
  qml6-module-qtquick-templates qml6-module-qtqml-workerscript qml6-module-qtcore libqt6svg6 \
  qml6-module-qt-labs-folderlistmodel \
  libocct-data-exchange-dev libocct-modeling-algorithms-dev libocct-foundation-dev \
  libocct-modeling-data-dev libocct-ocaf-dev libocct-visualization-dev libtbb-dev \
  fonts-dejavu-core nlohmann-json3-dev libpugixml-dev catch2
cmake --preset release
cmake --build --preset release
./build/release/bin/AeroCAD
```

A first build takes about 15 minutes on four cores.

## The wind tunnel engine (Docker)

The wind tunnel runs **OpenFOAM v2506** inside Docker. You need it only for step 6 of the
tutorial; everything before it works without it.

=== "Easiest: let the program do it"

    1. Open the toolbar tab **SIMULATE** and click **Wind Tunnel**.
    2. At the top of the panel, the engine status is red or orange when something is missing.
       Click **Install for me**. Each click does the next missing step: it installs `docker` and
       Colima with Homebrew, starts the virtual machine, and downloads the OpenFOAM image
       (about 380 MB). Click it again after each step until the status turns green.

=== "By hand (macOS)"

    ```sh
    brew install docker colima
    colima start --cpu 8 --memory 8 --vm-type vz      # adapt to your machine
    docker pull opencfd/openfoam-default:2506         # optional: saves waiting at the first run
    ```

    Colima's default virtual machine (2 CPUs, 2 GB) is too small for the medium and fine mesh, so give
    it more. After a restart of the Mac run `colima start ...` again (or press **Install for me**).

=== "Linux"

    Install Docker Engine and make sure `docker ps` works for your user. A local OpenFOAM v2506
    installation is used when Docker is not running.

Runs are stored in `~/.aerocad/cfd/`. Colima and Docker Desktop mount your home folder, so keep that
folder inside your home directory. Details: [Wind tunnel reference](../cfd.md#engine-openfoam-v2506-in-docker).

## Check that it works

Start the program. You should see the empty window with a **Getting started** card in the Properties
panel (Import, New sketch, Box). If a scan is not shown after loading, press ++f6++ (fit all).

Next: [The window and navigation](interface.md).
