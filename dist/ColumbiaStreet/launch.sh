#!/bin/sh
# Columbia Street launcher for OnionOS. Mirrors Onion's own GMU launcher: take
# the audio device from audioserver and use Onion's MI_AO/MI_GFX-backed SDL 1.2.
cd "$(dirname "$0")"

echo performance > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
[ -f /mnt/SDCARD/.tmp_update/script/stop_audioserver.sh ] && . /mnt/SDCARD/.tmp_update/script/stop_audioserver.sh

touch /tmp/stay_awake
unset LD_PRELOAD
export LD_LIBRARY_PATH="/mnt/SDCARD/.tmp_update/lib/parasyte:/lib:/config/lib:/mnt/SDCARD/miyoo/lib:/mnt/SDCARD/.tmp_update/lib:$LD_LIBRARY_PATH"

# Optional settings, before ./columbia (for example CS_BUFFER=2048 ./columbia):
#   CS_BUFFER=2048  bigger audio buffers if you hear crackle (1024 by default)
#   CS_RATE=44100   a lower sample rate: a little less work
#   CS_UPDATE=0     don't check GitHub for a new version
#   CS_LINK=0       leave the USB port alone (no USB link to a leading Brick)
# exit status 75: the app has just updated itself, so start the new version
while :; do
    ./columbia > log.txt 2>&1
    [ $? -eq 75 ] || break
done

rm -f /tmp/stay_awake
