#!/bin/bash

# --- Configuration ---
PRIMARY_REPO="https://git.sfymmik.xyz/SfymmiK/TermmiK"
FALLBACK_REPO="https://github.com/SFYMMIK/TermmiK"
CLONE_DIR="TermmiK"
BINARY_NAME="TermmiK"

# --- Step 1: Repository selection ---
echo -e "\e[1;34m[1/6] Checking repository availability...\e[0m"
if git ls-remote "$PRIMARY_REPO" &> /dev/null; then
    REPO_URL="$PRIMARY_REPO"
    echo "Primary repository is reachable: $REPO_URL"
else
    REPO_URL="$FALLBACK_REPO"
    echo -e "\e[1;33mPrimary repository unreachable. Falling back to: $REPO_URL\e[0m"
fi

# --- Step 2: Detecting package manager and installing dependencies ---
echo -e "\n\e[1;34m[2/6] Checking and installing dependencies...\e[0m"

if command -v pacman &> /dev/null; then
    echo "Detected Arch Linux / pacman."
    DEPS=(base-devel git wayland libx11 libxkbcommon fontconfig freetype2 pkgconf)
    sudo pacman -S --needed "${DEPS[@]}" || { echo -e "\e[1;31mDependency installation failed.\e[0m"; exit 1; }

elif command -v apt-get &> /dev/null; then
    echo "Detected Debian/Ubuntu / apt."
    DEPS=(build-essential git libwayland-dev libx11-dev libxkbcommon-dev libfontconfig1-dev libfreetype6-dev pkg-config)
    sudo apt-get update
    sudo apt-get install -y "${DEPS[@]}" || { echo -e "\e[1;31mDependency installation failed.\e[0m"; exit 1; }

elif command -v xbps-install &> /dev/null; then
    echo "Detected Void Linux / xbps."
    echo -e "\e[1;33m[!] WARNING for Void Linux users: If dependency installation fails,\e[0m"
    echo -e "\e[1;33m    please abort and run 'sudo xbps-install -Su' first to update your system.\e[0m"
    DEPS=(base-devel git wayland-devel libX11-devel libxkbcommon-devel fontconfig-devel freetype-devel pkg-config)
    sudo xbps-install -Sy "${DEPS[@]}" || { echo -e "\e[1;31mDependency installation failed.\e[0m"; exit 1; }

elif command -v eopkg &> /dev/null; then
    echo "Detected Solus Linux / eopkg."
    echo "Installing build essentials (system.devel)..."
    sudo eopkg install -c system.devel -y || { echo -e "\e[1;31mFailed to install build essentials.\e[0m"; exit 1; }
    DEPS=(git wayland-devel libx11-devel libxkbcommon-devel fontconfig-devel freetype2-devel pkgconfig)
    sudo eopkg install -y "${DEPS[@]}" || { echo -e "\e[1;31mDependency installation failed.\e[0m"; exit 1; }

elif command -v emerge &> /dev/null; then
    echo "Detected Gentoo / emerge."
    DEPS=(dev-vcs/git dev-libs/wayland x11-libs/libX11 x11-libs/libxkbcommon media-libs/fontconfig media-libs/freetype dev-util/pkgconf)

    echo "How do you want to install dependencies via emerge?"
    echo "1) Compile from source (Default)"
    echo "2) Use precompiled binaries (--getbinpkg)"
    read -r -p "Choose an option [1/2] (Default: 1): " EMERGE_CHOICE

    EMERGE_FLAGS="--ask=n"
    if [[ "$EMERGE_CHOICE" == "2" ]]; then
        EMERGE_FLAGS="$EMERGE_FLAGS --getbinpkg"
    fi

    sudo emerge $EMERGE_FLAGS "${DEPS[@]}" || { echo -e "\e[1;31mDependency installation failed.\e[0m"; exit 1; }

else
    echo -e "\e[1;33mWarning: Package manager not recognized. Manually ensure you have a compiler, make, git, and X11/Wayland libraries.\e[0m"
fi

# --- Step 3: Downloading / updating the repository ---
echo -e "\n\e[1;34m[3/6] Downloading TermmiK code...\e[0m"
if [ -d "$CLONE_DIR" ]; then
    echo "Directory $CLONE_DIR already exists. Updating code (git pull)..."
    cd "$CLONE_DIR" || exit

    # Ensure we are pulling from the currently selected active repo URL
    git remote set-url origin "$REPO_URL"
    git pull
else
    git clone "$REPO_URL"
    cd "$CLONE_DIR" || exit
fi

# --- Step 4: Environment selection (Wayland / X11) ---
echo -e "\n\e[1;34m[4/6] Build configuration\e[0m"
echo "For which environment do you want to compile the terminal?"
echo "1) Wayland only (DISABLE_X11=1)"
echo "2) X11 only (DISABLE_WAYLAND=1)"
echo "3) Wayland and X11 (Default)"
read -r -p "Choose an option [1/2/3] (Default: 3): " ENV_CHOICE

MAKE_FLAGS=""
if [[ "$ENV_CHOICE" == "1" ]]; then
    MAKE_FLAGS="DISABLE_X11=1"
elif [[ "$ENV_CHOICE" == "2" ]]; then
    MAKE_FLAGS="DISABLE_WAYLAND=1"
else
    MAKE_FLAGS=""
fi

# --- Step 5: Thread count selection and Compilation ---
MAX_THREADS=$(nproc)
echo -e "\nHow many CPU threads do you want to use for compilation?"
echo "Your CPU supports from 1 to $MAX_THREADS threads."
read -r -p "Enter a number (Press Enter to use the default maximum: $MAX_THREADS): " THREAD_CHOICE

# Thread input validation
if [[ -z "$THREAD_CHOICE" ]]; then
    THREADS=$MAX_THREADS
elif [[ "$THREAD_CHOICE" =~ ^[0-9]+$ ]] && [[ "$THREAD_CHOICE" -ge 1 ]] && [[ "$THREAD_CHOICE" -le "$MAX_THREADS" ]]; then
    THREADS=$THREAD_CHOICE
else
    echo -e "\e[1;33mInvalid value. Forcing the use of maximum threads: $MAX_THREADS\e[0m"
    THREADS=$MAX_THREADS
fi

echo -e "\n\e[1;34m[5/6] Compiling TermmiK...\e[0m"
echo "Running: make $MAKE_FLAGS -j$THREADS"

# Clean previous build to avoid conflicts when changing flags
make clean &> /dev/null

# Capture standard error (stderr) into a log file
if make $MAKE_FLAGS -j"$THREADS" 2> build_error.log; then
    echo -e "\n\e[1;32mCompilation successful!\e[0m"

    # --- Step 6: Installation and Cleanup ---
    echo -e "\n\e[1;34m[6/6] Installing and cleaning up...\e[0m"

    if [ -f "./$BINARY_NAME" ]; then
        echo "Installing $BINARY_NAME to /usr/local/bin (requires root)..."
        if sudo cp "./$BINARY_NAME" /usr/local/bin/; then
            echo -e "\e[1;32mSuccessfully installed to /usr/local/bin/$BINARY_NAME\e[0m"
        else
            echo -e "\e[1;31mFailed to install binary. Do you have sudo privileges?\e[0m"
            exit 1
        fi

        echo "Cleaning up build directory..."
        cd .. || exit
        rm -rf "$CLONE_DIR"
        echo "Source files removed."

        echo "Starting the terminal..."
        # Run the globally installed binary
        if command -v "$BINARY_NAME" &> /dev/null; then
            "$BINARY_NAME"
        else
            echo -e "\e[1;33mCould not find the global binary. Try running '$BINARY_NAME' manually.\e[0m"
        fi

    else
        echo -e "\e[1;33mCould not find the $BINARY_NAME file. Make sure the compilation generated it.\e[0m"
    fi

else
    echo -e "\n\e[1;31m================ BUILD ERROR ================\e[0m"
    cat build_error.log
    echo -e "\e[1;31m=============================================\e[0m"
    exit 1
fi
