FROM docker.io/library/debian:bookworm-slim

# Debian 12 keeps the runtime ABI at glibc 2.36. Do not use a rolling tag.
COPY setup.sh /tmp/steamdeck-setup.sh
RUN bash /tmp/steamdeck-setup.sh && rm /tmp/steamdeck-setup.sh
WORKDIR /src
