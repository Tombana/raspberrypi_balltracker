#!/bin/sh

pkill player 2> /dev/null

./player "/dev/shm/replay/replay_short.h264" 20
