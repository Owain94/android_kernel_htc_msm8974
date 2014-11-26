#!/system/bin/sh
# Copyright (c) 2014, Savoca <adeddo27@gmail.com>
# Copyright (c) 2009-2014, The Linux Foundation. All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are met:
#     * Redistributions of source code must retain the above copyright
#       notice, this list of conditions and the following disclaimer.
#     * Redistributions in binary form must reproduce the above copyright
#       notice, this list of conditions and the following disclaimer in the
#       documentation and/or other materials provided with the distribution.
#     * Neither the name of The Linux Foundation nor
#       the names of its contributors may be used to endorse or promote
#       products derived from this software without specific prior written
#       permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
# AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
# NON-INFRINGEMENT ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
# CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
# EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
# PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
# OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
# WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
# OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
# ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#

DOM_LOGFILE="/data/local/tmp/dom.log"

#Find PVS bin
PVS="`cat /sys/module/clock_krait_8974/parameters/pvs_number`"
echo PVS: $PVS >> $DOM_LOGFILE;

# Set TCP westwood
if [ -e /proc/sys/net/ipv4/tcp_congestion_control ]; then
	echo "westwood" > /proc/sys/net/ipv4/tcp_congestion_control
	echo "[kernel] TCP set: westwood" | tee -a $DOM_LOGFILE;
else
	echo "[kernel] are you network hi" | tee -a $DOM_LOGFILE;
fi

# KSM
if [ -e /sys/kernel/mm/ksm/run ]; then
	echo "250" > /sys/kernel/mm/ksm/pages_to_scan
	echo "1200" > /sys/kernel/mm/ksm/sleep_millisecs
	echo "1" >  /sys/kernel/mm/ksm/run
else
	echo "[kernel] Failed to set KSM" | tee -a $DOM_LOGFILE;
fi

# Screen off
if [ -e /sys/devices/system/cpu/cpu0/cpufreq/screen_off_max ]; then
	echo "1" > /sys/devices/system/cpu/cpu0/cpufreq/screen_off_max
else
	echo "[kernel] Failed to set screen off max" | tee -a $DOM_LOGFILE;
fi

echo 1 > /sys/devices/system/cpu/cpu0/cpufreq/screen_off_max

# Enable powersuspend
if [ -e /sys/kernel/power_suspend/power_suspend_mode ]; then
	echo "1" > /sys/kernel/power_suspend/power_suspend_mode
	echo "[kernel] Powersuspend enabled" | tee -a $DOM_LOGFILE;
else
	echo "[kernel] Failed to set powersuspend" | tee -a $DOM_LOGFILE;
fi

# Set RGB KCAL
if [ -e /sys/devices/platform/kcal_ctrl.0/kcal ]; then
	sd_r=255
	sd_g=255
	sd_b=255
	kcal="$sd_r $sd_g $sd_b"
	echo "$kcal" > /sys/devices/platform/kcal_ctrl.0/kcal
	echo "1" > /sys/devices/platform/kcal_ctrl.0/kcal_ctrl
	echo "[kernel] LCD_KCAL: red=[$sd_r], green=[$sd_g], blue=[$sd_b]" | tee -a $DOM_LOGFILE;
fi

# Disable MPD, enable intelliplug
if [ -e /sys/module/intelli_plug/parameters/intelli_plug_active ]; then
	stop mpdecision
	echo "1" > /sys/module/intelli_plug/parameters/intelli_plug_active
	echo "[furnace] IntelliPlug enabled" | tee /dev/kmsg
else
	echo "[furnace] IntelliPlug not found, using MPDecision" | tee /dev/kmsg
	start mpdecision
fi

# Freq
if [ -e /sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq ]; then
	echo "2265600" > /sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq
	echo "2265600" > /sys/devices/system/cpu/cpu1/cpufreq/scaling_max_freq
	echo "2265600" > /sys/devices/system/cpu/cpu2/cpufreq/scaling_max_freq
	echo "2265600" > /sys/devices/system/cpu/cpu3/cpufreq/scaling_max_freq
	echo "[kernel] Max freq set: 2265600" | tee -a $DOM_LOGFILE;
else
	echo "[kernel] Call the police!" | tee -a $DOM_LOGFILE;
fi

# BacklightDimmer
if [ -e /sys/backlight_dimmer/backlight_dimmer; ]; then
	echo "1" > /sys/backlight_dimmer/backlight_dimmer;
	echo "[kernel] Backlight dimmer enabled"  | tee -a $DOM_LOGFILE;
else
	echo "[kernel] Failed to set backlight dimmer!" | tee -a $DOM_LOGFILE;
fi

# CpuBoost
if [ -e /sys/module/cpu_boost/parameters/boost_ms ]; then
	echo "20" > /sys/module/cpu_boost/parameters/boost_ms;
	echo "1190400" > /sys/module/cpu_boost/parameters/sync_threshold;
	echo "1190400" > /sys/module/cpu_boost/parameters/input_boost_freq;
	echo "40" > /sys/module/cpu_boost/parameters/input_boost_ms;
	echo "[kernel] Cpuboost set"  | tee -a $DOM_LOGFILE;
else
	echo "[kernel] Failed to set cpuboost!" | tee -a $DOM_LOGFILE;
fi

fstrim -v /cache | tee -a $DOM_LOGFILE;
fstrim -v /data | tee -a $DOM_LOGFILE;