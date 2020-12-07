#!/bin/bash
export LIBRARY_PATH=/usr/lib64/mysql:$LIBRARY_PATH
export LD_LIBRARY_PATH=/usr/lib64/mysql:$LD_LIBRARY_PATH

make server
