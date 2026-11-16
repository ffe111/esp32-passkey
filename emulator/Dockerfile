# Pico FIDO emulator: the real firmware logic compiled for Linux, speaking CTAPHID
# over TCP port 35962. Used to test passkey-cli without hardware.
FROM debian:bookworm

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake git ca-certificates pkg-config \
        libtss2-dev libpcsclite-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY firmware/pico-fido /build/pico-fido
COPY .git/modules/firmware/pico-fido /build/gitdata
RUN echo "gitdir: /build/gitdata" > /build/pico-fido/.git \
 && echo "gitdir: /build/gitdata/modules/pico-keys-sdk" > /build/pico-fido/pico-keys-sdk/.git \
 && git config --global --add safe.directory '*' \
 && sed -i 's/^\s*worktree\s*=.*$//' /build/gitdata/config /build/gitdata/modules/pico-keys-sdk/config \
 && cmake -S /build/pico-fido -B /build/out -DENABLE_EMULATION=1 \
 && cmake --build /build/out -j"$(nproc)"

WORKDIR /data
ENV PICOKEYS_EMULATION_DISABLE_CCID=1
EXPOSE 35962
CMD ["/build/out/pico_fido"]
