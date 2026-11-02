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
        kwin \
        libkscreen \
        mesa \
        ninja \
        opencv \
        yaml-cpp \
        psmisc \
        python \
        libarchive \
        nodejs \
        noto-fonts \
        onnxruntime-cpu \
        pkgconf \
        qt6-base \
        qt6-multimedia \
        qt6-scxml \
        qt6-wayland \
        qt6-websockets \
        socat \
        systemd-libs \
        dbus \
        sway \
        unzip \
        wayland \
        wayland-protocols \
        wayland-utils \
    && setcap -r /usr/bin/sway \
    && pacman -Scc --noconfirm

# Rootless runners cannot execute the distribution binary with file capabilities.
RUN setcap -r /usr/bin/kwin_wayland

# Arch's base image excludes i18n sources. Required locale regressions must
# generate their own comma-decimal locale rather than silently skip.
RUN sed -i 's@usr/share/i18n/\*@!usr/share/i18n/*@g' /etc/pacman.conf \
    && pacman -S --noconfirm glibc \
    && pacman -Scc --noconfirm

COPY CubismSdkForNative-5-r.5/ /opt/deskwaifu-deps/CubismSdkForNative-5-r.5/
COPY thirdParty/glew/ /opt/deskwaifu-deps/glew/

ENV LIBGL_ALWAYS_SOFTWARE=1 \
    GALLIUM_DRIVER=llvmpipe \
    QT_QPA_PLATFORM=wayland

# Shared package metadata, independent of the orchestration host/account.
RUN printf '\nPACKAGER="DeskWaifu reproducible build"\n' >> /etc/makepkg.conf

# PRoot is not in Arch's official repositories. Pin the upstream static binary;
# it permits dependency-only install roots inside the unprivileged CI container.
RUN curl --fail --location --retry 3 --proto '=https' --proto-redir '=https' \
      https://github.com/proot-me/proot/releases/download/v5.5.0/proot -o /usr/local/bin/proot \
    && echo 'db185300a9daabbce86aca7632d8c7fdc4249f6f153171954013b70c5f491ec0  /usr/local/bin/proot' | sha256sum -c - \
    && chmod 755 /usr/local/bin/proot
