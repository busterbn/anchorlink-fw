FROM ghcr.io/nrfconnect/sdk-nrf-toolchain:v3.1.1

# Makes the nrf image install jlink
ENV ACCEPT_JLINK_LICENSE=1

# Install just
RUN wget -O- https://just.systems/install.sh | bash -s -- --to /bin && just --version

WORKDIR /build-env
COPY patches ./patches
COPY project/west.yml ./project/west.yml
COPY justfile .
RUN just init
RUN rm -rf project

WORKDIR /workdir
