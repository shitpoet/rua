#!/bin/bash
dev=`nmcli -t -f DEVICE,NAME con show --active | grep p2 | cut -d':' -f 1`
#echo $dev
nice -n -11 ./rua $dev 192.168.43.1

