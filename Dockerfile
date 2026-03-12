# -----------------------------------------------------------------------
# Stage 1 – install XC8 into a temporary builder layer
# -----------------------------------------------------------------------
FROM ubuntu:22.04 AS builder

ARG XC8_VERSION=3.10

ARG DFP_VERSION=1.8.254

# 32-bit compatibility libraries required by the XC8 installer binary
RUN dpkg --add-architecture i386 && \
    apt-get update && \
    apt-get install -y --no-install-recommends \
        ca-certificates \
        wget \
        unzip \
        libc6:i386 \
        libncurses5:i386 \
        libstdc++6:i386 && \
    rm -rf /var/lib/apt/lists/*

# Download and install XC8 directly from Microchip
# Note: --netservername is required by the unattended installer even for free/eval mode
RUN wget -q -O /tmp/xc8-installer.run \
        "https://ww1.microchip.com/downloads/aemDocuments/documents/DEV/ProductDocuments/SoftwareTools/xc8-v${XC8_VERSION}-full-install-linux-x64-installer.run" && \
    chmod +x /tmp/xc8-installer.run && \
    /tmp/xc8-installer.run --mode unattended \
        --unattendedmodeui none \
        --netservername localhost \
        --LicenseType FreeMode \
        --prefix /opt/microchip/xc8 && \
    rm -f /tmp/xc8-installer.run

# Download and extract the PIC12-16F1xxx Device Family Pack (required by XC8 v3.x for PIC16F1829)
RUN wget -q -O /tmp/dfp.atpack \
        "https://packs.download.microchip.com/Microchip.PIC12-16F1xxx_DFP.${DFP_VERSION}.atpack" && \
    mkdir -p /opt/microchip/packs/Microchip.PIC12-16F1xxx_DFP && \
    unzip -q /tmp/dfp.atpack -d /opt/microchip/packs/Microchip.PIC12-16F1xxx_DFP && \
    rm -f /tmp/dfp.atpack

# -----------------------------------------------------------------------
# Stage 2 – lean runtime image with only the compiler + build tools
# -----------------------------------------------------------------------
FROM ubuntu:22.04

ARG XC8_VERSION=3.10

# Build tools and 32-bit runtime libs needed to *run* xc8-cc
RUN dpkg --add-architecture i386 && \
    apt-get update && \
    apt-get install -y --no-install-recommends \
        make \
        libc6:i386 \
        libstdc++6:i386 \
        libncurses5:i386 && \
    rm -rf /var/lib/apt/lists/*

# Copy the installed compiler and DFP from the builder stage
COPY --from=builder /opt/microchip/xc8 /opt/microchip/xc8
COPY --from=builder /opt/microchip/packs /opt/microchip/packs

# Make xc8-cc available on PATH
# Note: --prefix installs directly to /opt/microchip/xc8/bin (no version subdirectory)
ENV PATH="/opt/microchip/xc8/bin:${PATH}"

# Path to the PIC12-16F1xxx Device Family Pack (used by xc8-cc -mdfp)
# Note: must point to the xc8/ subdirectory inside the pack
ENV XC8_DFP="/opt/microchip/packs/Microchip.PIC12-16F1xxx_DFP/xc8"

# Source is mounted here at runtime
WORKDIR /src

# Default: run make (builds the project using the Makefile)
# Override with: docker run ... xc8-cc [args]
CMD ["make"]
