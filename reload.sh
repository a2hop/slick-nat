#!/bin/bash

./loader.sh unload
git pull
./build.sh
./loader.sh load    

# Configure NAT mappings
./src/slnat bri1 add 7607:af56:ff8:d12::/96 2607:f8f8:631:d601:2000:d12::/96
./src/slnat bri1 add 7607:af56:abb1:c7::/96 2a0a:8dc0:509b:21::/96


slnat bri1 add 7607:af56:ff8:d12::/96 2607:f8f8:631:d601:2000:d12::/96
slnat bri1 add 7607:af56:abb1:c7::/96 2a0a:8dc0:509b:21::/96

#./src/slnat add-batch /etc/slick-nat/routes