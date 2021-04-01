#cp arch/arm64/boot/dts/freescale/imx8mm-var-dart-feig.dtb /tftpboot/
cp arch/arm64/boot/Image.gz /media/kamel/rootfs/boot
cp arch/arm64/boot/dts/freescale/imx8mm-var-dart-feig.dtb /media/kamel/rootfs/boot/
#sudo cp /tftpboot/Image.gz /media/kamel/rootfs/boot/
umount /media/kamel/rootfs
