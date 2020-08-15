# ESP32-S2

I recommended lubuntu 18.4 and python3 for this tutorial

# First step: Download esp-idf
```
cd ~/esp
git clone --recursive https://github.com/espressif/esp-idf.git'
```

# Second step: Install
```
cd ~/esp/esp-idf
./install.sh
```

# Export tools
`. ./export.sh`

Our main compile file is idf.py . Before use it, we must export tools every terminal

We can create a alians to do this with this commands.

`sudo gedit ~/.bashrc`

Write this command start of script.

`alias get_idf='. $HOME/esp/esp-idf/export.sh'`

There two way to compile and flash projects.(recommended 2. way)

after that we can start to use eclipse as a compiler.But before it we must update java 8 to java 11 with this command.

`sudo apt install default-java`

Download eclipse from this link: https://www.eclipse.org/downloads/ 

Install eclipse C/C++ developer kit

this tutorial show how to configs and other stuffs : https://github.com/espressif/idf-eclipse-plugin/blob/master/README.md

Note: When we changed projects compiler step in a bug like : Build tools not configured correctly 


# Second way (Without compiler)

After export tools we can compile and flash our first program
```
cd ~/esp
cp -r /esp-idf/examples/get-started/blink
cd blink
```
Write this command to export tools

`get_idf`

Set project target with this command

`idf.py set-target esp32s2`

Regulate config.(Default configs enough yet.)

`idf.py menuconfig`

Build the project.

`idf.py build`

# Flash 

Learn port of device.

`ls /dev/tty*`

Put it in device !!!

`idf.py -p /dev/ttyUSB0 flash`
        
# congratulations

I wait your questions

Regards

´´´
