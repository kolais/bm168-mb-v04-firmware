FROM debian:bookworm-20250203-slim@sha256:40b107342c492725bc7aacbe93a49945445191ae364184a6d24fedb28172f6f7

# A fixed Debian snapshot fixes compiler, image-tool and Python package versions.
RUN rm -f /etc/apt/sources.list.d/debian.sources && \
    printf '%s\n' \
      'deb [check-valid-until=no] http://snapshot.debian.org/archive/debian/20250210T000000Z bookworm main' \
      'deb [check-valid-until=no] http://snapshot.debian.org/archive/debian/20250210T000000Z bookworm-updates main' \
      'deb [check-valid-until=no] http://snapshot.debian.org/archive/debian-security/20250210T000000Z bookworm-security main' > /etc/apt/sources.list && \
    apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake ninja-build python3 python3-venv ca-certificates \
      file git xz-utils mtd-utils squashfs-tools qemu-user bc bison flex libssl-dev libelf-dev && \
    rm -rf /var/lib/apt/lists/*
COPY tools/requirements.txt /opt/requirements.txt
RUN python3 -m venv /opt/ubi && /opt/ubi/bin/pip install --no-cache-dir -r /opt/requirements.txt
ENV PATH="/opt/ubi/bin:${PATH}" LC_ALL=C TZ=UTC
WORKDIR /project
CMD ["bash"]
