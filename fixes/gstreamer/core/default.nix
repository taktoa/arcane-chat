{ stdenv, fetchurl, pkgconfig, perl, bison, flex, python, gobjectIntrospection
, glib, makeWrapper, windows
}:

with stdenv.lib;

let nativeLinux = stdenv.isLinux && !(stdenv ? cross);
in stdenv.mkDerivation rec {
  name = "gstreamer-1.9.2";

  meta = {
    description = "Open source multimedia framework";
    homepage = "http://gstreamer.freedesktop.org";
    license = stdenv.lib.licenses.lgpl2Plus;
    platforms = stdenv.lib.platforms.unix;
    maintainers = [ stdenv.lib.maintainers.ttuegel ];
  };

  src = fetchurl {
    url = "${meta.homepage}/src/gstreamer/${name}.tar.xz";
    sha256 = "0ybmj3xzh75p3f0yvi28863lxkn51rfcf524qkl0b9zc25m1jnh0";
  };

  outputs = [ "dev" "out" ];
  outputBin = "dev";

  nativeBuildInputs = [
    pkgconfig perl bison flex python gobjectIntrospection makeWrapper
  ];

  buildInputs = optionals (!nativeLinux) [
    windows.mingw_w64_pthreads
  ];

  propagatedBuildInputs = [ glib ];

  patches = optionals (!nativeLinux) [ ./gstreamer-localtime_r.patch ];

  enableParallelBuilding = true;

  configureFlags = optionals (!nativeLinux) [
    "--disable-shared"
    "--enable-static"
    "--enable-check"
    "--disable-fatal-warnings"
  ];

  preConfigure = ''
    configureFlagsArray+=("--exec-prefix=$dev")
  '';

  postInstall = ''
    for prog in "$out/bin/"*; do
        wrapProgram "$prog" --prefix GST_PLUGIN_SYSTEM_PATH : "\$(unset _tmp; for profile in \$NIX_PROFILES; do _tmp="\$profile/lib/gstreamer-1.0''$\{_tmp:+:\}\$_tmp"; done; printf "\$_tmp")"
    done
  '';

  preFixup = ''
    moveToOutput "share/bash-completion" "$dev"
  '';

  setupHook = ./setup-hook.sh;
}
