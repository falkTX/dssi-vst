#!/bin/sh
dll="$1"
test -f "$dll" || exit 1
if file "$dll" | grep -qi "MS-DOS executable" && strings "$dll" | grep -qi vst; then
    bn=`basename "$dll"`
    if Xdialog --default-no --yesno "WARNING: About to load DLL \"$bn\" as a JACK client.\n\nThis DLL looks like a VST plugin, but it's\nimpossible to tell what it will actually do.\nRunning it could damage your system.\n\nDo you want to go ahead and run it anyway?" 15 70; then
	exec setsid vsthost "$dll"
    fi
fi

