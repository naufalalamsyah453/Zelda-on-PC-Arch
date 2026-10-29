#!/bin/bash
# Builds an Arch Linux package (.pkg.tar.zst) from the staged portable build so it can be
# installed with `pacman -U`. Because Zelda on PC keeps settings/saves next to its own
# executable, the package installs the read-only content to /opt/ZeldaOnPC and ships a
# launcher (/usr/bin/zelda-on-pc) that copies it to a per-user writable folder on first run.
# Usage: tools/package_arch.sh <version> [stage-dir]   (after tools/package_linux.sh)
#   stage-dir defaults to build/stage-linux/ZeldaOnPC
set -euo pipefail

version="$1"
root="$(cd "$(dirname "$0")/.." && pwd)"
script="$(cd "$(dirname "$0")" && pwd)/$(basename "$0")"
stage="${2:-$root/build/stage-linux/ZeldaOnPC}"

# makepkg refuses to run as root (as in CI containers). Hand the build to an unprivileged
# user: everything this script writes lives under build/, so only that has to be chowned.
if [ "$(id -u)" -eq 0 ]; then
	builder="${ARCH_PKG_BUILDER:-zpkg}"
	id "$builder" >/dev/null 2>&1 || useradd -m "$builder"
	chown -R "$builder" "$root/build"
	exec sudo -u "$builder" bash "$script" "$@"
fi

pkgname="zelda-on-pc"
pkgver="$version"
pkgrel="1"
arch="x86_64"
outfile="$root/build/${pkgname}-${pkgver}-${pkgrel}-${arch}.pkg.tar.zst"

if [ ! -d "$stage" ]; then
	echo "package_arch.sh: stage dir not found: $stage" >&2
	exit 1
fi

builddir="$root/build/archpkg"
rm -rf "$builddir"
mkdir -p "$builddir"

# Launcher: run the app from a per-user writable copy (the app writes settings.ini and
# data/ next to its executable; /opt is read-only for a normal user).
cat > "$builddir/zelda-on-pc" <<'EOF'
#!/bin/sh
set -e
SRC="/opt/ZeldaOnPC"
RUN="${XDG_DATA_HOME:-$HOME/.local/share}/zelda-on-pc"
mkdir -p "$RUN"
# Copy the app into the user's folder on first run and refresh it when the installed
# binary is newer (package update). data/ and settings.ini are not in SRC, so they are
# preserved across copies.
if [ ! -x "$RUN/ZeldaOnPC" ] || [ "$SRC/ZeldaOnPC" -nt "$RUN/ZeldaOnPC" ]; then
	cp -a "$SRC/." "$RUN/"
fi
exec "$RUN/ZeldaOnPC" "$@"
EOF
chmod 755 "$builddir/zelda-on-pc"

# Desktop entry.
cat > "$builddir/zelda-on-pc.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=Zelda on PC
GenericName=Wii U Emulator Game
Comment=The Legend of Zelda: Breath of the Wild, set up as a PC game
Exec=zelda-on-pc
Icon=zelda-on-pc
Terminal=false
Categories=Game;Emulator;
StartupNotify=true
EOF

# Icon (project logo). Optional: skip if not present.
if [ -f "$root/docs/icon.png" ]; then
	cp "$root/docs/icon.png" "$builddir/zelda-on-pc.png"
fi

# PKGBUILD. $stage is baked in as an absolute path; $startdir/$pkgdir stay literal for makepkg.
cat > "$builddir/PKGBUILD" <<EOF
pkgname=$pkgname
pkgver=$pkgver
pkgrel=$pkgrel
pkgdesc="The Legend of Zelda: Breath of the Wild, set up as a PC game (Cemu fork)"
arch=('x86_64')
url="https://github.com/naufalalamsyah453/Zelda-on-PC-Arch"
license=('MPL2')
depends=('gtk3' 'libpulse' 'bluez-libs' 'vulkan-icd-loader' 'mesa')
optdepends=('gamemode: Feral Interactive GameMode support')
source=()
sha256sums=()

package() {
	# read-only application content; the launcher copies it to a user folder at runtime
	install -d "\$pkgdir/opt/ZeldaOnPC"
	cp -a "$stage/." "\$pkgdir/opt/ZeldaOnPC/"
	# launcher + desktop entry
	install -Dm755 "\$startdir/zelda-on-pc" "\$pkgdir/usr/bin/zelda-on-pc"
	install -Dm644 "\$startdir/zelda-on-pc.desktop" "\$pkgdir/usr/share/applications/zelda-on-pc.desktop"
	if [ -f "\$startdir/zelda-on-pc.png" ]; then
		install -Dm644 "\$startdir/zelda-on-pc.png" "\$pkgdir/usr/share/pixmaps/zelda-on-pc.png"
	fi
}
EOF

# Build the package. --nodeps: dependencies are already satisfied / not needed to package.
( cd "$builddir" && makepkg --nodeps --force --noconfirm )

if [ ! -f "$builddir/${pkgname}-${pkgver}-${pkgrel}-${arch}.pkg.tar.zst" ]; then
	echo "package_arch.sh: expected package not produced" >&2
	exit 1
fi
mv -f "$builddir/${pkgname}-${pkgver}-${pkgrel}-${arch}.pkg.tar.zst" "$outfile"
echo "Built $outfile ($(du -h "$outfile" | cut -f1))"
