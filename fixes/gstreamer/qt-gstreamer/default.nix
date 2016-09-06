{ stdenv, fetchurl, fetchpatch, cmake, automoc4, flex, bison, pkgconfig
, gstreamer, gst-plugins-base, boost, glib, qtbase, qtdeclarative }:

stdenv.mkDerivation rec {
  name = "${pname}-1.2.0";
  pname = "qt-gstreamer";

  src = fetchurl {
    url = "http://gstreamer.freedesktop.org/src/${pname}/${name}.tar.xz";
    sha256 = "9f3b492b74cad9be918e4c4db96df48dab9c012f2ae5667f438b64a4d92e8fd4";
  };

  patches = [
    (fetchpatch {
      url = "https://cgit.freedesktop.org/gstreamer/qt-gstreamer/patch/?id=e2ca8094aa8d0eac1c3a98df66fe94ce0c754088";
      sha256 = "1qps0nlc26d74wk8h96xl5s3d9qrdx6c0ph0zpl1dnc691lgyf6s";
    })
  ];

  outputs = [ "dev" "out" ];

  buildInputs = [ gstreamer gst-plugins-base glib qtbase qtdeclarative ];
  propagatedBuildInputs = [ boost ];
  nativeBuildInputs = [ cmake automoc4 flex bison pkgconfig ];

  cmakeFlags = "-DQT_VERSION=5 -DUSE_QT_PLUGIN_DIR=OFF -DUSE_GST_PLUGIN_DIR=OFF";

  meta = {
    platforms = stdenv.lib.platforms.linux;
  };
}
