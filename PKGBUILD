# Maintainer: betelqeyza <avsarusta4422@hotmail.com>
pkgname=excalibur-wmi-dkms
pkgver=0.1
pkgrel=1
pkgdesc="Casper Excalibur WMI driver (DKMS)"
arch=('x86_64')
url="https://github.com/betelqeyza/excalibur-wmi"
license=('GPL')
depends=('dkms')
source=('dkms.conf'
        'Makefile'
        'excalibur_wmi.c')
sha256sums=('SKIP'
            'SKIP'
            'SKIP')

package() {
  install -dm755 "${pkgdir}/usr/src/${pkgname%-dkms}-${pkgver}"
  cp dkms.conf Makefile excalibur_wmi.c "${pkgdir}/usr/src/${pkgname%-dkms}-${pkgver}/"
}
