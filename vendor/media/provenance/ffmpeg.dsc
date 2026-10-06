-----BEGIN PGP SIGNED MESSAGE-----
Hash: SHA512

Format: 3.0 (quilt)
Source: ffmpeg
Binary: ffmpeg, ffmpeg-doc, libavcodec61, libavcodec-extra61, libavcodec-extra, libavcodec-dev, libavdevice61, libavdevice-dev, libavfilter10, libavfilter-extra10, libavfilter-extra, libavfilter-dev, libavformat61, libavformat-extra61, libavformat-extra, libavformat-dev, libavutil59, libavutil-dev, libpostproc58, libpostproc-dev, libswresample5, libswresample-dev, libswscale8, libswscale-dev
Architecture: any all
Version: 7:7.1.5-0+deb13u1
Maintainer: Debian Multimedia Maintainers <debian-multimedia@lists.debian.org>
Uploaders:  Reinhard Tartler <siretart@tauware.de>, Balint Reczey <balint@balintreczey.hu>, James Cowgill <jcowgill@debian.org>, Sebastian Ramacher <sramacher@debian.org>,
Homepage: https://ffmpeg.org/
Standards-Version: 4.7.2
Vcs-Browser: https://salsa.debian.org/multimedia-team/ffmpeg
Vcs-Git: https://salsa.debian.org/multimedia-team/ffmpeg.git
Testsuite: autopkgtest
Testsuite-Triggers: build-essential, pkg-config
Build-Depends: clang [amd64 arm64 i386 ppc64el], debhelper-compat (= 13), glslang-dev, flite1-dev, frei0r-plugins-dev <!pkg.ffmpeg.stage1>, ladspa-sdk <!pkg.ffmpeg.stage1>, libaom-dev, libaribb24-dev <!pkg.ffmpeg.noextra>, libass-dev, libbluray-dev <!pkg.ffmpeg.stage1>, libbs2b-dev, libbz2-dev, libcaca-dev <!pkg.ffmpeg.stage1>, libcdio-paranoia-dev, libchromaprint-dev <!pkg.ffmpeg.stage1>, libcodec2-dev, libdav1d-dev, libdvdnav-dev <!pkg.ffmpeg.stage1>, libdvdread-dev <!pkg.ffmpeg.stage1>, libdc1394-dev [linux-any], libdrm-dev [linux-any], libffmpeg-nvenc-dev [amd64 arm64 i386], libfontconfig-dev, libfreetype-dev, libfribidi-dev, libgl-dev, libgme-dev, libgnutls28-dev, libgsm1-dev, libharfbuzz-dev, libiec61883-dev [linux-any], libavc1394-dev [linux-any], libjack-jackd2-dev <!pkg.ffmpeg.stage1>, libjxl-dev [!alpha] <!pkg.ffmpeg.stage1>, liblilv-dev <!pkg.ffmpeg.stage1>, liblzma-dev, libmp3lame-dev, libmysofa-dev, libopenal-dev, libopencore-amrnb-dev <!pkg.ffmpeg.noextra>, libopencore-amrwb-dev <!pkg.ffmpeg.noextra>, libopenjp2-7-dev (>= 2.1), libopenmpt-dev, libopus-dev, libplacebo-dev (>= 4.192) [linux-any] <!pkg.ffmpeg.stage1>, libpocketsphinx-dev (>= 0.8+5prealpha+1-7~) [!alpha !hppa !ia64 !m68k !mips64el !powerpc !ppc64 !s390x !sparc64] <!pkg.ffmpeg.stage1>, libpulse-dev <!pkg.ffmpeg.stage1>, librabbitmq-dev <!pkg.ffmpeg.stage1>, librav1e-dev [!alpha !hppa !hurd-i386 !ia64 !m68k !sh4 !sparc64 !x32] <!pkg.ffmpeg.stage1>, librist-dev <!pkg.ffmpeg.stage1>, librubberband-dev, librsvg2-dev [!alpha !hppa !hurd-i386 !ia64 !m68k !sh4 !x32] <!pkg.ffmpeg.stage1>, libsctp-dev [linux-any] <!pkg.ffmpeg.stage1>, libsdl2-dev <!pkg.ffmpeg.stage1>, libshine-dev (>= 3.0.0), libsmbclient-dev (>= 4.13) [!hurd-i386] <!pkg.ffmpeg.noextra>, libsnappy-dev, libsoxr-dev, libspeex-dev, libsrt-gnutls-dev <!pkg.ffmpeg.stage1>, libssh-dev <!pkg.ffmpeg.stage1>, libsvtav1enc-dev <!pkg.ffmpeg.stage1>, libtesseract-dev <!pkg.ffmpeg.noextra>, libtheora-dev, libtwolame-dev, libva-dev (>= 1.3) [!hurd-any], libvdpau-dev, libvidstab-dev, libvo-amrwbenc-dev <!pkg.ffmpeg.noextra>, libvorbis-dev, libvpl-dev [amd64], libvpx-dev, libvulkan-dev [linux-any], libwebp-dev, libx264-dev <!pkg.ffmpeg.stage1>, libx265-dev (>= 1.8), libxcb-shape0-dev, libxcb-shm0-dev, libxcb-xfixes0-dev, libxml2-dev, libxv-dev, libxvidcore-dev, libzimg-dev, libzmq3-dev <!pkg.ffmpeg.stage1>, libzvbi-dev <!pkg.ffmpeg.stage1>, ocl-icd-opencl-dev | opencl-dev, pkgconf, texinfo, nasm, zlib1g-dev
Build-Depends-Indep: cleancss, doxygen, node-less, tree
Package-List:
 ffmpeg deb video optional arch=any
 ffmpeg-doc deb doc optional arch=all
 libavcodec-dev deb libdevel optional arch=any
 libavcodec-extra deb metapackages optional arch=any profile=!pkg.ffmpeg.noextra
 libavcodec-extra61 deb libs optional arch=any profile=!pkg.ffmpeg.noextra
 libavcodec61 deb libs optional arch=any
 libavdevice-dev deb libdevel optional arch=any
 libavdevice61 deb libs optional arch=any
 libavfilter-dev deb libdevel optional arch=any
 libavfilter-extra deb metapackages optional arch=any profile=!pkg.ffmpeg.noextra
 libavfilter-extra10 deb libs optional arch=any profile=!pkg.ffmpeg.noextra
 libavfilter10 deb libs optional arch=any
 libavformat-dev deb libdevel optional arch=any
 libavformat-extra deb metapackages optional arch=any profile=!pkg.ffmpeg.noextra
 libavformat-extra61 deb libs optional arch=any profile=!pkg.ffmpeg.noextra
 libavformat61 deb libs optional arch=any
 libavutil-dev deb libdevel optional arch=any
 libavutil59 deb libs optional arch=any
 libpostproc-dev deb libdevel optional arch=any
 libpostproc58 deb libs optional arch=any
 libswresample-dev deb libdevel optional arch=any
 libswresample5 deb libs optional arch=any
 libswscale-dev deb libdevel optional arch=any
 libswscale8 deb libs optional arch=any
Checksums-Sha1:
 280615051f0546371d20d8b119f3ff357e0fb948 11050340 ffmpeg_7.1.5.orig.tar.xz
 bbceadec2643f1c9dfd8ce6b954d7b59c7f69b28 520 ffmpeg_7.1.5.orig.tar.xz.asc
 af86a93cdf7bad826e249aaf9af574095fe00282 54176 ffmpeg_7.1.5-0+deb13u1.debian.tar.xz
Checksums-Sha256:
 de668509caf9e35e3cd162473441fdb29538c6d96ed080292b3cf9e6fc5d558f 11050340 ffmpeg_7.1.5.orig.tar.xz
 7ce4b9d56e3ef0cd4c3a9c0b2c8a034ac91e6c4c011c1f10b05a8aa7292fca35 520 ffmpeg_7.1.5.orig.tar.xz.asc
 a1be51d8a10744952fe94fa318bf71bbc8074bed0951382c079ab7ef227f74ef 54176 ffmpeg_7.1.5-0+deb13u1.debian.tar.xz
Files:
 8a5e3d530be908235511f585ccaceafd 11050340 ffmpeg_7.1.5.orig.tar.xz
 39edeb1675b322cd044a74e447a9463e 520 ffmpeg_7.1.5.orig.tar.xz.asc
 2f632ffb7bfdb4875c6096d1aa6e6ecb 54176 ffmpeg_7.1.5-0+deb13u1.debian.tar.xz


-----BEGIN PGP SIGNATURE-----

wr0EARYKAG8Fgmo5YfQJECGTazZgD82JRxQAAAAAAB4AIHNhbHRAbm90YXRpb25z
LnNlcXVvaWEtcGdwLm9yZwLr2oNbH7Z6uefYj2vvuxd/QKyDwEQVH6XdS88cd3Du
FiEEQmJ+hB2ZZ9qD4fqQIZNrNmAPzYkAAE90AQC1Ra94PGk+4JrSLKlw9IwVDJi3
uuSew0wInFe1RF4MdwEA0WAai+GSd8OMHPZdbDkMUETAG4jt7I2Uee5iUPM2BA0=
=MFwl
-----END PGP SIGNATURE-----
