#!/bin/sh

base_path="/dev/shm/replay"
fragments_path="/dev/shm/replay/fragments"
ignore_recent_chunks=2
long_chunks=20
short_chunks=10

short_replay_file=$base_path/replay_short.h264
long_replay_file=$base_path/replay_long.h264

long_fragments=`ls -tr $fragments_path/out*.h264 | head -n-$ignore_recent_chunks | tail -n$long_chunks`
short_fragments=`echo "$long_fragments" | tail -n$short_chunks`

generate_video() {
  if [ -n "$fragments" ]; then
    cat $fragments > $replay_file
  fi
  #cp $replay_file /media/pi/FOOSDRIVE/`date +"%Y%m%d_%H%M%S"`.h264
}

# generate short replay
replay_file=$short_replay_file
fragments=$short_fragments
generate_video

# generate long replay
replay_file=$long_replay_file
fragments=$long_fragments
generate_video
