# Rust CI / Arch packaging image. No proprietary SDK or application sources.
FROM archlinux:base-devel AS toolchain
ARG PACMAN_MIRROR=https://mirrors.tuna.tsinghua.edu.cn/archlinux
RUN printf 'Server = %s/$repo/os/$arch\nServer = https://geo.mirror.pkgbuild.com/$repo/os/$arch\n' "$PACMAN_MIRROR" > /etc/pacman.d/mirrorlist \
    && pacman -Syu --noconfirm rust git python nodejs libarchive libpipewire clang wl-clipboard sccache \
    && pacman -Scc --noconfirm
RUN printf '\nPACKAGER="DeskWaifu reproducible build"\n' >> /etc/makepkg.conf
ENV LANG=C.UTF-8
WORKDIR /workspace

FROM toolchain AS cargo-dependencies
ARG CARGO_HTTPS_PROXY
COPY Cargo.lock /tmp/workspace-Cargo.lock
RUN --mount=type=bind,source=.,target=/source \
    mkdir -p /opt/deskwaifu-cargo && \
    cd /source && cmp Cargo.lock /tmp/workspace-Cargo.lock && \
    HTTPS_PROXY="$CARGO_HTTPS_PROXY" cargo vendor --locked /opt/deskwaifu-cargo/vendor > /opt/deskwaifu-cargo/config.toml

FROM toolchain AS ci
COPY --from=cargo-dependencies /opt/deskwaifu-cargo /opt/deskwaifu-cargo
