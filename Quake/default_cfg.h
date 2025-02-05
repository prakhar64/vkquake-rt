// keep in sync with Misc/qs_pak/default.cfg

static char default_cfg[] =
	"unbindall\n"

	"bind MOUSE1 +attack\n"

	"bind w +forward\n"
	"bind s +back\n"
	"bind a +moveleft\n"
	"bind d +moveright\n"

	"bind SPACE +jump\n"
	"bind SHIFT +speed\n"
	"bind CTRL +movedown\n"

	"bind TAB +showscores\n"

    "bind f rt_pfnswitch\n"

	"bind 1 \"impulse 1\"\n"
	"bind 2 \"impulse 2\"\n"
	"bind 3 \"impulse 3\"\n"
	"bind 4 \"impulse 4\"\n"
	"bind 5 \"impulse 5\"\n"
	"bind 6 \"impulse 6\"\n"
	"bind 7 \"impulse 7\"\n"
	"bind 8 \"impulse 8\"\n"

	"bind 0 \"impulse 0\"\n"

	"bind / \"impulse 10\"\n"
	"bind MWHEELDOWN \"impulse 10\"\n"
	"bind MWHEELUP \"impulse 12\"\n"

	"alias zoom_in \"sensitivity 2;fov 90;wait;fov 70;wait;fov 50;wait;fov 30;wait;fov 10;wait;fov 5;bind F11 zoom_out\"\n"
	"alias zoom_out \"sensitivity 4;fov 5;wait;fov 10;wait;fov 30;wait;fov 50;wait;fov 70;wait;fov 90;bind F11 zoom_in; sensitivity 3\"\n"
	"bind F11 zoom_in\n"

	"bind F1 \"help\"\n"
	"bind F2 \"menu_save\"\n"
	"bind F3 \"menu_load\"\n"
	"bind F4 \"menu_options\"\n"
	"bind F5 \"menu_multiplayer\"\n"
	"bind F6 \"echo Quicksaving...; wait; save quick\"\n"
	"bind F9 \"echo Quickloading...; wait; load quick\"\n"

	"bind PAUSE \"pause\"\n"
	"bind ESCAPE \"togglemenu\"\n"
	"bind ~ \"toggleconsole\"\n"
	"bind ` \"toggleconsole\"\n"

	"bind t \"messagemode\"\n"

	"bind + \"sizeup\"\n"
	"bind = \"sizeup\"\n"
	"bind - \"sizedown\"\n"

	"bind LSHOULDER \"impulse 12\"\n"
	"bind RSHOULDER \"impulse 10\"\n"
	"bind LTRIGGER +jump\n"
	"bind RTRIGGER +attack\n"

	"gamma 1.0\n"
	"volume 0.7\n"
	"sensitivity 3\n"

	"viewsize 110\n"
	"scr_conscale 1.6\n"
	"scr_menuscale 1.6\n"
	"scr_sbarscale 1.6\n"

	"+mlook\n";
