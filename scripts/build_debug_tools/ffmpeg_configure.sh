#!/bin/bash
./configure \
                --enable-pic \
				--enable-static \
				--disable-shared \
                --disable-stripping \
                --disable-doc \
                \
                --disable-everything \
                \
                --enable-indev=v4l2 \
                \
                --enable-protocol=file \
                --enable-protocol=pipe \
                --enable-protocol=http \
                --enable-protocol=https \
                \
                --enable-muxer=matroska \
                --enable-muxer=mjpeg \
                --enable-muxer=rawvideo \
                --enable-muxer=rtp \
                \
                --enable-demuxer=rtsp \
                --enable-demuxer=matroska \
                --enable-demuxer=mjpeg \
                --enable-demuxer=rawvideo \
                \
                --enable-decoder=h264 \
                --enable-decoder=mpeg4 \
                --enable-decoder=mjpeg \
                \
                --enable-parser=h264 \
                --enable-parser=mpeg4video \
                --enable-parser=mjpeg \
                \
                --enable-encoder=mjpeg \
                --enable-encoder=rawvideo \
                \


