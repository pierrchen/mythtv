include ( ../../config.mak )
include ( ../../settings.pro )

TEMPLATE = lib
TARGET = mythavformat-$$LIBVERSION
CONFIG += thread dll warn_off
target.path = $${LIBDIR}
INSTALLS = target

INCLUDEPATH += ../ ../../ ../libavcodec ../libavutil ../libmythtv

DEFINES += HAVE_AV_CONFIG_H _LARGEFILE_SOURCE

LIBS += $$LOCAL_LIBDIR_X11

cygwin :LIBS += -lz

QMAKE_CLEAN += $(TARGET) $(TARGETA) $(TARGETD) $(TARGET0) $(TARGET1) $(TARGET2)

# Input
HEADERS += asf.h avformat.h avi.h avio.h dv.h mpegts.h os_support.h qtpalette.h
HEADERS += riff.h

SOURCES += 4xm.c allformats.c asf.c au.c avidec.c avienc.c avio.c aviobuf.c 
SOURCES += crc.c cutils.c dv.c ffm.c file.c flvdec.c flvenc.c idcin.c idroq.c
SOURCES += ipmovie.c mov.c movenc.c mp3.c mpeg.c mpegts.c mpegtsenc.c 
SOURCES += mpjpeg.c nut.c os_support.c rm.c psxstr.c raw.c flic.c audio.c
SOURCES += segafilm.c swf.c utils.c wav.c wc3movie.c westwood.c yuv4mpeg.c
SOURCES += sierravmd.c asf-enc.c matroska.c img2.c electronicarts.c sol.c
SOURCES += nsvdec.c ogg2.c oggparsevorbis.c oggparsetheora.c oggparseflac.c
SOURCES += mmf.c gif.c gifdec.c daud.c adtsenc.c vocdec.c vocenc.c
SOURCES += aiff.c avs.c mm.c tta.c voc.c smacker.c oggparseogm.c nuv.c wv.c
SOURCES += gxf.c gxfenc.c riff.c amr.c mxf.c isom.c dvenc.c
SOURCES += dsicin.c mpc.c mtv.c tiertexseq.c

# not using:  barpainet.* beosaudio.cpp, dc1394.c, framehook.*
# not using:  grab.c v4l2.c x11grab.c

contains( CONFIG_APC_DEMUXER, yes) {
    SOURCES += apc.c
}

contains( CONFIG_BETHSOFTVID_DEMUXER, yes) {
    SOURCES += bethsoftvid.c
}

contains( CONFIG_C93_DEMUXER, yes) {
    SOURCES += c93.c
}

contains( CONFIG_DV1394_DEMUXER, yes) {
    SOURCES += dv1394.c
}

contains( CONFIG_DXA_DEMUXER, yes) {
    SOURCES += dxa.c
}

contains( CONFIG_THP_DEMUXER, yes) {
    SOURCES += thp.c
}

contains( CONFIG_VORBIS, yes ) {
    SOURCES += ogg.c
}

contains( CONFIG_NETWORK, yes ) {
    SOURCES += rtp.c rtp_h264.c rtpproto.c rtsp.c udp.c tcp.c http.c
}

inc.path = $${PREFIX}/include/mythtv/ffmpeg/
inc.files = avformat.h avio.h rtp.h rtsp.h rtspcodes.h

INSTALLS += inc

LIBS += -L../libavcodec -lmythavcodec-$$LIBVERSION -L../libavutil -lmythavutil-$$LIBVERSION

macx {
    LIBS               += -lz
    QMAKE_LFLAGS_SHLIB += -single_module
    QMAKE_LFLAGS_SHLIB += -seg1addr 0xC4000000
    SOURCES            -= audio.c
#    SOURCES            += audio-darwin.c
}

