# One CI image: a pinned distro base with one profile's packages installed
# (tools/ci/images/provision.sh). tools/ci/ci_images.py builds it from a context
# that holds only the profile's input files, so the tag's hash covers every byte
# the build reads.
ARG BASE
FROM ${BASE} AS profile
ARG PROFILE
ARG PUTGD_QT_VERSION=
COPY . /tmp/putgd-ci-image
RUN PUTGD_QT_VERSION="$PUTGD_QT_VERSION" bash /tmp/putgd-ci-image/tools/ci/images/provision.sh "$PROFILE" \
    && rm -rf /tmp/putgd-ci-image

# The AppImage builder: what jurplel/install-qt-action exported on the hosted runner.
FROM profile AS appimage
ARG PUTGD_QT_VERSION
ENV QT_ROOT_DIR=/opt/Qt/${PUTGD_QT_VERSION}/gcc_64
ENV PATH=${QT_ROOT_DIR}/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
    LD_LIBRARY_PATH=${QT_ROOT_DIR}/lib \
    PKG_CONFIG_PATH=${QT_ROOT_DIR}/lib/pkgconfig \
    QT_PLUGIN_PATH=${QT_ROOT_DIR}/plugins \
    QML2_IMPORT_PATH=${QT_ROOT_DIR}/qml
