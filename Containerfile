# Rust CI / Arch packaging image. No proprietary SDK or application sources.
FROM archlinux:base-devel AS ci
ARG PACMAN_MIRROR=https://mirrors.tuna.tsinghua.edu.cn/archlinux
RUN printf 'Server = %s/$repo/os/$arch\nServer = https://geo.mirror.pkgbuild.com/$repo/os/$arch\n' "$PACMAN_MIRROR" > /etc/pacman.d/mirrorlist \
    && pacman -Syu --noconfirm rust git python nodejs libarchive \
    && pacman -Scc --noconfirm
RUN printf '\nPACKAGER="DeskWaifu reproducible build"\n' >> /etc/makepkg.conf
ENV LANG=C.UTF-8
WORKDIR /workspace
