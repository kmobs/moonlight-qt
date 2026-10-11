TEMPLATE = subdirs
CONFIG += ordered

linkpolicy.file = $$PWD/linkpolicy.pro
SUBDIRS += linkpolicy

framing.file = $$PWD/framing.pro
SUBDIRS += framing

compression.file = $$PWD/compression.pro
SUBDIRS += compression

sdp.file = $$PWD/sdp.pro
SUBDIRS += sdp

# The round trip compiles the vendored codec and needs a Vulkan GPU at runtime
win32:contains(QT_ARCH, x86_64) {
    roundtrip.file = $$PWD/roundtrip.pro
    SUBDIRS += roundtrip

    # The client's D3D11 surface pool and Vulkan decoder, without a host
    d3d11.file = $$PWD/d3d11.pro
    SUBDIRS += d3d11
}
linux:contains(QT_ARCH, x86_64) {
    roundtrip.file = $$PWD/roundtrip.pro
    SUBDIRS += roundtrip
}
macx {
    roundtrip.file = $$PWD/roundtrip.pro
    SUBDIRS += roundtrip

    metalcalibration.file = $$PWD/metalcalibration.pro
    SUBDIRS += metalcalibration
}

rtpqueue.file = $$PWD/rtpqueue.pro
SUBDIRS += rtpqueue

udpreceive.file = $$PWD/udpreceive.pro
SUBDIRS += udpreceive

udpaddress.file = $$PWD/udpaddress.pro
SUBDIRS += udpaddress
