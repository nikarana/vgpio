# vgpio
virtual GPIO driver

実GPIOが無いPCのLinux上で仮想的なGPIOを作り操作できるようにする  
仮想GPIOドライバです。

・準備  
Makefileがあるディレクトリでmakeします。  
$ make  
→vgpio.koができます。  

・インストール  
$ sudo insmod vgpio.ko  
→/sys/kernel/vgpioディレクトリができます。

・Usage  
Ex.) GPIO23を in で使用する  
$ echo 23 > /sys/kernel/vgpio/export  
→ /sys/kernel/vgpio/gpio23ディレクトリができます。  
$ echo "in" > /sys/kernel/vgpio/gpio23/direction  
$ cat /sys/kernel/vgpio/gpio23/value  
0  
（値を変える場合は、別途 echo 1 > /sys/kernel/vgpio/gpio23/value する）  

・注意点
※本物のGPIOは、In方向では書き込みはできないが、このドライバではできてしまいます。  
※select/pollを使用した待ちは今できません。  
すぐできそうですが、自分がやりたいことは現状でできているので、  
しばらくこのままにしています。  

・ライセンス  
このドライバのライセンスは GPL v2 です。  
以下のソースを参考にしています。  
Linux kernel v4.18  
/samples/kobject/kobject-example.c  
/drivers/gpio/gpiolib-sysfs.c  
