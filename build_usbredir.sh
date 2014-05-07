#!/bin/sh

# Grab dependencies
sudo apt-get install autoconf libtool

PREFIX=/opt/neverware_usbredir
if [ ! -d $PREFIX ]; then
    sudo mkdir -p $PREFIX
fi
./autogen.sh --prefix=$PREFIX
make
sudo make install
