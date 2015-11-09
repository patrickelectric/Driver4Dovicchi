# Driver4Dovicchi
=D

# instala o source do kernel
sudo apt-get install linux-headers-3.16.0-4-*

# adicionar driver ao kernel
sudo insmod readfile.ko

# para ler as msgs do kernel printk
/var/log/messages

# arquivos para obter informacoes sobre o computador
/proc/meminfo
/proc/cpuinfo
/proc/version

#list all modules
lsmod

# adicionar modulos 
sudo insmod driver.ko

# remover driver 
sudo rmmod driver


