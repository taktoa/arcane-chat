{ stdenv, fetchurl, pkgconfig, python
, gst-plugins-base, orc, bzip2
, libv4l, libdv, libavc1394, libiec61883
, libvpx, speex, flac, taglib, libshout
, cairo, gdk_pixbuf, aalib, libcaca
, libsoup, libpulseaudio, libintlOrEmpty
}:

let
  inherit (stdenv.lib) optionals optionalString;
in
stdenv.mkDerivation rec {
  name = "gst-plugins-good-1.9.2";

  meta = with stdenv.lib; {
    description = "Gstreamer Good Plugins";
    homepage    = "http://gstreamer.freedesktop.org";
    longDescription = ''
      a set of plug-ins that we consider to have good quality code,
      correct functionality, our preferred license (LGPL for the plug-in
      code, LGPL or LGPL-compatible for the supporting library).
    '';
    license     = licenses.lgpl2Plus;
    platforms   = platforms.unix;
  };

  src = fetchurl {
    url = "${meta.homepage}/src/gst-plugins-good/${name}.tar.xz";
    sha256 = "1inq4lcs600myzpvyrc5sr328xk22zgsivsmnmsi4b23f3vvf9rs";
  };

  outputs = [ "dev" "out" ];

  nativeBuildInputs = [ pkgconfig python ];

  buildInputs = [
    gst-plugins-base orc bzip2
    libdv libvpx speex flac taglib
    cairo gdk_pixbuf aalib libcaca
    libsoup libshout
  ]
  ++ libintlOrEmpty
  ++ optionals stdenv.isLinux [ libv4l libpulseaudio libavc1394 libiec61883 ];

  configureFlags = if (!stdenv.isLinux || (stdenv ? cross)) then [
    "--disable-shared"
    "--enable-static"
    "--disable-fatal-warnings"
  ] else [
  ];

  preFixup = ''
    mkdir -p "$dev/lib/gstreamer-1.0"
    mv "$out/lib/gstreamer-1.0/"*.la "$dev/lib/gstreamer-1.0"
  '';

  LDFLAGS = optionalString stdenv.isDarwin "-lintl";
}
