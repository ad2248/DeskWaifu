# Arch Linux CI image for DeskWaifu.
#
# The ci target contains the proprietary Cubism SDK from the local build
# context. Publish it only to the private project registry.
#
# Build and publish:
#   podman build --target ci -t git.kals.top/kals/deskwaifu-ci:<tag> .
#   podman push git.kals.top/kals/deskwaifu-ci:<tag>

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
        mesa \
        ninja \
        nodejs \
        noto-fonts \
        pkgconf \
        qt6-base \
        qt6-wayland \
        socat \
        sway \
        unzip \
        wayland \
        wayland-protocols \
        wayland-utils \
    && setcap -r /usr/bin/sway \
    && pacman -Scc --noconfirm

COPY CubismSdkForNative-5-r.5/ /opt/waifuland-deps/CubismSdkForNative-5-r.5/
COPY thirdParty/glew/ /opt/waifuland-deps/glew/

ENV LIBGL_ALWAYS_SOFTWARE=1 \
    GALLIUM_DRIVER=llvmpipe \
    QT_QPA_PLATFORM=wayland
