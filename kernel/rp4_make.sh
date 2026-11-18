#!/usr/bin/env bash

# This will build kernel module which will work versus flash connected to external rp4
# header. Since rp4 DT sets by default very high frequency values on empty interfaces we need 
# to downgrade the frequency so it fits to chip 
make WORKING_TARGET_SPI_SPEED=5000000
