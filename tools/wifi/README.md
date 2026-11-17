# Wi-Fi on the H2: the hardware is there

The H2's Bluetooth module, an AzureWave AW-NM372SM (Broadcom BCM43438), also has
2.4 GHz Wi-Fi (802.11 b/g/n), and it's wired: enabled by PC17 (`WL_REG_ON`), on
SDIO through the X1000's second MMC controller (MSC1, PC00-PC05). The stock
firmware never used it.

`sdio-probe.dts.patch` is the device-tree change that showed it: applied after
the board patch (copy it into `buildroot/external/board/h2/patches/linux/` and
rebuild the kernel with `linux-dirclean all`), the kernel finds
`mmc1: new high speed SDIO card` with vendor 0x02d0, device 0xa9a6. It keeps the
module (and so Bluetooth's supply) powered all the time, so it isn't in the build.

What Wi-Fi in Fathom would take:

- kernel: `CONFIG_BRCMFMAC` with SDIO, `CONFIG_CFG80211`, and a power sequence
  that follows Wi-Fi being on or off (one supply, BT_PWR on PC22, with Bluetooth);
- the chip's firmware and calibration from the user's own `flash.img` (as for
  Bluetooth): stock `cyw43438-7.46.58.35.bin` and `nvram_azw372.txt`, loaded as
  `brcm/brcmfmac43430-sdio.bin` and `brcm/brcmfmac43430-sdio.txt`;
- `wpa_supplicant`, DHCP, and something to use it for (network shares, DLNA,
  internet radio, streaming services' APIs where they allow it);
- RAM: about 6 MB is free with the player running, so every daemon counts.
