#! /bin/sh
# install-candled.sh -- install candled, the candle server, on Debian, Ubuntu (WSL2 too), Fedora and other
# RPM-based systems, Alpine, or anything else with a C compiler:  build, install, make a service user and the
# server's key, install and start the service.  Run it from the unpacked source (or a git checkout that has been
# bootstrapped), as root; or with --user to put everything under your home directory with no root at all.
#
#   sudo sh linux/install-candled.sh                  # system-wide, from this source tree
#   sh linux/install-candled.sh --user                # for you only (no root, no service, run it by hand or from your own unit)
#   sudo sh linux/install-candled.sh --uninstall [--purge]
set -eu

PROG=install-candled.sh
SRC=$(cd "$(dirname "$0")/.." && pwd)
MODE=system; ACTION=install; PURGE=no; SERVICE=yes; DEPS=yes; BIND=""; PREFIX=""; ASSUME_YES=no; WSL_SYSTEMD=no
CANDLED_USER=candled

usage() {
cat <<EOF
Usage: sh $PROG [OPTION]...
Install candled, the candle server of wincan.

  --user               install for the current user only: under ~/.local, no root, no service
  --prefix=DIR         install under DIR (default /usr/local; with --user, ~/.local)
  --bind=ADDRESS       the address candled listens on (default 127.0.0.1)
  --no-service         do not install or start a service
  --no-deps            do not install the build dependencies (the script says which are missing)
  --enable-wsl-systemd on WSL2 without systemd: turn systemd on in /etc/wsl.conf (a backup is made; WSL must then be
                       restarted with  wsl --shutdown  from Windows)
  --uninstall          remove what this script installed (the configuration, the key and the data stay ...
  --purge              ... unless --purge is also given)
  -y, --yes            do not ask
  -h, --help           display this help and exit
EOF
}

say() { printf '%s\n' "$*"; }
die() { printf '%s: %s\n' "$PROG" "$*" >&2; exit 1; }
ask() { [ "$ASSUME_YES" = yes ] && return 0; printf '%s [y/N] ' "$1"; read -r a || a=n; case $a in y|Y|yes) return 0;; *) return 1;; esac; }

for a in "$@"; do
    case $a in
        --user) MODE=user ;;
        --prefix=*) PREFIX=${a#--prefix=} ;;
        --bind=*) BIND=${a#--bind=} ;;
        --no-service) SERVICE=no ;;
        --no-deps) DEPS=no ;;
        --enable-wsl-systemd) WSL_SYSTEMD=yes ;;
        --uninstall) ACTION=uninstall ;;
        --purge) PURGE=yes ;;
        -y|--yes) ASSUME_YES=yes ;;
        -h|--help) usage; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
done

# ---- where things go
if [ "$MODE" = user ]; then
    [ -n "$PREFIX" ] || PREFIX=$HOME/.local
    SYSCONF=${XDG_CONFIG_HOME:-$HOME/.config}      # candled looks for $SYSCONF/candled.conf and $SYSCONF/candled/
    STATE=$PREFIX/var/lib/candled
    CONF=$SYSCONF/candled.conf
    CONFDIR=$SYSCONF/candled
    SERVICE=no
else
    [ "$(id -u)" = 0 ] || die "run as root (sudo), or use --user"
    [ -n "$PREFIX" ] || PREFIX=/usr/local
    SYSCONF=/etc; STATE=/var/lib/candled; CONF=/etc/candled.conf; CONFDIR=/etc/candled
fi

# ---- what system is this
OSID=unknown; OSLIKE=""
if [ -r /etc/os-release ]; then OSID=$(. /etc/os-release; echo "${ID:-unknown}"); OSLIKE=$(. /etc/os-release; echo "${ID_LIKE:-}"); fi
PM=""
case " $OSID $OSLIKE " in
    *" debian "*|*" ubuntu "*) PM=apt ;;
    *" fedora "*|*" rhel "*|*" centos "*) if command -v dnf >/dev/null 2>&1; then PM=dnf; else PM=yum; fi ;;
    *" suse "*|*" opensuse "*) PM=zypper ;;
    *" alpine "*) PM=apk ;;
    *" arch "*) PM=pacman ;;
esac
WSL=no; grep -qi microsoft /proc/version 2>/dev/null && WSL=yes
INIT=none
if [ -d /run/systemd/system ]; then INIT=systemd
elif command -v rc-service >/dev/null 2>&1 && [ -x /sbin/openrc-run ]; then INIT=openrc; fi
say "system: $OSID${OSLIKE:+ ($OSLIKE)}, package manager: ${PM:-none known}, init: $INIT${WSL:+, WSL2: $WSL}"

# ---- uninstall
if [ "$ACTION" = uninstall ]; then
    if [ "$MODE" = system ]; then
        case $INIT in
            systemd) systemctl disable --now candled 2>/dev/null || true; rm -f /etc/systemd/system/candled.service; systemctl daemon-reload 2>/dev/null || true ;;
            openrc) rc-service candled stop 2>/dev/null || true; rc-update del candled default 2>/dev/null || true; rm -f /etc/init.d/candled ;;
        esac
    fi
    if [ -f "$SRC/Makefile" ]; then ( cd "$SRC" && make uninstall ) || true; else rm -f "$PREFIX/sbin/candled"; fi
    if [ "$PURGE" = yes ]; then
        rm -rf "$CONFDIR" "$CONF" "$STATE"
        if [ "$MODE" = system ]; then userdel "$CANDLED_USER" 2>/dev/null || deluser "$CANDLED_USER" 2>/dev/null || true; fi
        say "candled and its configuration, key and data are removed."
    else
        say "candled is removed.  Kept: $CONF, $CONFDIR (the key and the authorized clients) and $STATE; --purge removes them."
    fi
    exit 0
fi

# ---- build dependencies
need_deps=no
cc_ok() { printf '#include <sqlite3.h>\n#include <curl/curl.h>\n#include <openssl/ssl.h>\nint main(void){return 0;}\n' | ${CC:-cc} -x c - -o /dev/null -lsqlite3 -lcurl -lssl -lcrypto 2>/dev/null; }
if ! command -v "${CC:-cc}" >/dev/null 2>&1 || ! command -v make >/dev/null 2>&1 || ! cc_ok; then need_deps=yes; fi
if [ "$need_deps" = yes ]; then
    case $PM in
        apt)    PKGS="build-essential libsqlite3-dev libcurl4-openssl-dev libssl-dev"; INSTALL="apt-get install -y" ;;
        dnf)    PKGS="gcc make sqlite-devel libcurl-devel openssl-devel"; INSTALL="dnf install -y" ;;
        yum)    PKGS="gcc make sqlite-devel libcurl-devel openssl-devel"; INSTALL="yum install -y" ;;
        zypper) PKGS="gcc make sqlite3-devel libcurl-devel libopenssl-devel"; INSTALL="zypper --non-interactive install" ;;
        apk)    PKGS="build-base sqlite-dev curl-dev openssl-dev"; INSTALL="apk add" ;;
        pacman) PKGS="base-devel sqlite curl openssl"; INSTALL="pacman -S --noconfirm" ;;
        *)      PKGS=""; INSTALL="" ;;
    esac
    if [ -z "$PKGS" ]; then die "a C compiler, make and the development files of sqlite3, libcurl and OpenSSL are needed; install them and run this again"; fi
    if [ "$DEPS" = yes ] && [ "$MODE" = system ]; then
        say "installing the build dependencies: $PKGS"
        [ "$PM" = apt ] && apt-get update
        # shellcheck disable=SC2086
        $INSTALL $PKGS
    else
        die "missing build dependencies; install: $PKGS"
    fi
fi

# ---- build and install
cd "$SRC"
[ -x ./configure ] || { [ -x ./bootstrap ] && ./bootstrap; } || die "no configure script here: unpack a release tarball, or run ./bootstrap in a checkout (needs autoconf and automake)"
UNIT_ARGS=""
[ "$MODE" = system ] && [ "$INIT" = systemd ] && UNIT_ARGS="--with-systemdsystemunitdir=/etc/systemd/system"
if [ "$MODE" = user ]; then
    ./configure --prefix="$PREFIX" --sysconfdir="$SYSCONF" --localstatedir="$PREFIX/var" >/dev/null
else
    ./configure --prefix="$PREFIX" --sysconfdir="$SYSCONF" --localstatedir=/var $UNIT_ARGS >/dev/null
fi
make >/dev/null
make install >/dev/null
CANDLED=$PREFIX/sbin/candled
[ -x "$CANDLED" ] || die "the build did not make $CANDLED"
say "installed $CANDLED ($("$CANDLED" --version | head -n 1))"

# ---- the user, the directories, the configuration
if [ "$MODE" = system ]; then
    if ! id "$CANDLED_USER" >/dev/null 2>&1; then
        say "making the service user $CANDLED_USER"
        if command -v useradd >/dev/null 2>&1; then useradd --system --home-dir "$STATE" --no-create-home --shell /usr/sbin/nologin "$CANDLED_USER"
        else addgroup -S "$CANDLED_USER" 2>/dev/null || true; adduser -S -D -H -h "$STATE" -s /sbin/nologin -G "$CANDLED_USER" "$CANDLED_USER"; fi
    fi
    mkdir -p "$STATE/candles" "$CONFDIR"
    chown -R "$CANDLED_USER:$CANDLED_USER" "$STATE"
    chown root:"$CANDLED_USER" "$CONFDIR"; chmod 750 "$CONFDIR"
else
    mkdir -p "$STATE/candles" "$CONFDIR"
    # the sample names the system directories: point it at this user's
    sed -i -e "s|/var/lib/candled|$STATE|g" -e "s|/etc/candled/|$CONFDIR/|g" "$CONF"
fi
if [ -n "$BIND" ] && [ -f "$CONF" ]; then
    if grep -q '^listen ' "$CONF"; then sed -i "s|^listen .*|listen $BIND|" "$CONF"; else printf 'listen %s\n' "$BIND" >> "$CONF"; fi
    say "candled will listen on $BIND (put a firewall in front of the port if that is not a loopback address)"
fi
CERT=$CONFDIR/server.crt; KEY=$CONFDIR/server.key; AUTHZ=$CONFDIR/authorized-clients
"$CANDLED" --config "$CONF" --check-config --certificate "$CERT" --key "$KEY" >/dev/null || die "the configuration $CONF is not good"
FP=$("$CANDLED" --config "$CONF" --gen-cert --certificate "$CERT" --key "$KEY" | sed -n 's/^server fingerprint: //p')
if [ "$MODE" = system ]; then
    chown root:"$CANDLED_USER" "$KEY" "$CERT" 2>/dev/null || true; chmod 640 "$KEY"; chmod 644 "$CERT"
    touch "$AUTHZ"; chown root:"$CANDLED_USER" "$AUTHZ"; chmod 640 "$AUTHZ"
else
    touch "$AUTHZ"
fi

# ---- the service
STARTED=no
if [ "$SERVICE" = yes ] && [ "$MODE" = system ]; then
    case $INIT in
        systemd)
            [ -f /etc/systemd/system/candled.service ] || cp "$SRC/contrib/candled.service" /etc/systemd/system/candled.service
            systemctl daemon-reload
            systemctl enable --now candled && STARTED=yes ;;
        openrc)
            cp "$SRC/contrib/candled.openrc" /etc/init.d/candled; chmod 755 /etc/init.d/candled
            rc-update add candled default; rc-service candled start && STARTED=yes ;;
        *)
            if [ "$WSL" = yes ]; then
                say ""
                say "WSL2 is running without systemd, so there is no service manager to start candled."
                if [ "$WSL_SYSTEMD" = yes ]; then
                    [ -f /etc/wsl.conf ] && cp /etc/wsl.conf "/etc/wsl.conf.before-candled.$(date +%Y%m%d%H%M%S)"
                    if grep -q '^\[boot\]' /etc/wsl.conf 2>/dev/null; then
                        grep -q '^systemd *= *true' /etc/wsl.conf || sed -i '/^\[boot\]/a systemd=true' /etc/wsl.conf
                    else printf '\n[boot]\nsystemd=true\n' >> /etc/wsl.conf; fi
                    say "systemd=true is now in /etc/wsl.conf (a backup was made).  From Windows run  wsl --shutdown,  open WSL again,"
                    say "and run  sudo systemctl enable --now candled"
                else
                    say "Choose one:"
                    say "  - start it by hand:     sudo -u $CANDLED_USER $CANDLED --daemon --pidfile /run/candled.pid     (logs go to the system log)"
                    say "  - turn systemd on:      sudo sh $0 --enable-wsl-systemd     (then  wsl --shutdown  from Windows)"
                    say "  - start it at every WSL start: put   command = $CANDLED --daemon --pidfile /run/candled.pid   in the [boot] section of /etc/wsl.conf"
                fi
            else
                say "no service manager found: start it with   $CANDLED --daemon --pidfile /run/candled.pid"
            fi ;;
    esac
fi

# ---- what to do next
LISTEN=$(sed -n 's/^listen  *//p' "$CONF" | head -n 1); [ -n "$LISTEN" ] || LISTEN=127.0.0.1
PORT=$(sed -n 's/^port  *//p' "$CONF" | head -n 1); [ -n "$PORT" ] || PORT=8766
say ""
say "candled is installed."
say "  configuration:       $CONF"
say "  candle files:        $STATE/candles  (put the collector's NAME.bin files here, or change 'candles' in the configuration)"
say "  listening on:        $LISTEN:$PORT (TLS, clients authorized by key)"
[ "$STARTED" = yes ] && say "  service:             started"
say "  server fingerprint:  $FP"
say ""
say "To let a wincan in, give its owner the server fingerprint above (wincan's server_pin), and authorize the key it shows:"
say "    $( [ "$MODE" = system ] && echo sudo ) $CANDLED --authorize SHA256:...      (the fingerprint the Windows installer printed)"
WINDIR=$PREFIX/share/wincan/windows
if [ "$WSL" = yes ] && [ -d "$WINDIR" ] && command -v wslpath >/dev/null 2>&1; then
    say ""
    say "On Windows (this is WSL2: localhost reaches this candled), install wincan with:"
    say "    powershell.exe -ExecutionPolicy Bypass -File \"\$(wslpath -w $WINDIR/install.ps1)\" -Server https://localhost:$PORT -ServerPin $FP -Launch"
fi
