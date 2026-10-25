# DeskWaifu 使用的 Arch Linux CI 镜像。
#
# ci 目标包含本地构建上下文中的专有 Cubism SDK。
# 必须保持私有：公开所有者名下的 Gitea 包也会公开。
#
# 构建并私下传输到运行器：
#   bash scripts/rebuild-ci-image.sh

FROM archlinux:base-devel AS ci

ARG PACMAN_MIRROR=https://mirrors.tuna.tsinghua.edu.cn/archlinux
RUN { printf 'Server = %s/$repo/os/$arch\n' "${PACMAN_MIRROR}"; \
      printf 'Server = https://mirrors.ustc.edu.cn/archlinux/$repo/os/$arch\n'; \
      printf 'Server = https://geo.mirror.pkgbuild.com/$repo/os/$arch\n'; \
    } > /etc/pacman.d/mirrorlist \
    && sed -i 's/^#ParallelDownloads.*/ParallelDownloads = 5/' /etc/pacman.conf \
    && pacman -Syu --noconfirm \
        cmake \
        curl \
        git \
        glu \
        libglvnd \
        layer-shell-qt \
        mesa \
        ninja \
        nodejs \
        noto-fonts \
        pkgconf \
        qt6-base \
        qt6-multimedia \
        qt6-scxml \
        qt6-wayland \
        qt6-websockets \
        socat \
        systemd-libs \
        sway \
        unzip \
        wayland \
        wayland-protocols \
        wayland-utils \
    && setcap -r /usr/bin/sway \
    && pacman -Scc --noconfirm

COPY CubismSdkForNative-5-r.5/ /opt/deskwaifu-deps/CubismSdkForNative-5-r.5/
COPY thirdParty/glew/ /opt/deskwaifu-deps/glew/

ENV LIBGL_ALWAYS_SOFTWARE=1 \
    GALLIUM_DRIVER=llvmpipe \
    QT_QPA_PLATFORM=wayland
