FROM mcr.microsoft.com/devcontainers/base:ubuntu-22.04

# Linux SDK build dependencies.
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    mount util-linux bash-completion vim sudo locales tzdata time rsync bc \
    python3 python3-pip whiptail python3-requests \
    build-essential crossbuild-essential-arm64 ccache \
    git git-lfs ssh make gcc g++ ruby \
    libssl-dev liblz4-tool expect patchelf chrpath gawk texinfo diffstat \
    binfmt-support qemu-user-static bison flex fakeroot cmake \
    unzip device-tree-compiler ncurses-dev net-tools u-boot-tools dpkg-dev \
    libgmp-dev libmpc-dev binutils libelf-dev curl pv \
    devscripts equivs software-properties-common \
    gcc-aarch64-linux-gnu g++-aarch64-linux-gnu \
    debootstrap cpio iputils-ping pigz tar aria2 \
    libncurses5 zip less e2fsprogs ffmpeg \
    && rm -rf /var/lib/apt/lists/*

# pyelftools: required by Rockchip boot/firmware packing scripts
RUN pip3 install pyelftools
