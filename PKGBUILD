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
    # 插件（MODULE）必须单独安装：主程序会扫描 ../lib/phonetoolbox/plugins。
    # 不装的话插件面板在用户机器上永远显示"未找到插件"。
    install -Dm644 "build/plugins/libhuawei_flash_plugin.so" \
        "$pkgdir/usr/lib/phonetoolbox/plugins/libhuawei_flash_plugin.so"
}
