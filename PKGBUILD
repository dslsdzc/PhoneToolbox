# Maintainer: PhoneToolbox Developers

pkgname=phonetoolbox
pkgver=0.0.1
pkgrel=1
pkgdesc='Cross-platform phone toolbox for Android device management (ADB/Fastboot)'
arch=(x86_64)
url=''
license=(custom)
depends=(qt6-base libusb)
makedepends=(cmake pkg-config)
source=("$pkgname-$pkgver::file://$PWD")
sha256sums=('SKIP')

build() {
    cd "$srcdir/$pkgname-$pkgver"
    cmake -B build \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr
    cmake --build build
}

package() {
    cd "$srcdir/$pkgname-$pkgver"
    install -Dm755 "build/PhoneToolbox" "$pkgdir/usr/bin/phonetoolbox"
    install -Dm644 "phonetoolbox.desktop" "$pkgdir/usr/share/applications/phonetoolbox.desktop"
}
